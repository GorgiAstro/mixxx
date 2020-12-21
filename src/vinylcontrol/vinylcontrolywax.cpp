#include <QtDebug>
#include "control/controlproxy.h"
#include "control/controlobject.h"
#include "util/defs.h"
#include "util/math.h"
#include "util/timer.h"
#include "vinylcontrolywax.h"

namespace {
constexpr int kChannels = 2;
}

VinylControlYwax::VinylControlYwax(UserSettingsPointer pConfig, const QString& group)
        : VinylControl(pConfig, group) {

    // TODO(rryan): Should probably live in VinylControlManager since it's not
    // specific to a VC deck.
    signalenabled->slotSet(m_pConfig->getValueString(
            ConfigKey(VINYL_PREF_KEY, "show_signal_quality")).toInt());

    // Get the vinyl type and speed.
    QString strVinylType = m_pConfig->getValueString(
            ConfigKey(group,"vinylcontrol_vinyl_type"));

    int iSampleRate = m_pConfig->getValueString(
            ConfigKey("[Soundcard]","Samplerate")).toULong();

    qDebug() << "Ywax Vinyl control starting with a sample rate of:" << iSampleRate;

    Ywax::VinylType vinylType;
    // Mapping the settings string to an Ywax enum
    if (strVinylType == MIXXX_VINYL_MIXVIBESDVS) {
        vinylType = Ywax::VinylType::mixvibes_v2;
    } else if (strVinylType == MIXXX_VINYL_SERATOCV02VINYLSIDEA) {
        vinylType = Ywax::VinylType::serato_2a;
    } else if (strVinylType == MIXXX_VINYL_SERATOCV02VINYLSIDEB) {
        vinylType = Ywax::VinylType::serato_2b;
    } else if (strVinylType == MIXXX_VINYL_SERATOCD) {
        vinylType = Ywax::VinylType::serato_cd;
    } else if (strVinylType == MIXXX_VINYL_TRAKTORSCRATCHSIDEA) {
        vinylType = Ywax::VinylType::traktor_a;
    } else if (strVinylType == MIXXX_VINYL_TRAKTORSCRATCHSIDEB) {
        vinylType = Ywax::VinylType::traktor_b;
    } else if (strVinylType == MIXXX_VINYL_TRAKTORSCRATCHMK2SIDEA) {
        vinylType = Ywax::VinylType::traktor_mk2_a;
    } else if (strVinylType == MIXXX_VINYL_TRAKTORSCRATCHMK2SIDEB) {
        vinylType = Ywax::VinylType::traktor_mk2_b;
    } else {
        // TODO: throw exception, but defaulting to Serato for now
        vinylType = Ywax::VinylType::serato_2a;
    }
    ywax = new Ywax(vinylType, iSampleRate);

    qDebug() << "Starting vinyl control ywax thread";
}

VinylControlYwax::~VinylControlYwax() {
    m_pVCRate->set(0.0);
    delete ywax;
}

void VinylControlYwax::analyzeSamples(CSAMPLE* pSamples, size_t nFrames) {

    //TODO: Move all these config object get*() calls to an "updatePrefs()" function,
    //        and make that get called when any options get changed in the preferences dialog, rather than
    //        polling everytime we get a buffer.


    // Check if vinyl control is enabled...
    m_bIsEnabled = enabled && checkEnabled(m_bIsEnabled, enabled->toBool());


    //are we even playing and enabled at all?
    if (!m_bIsEnabled) {
        return;
    }

    auto gain = static_cast<CSAMPLE_GAIN>(m_pVinylControlInputGain->get());

    // We only support amplifying with the VC pre-amp.
    if (gain < 1.0f) {
        gain = 1.0f;
    }

    size_t samplesSize = nFrames * kChannels;
    CSAMPLE pSamplesWithGain[samplesSize];

    // Applying gain, but limiting to [-1, 1] range
    for (int i = 0; i < static_cast<int>(samplesSize); ++i) {
        pSamplesWithGain[i] = CSAMPLE_clamp(pSamples[i] * gain);
    }

    bool bHaveSignal = ywax->submitPcmData(pSamplesWithGain, nFrames);

    if (!bHaveSignal) {
        qDebug() << "Not enough signal";
        return;
    }

    qDebug() << "Phase error: " << ywax->getPhaseError();

    float dVinylPitch;
    if (!ywax->getPitch(dVinylPitch)) {
        // The PLL did not succeed retrieving the pitch
        qDebug() << "Invalid Pitch";
        return;
    }

    float toneFreq_Hz;
    ywax->getToneFreq(toneFreq_Hz);
    qDebug() << "Pitch: " << dVinylPitch << ", tone: " << toneFreq_Hz;
    // TODO: report quality of phase sync
}

bool VinylControlYwax::checkEnabled(bool was, bool is) {
    // if we're not enabled, but the last object was, try turning ourselves on
    // XXX: is this just a race that's working right now?
    if (!is && wantenabled->get() > 0) {
        enabled->slotSet(true);
        wantenabled->slotSet(false); //don't try to do this over and over
        return true; //optimism!
    }

    if (was != is) {
        // we reset the scratch value, but we don't reset the rate slider.
        // This means if we are playing, and we disable vinyl control,
        // the track will keep playing at the previous rate.
        // This allows for single-deck control, dj handoffs, etc.

        /*togglePlayButton(playButton->toBool() || fabs(m_pVCRate->get()) > 0.05);
        m_pVCRate->set(m_pRateRatio->get());
        resetSteadyPitch(0.0, 0.0);
        m_bForceResync = true;
        if (!was) {
            m_dOldFilePos = 0.0;
        }
        m_iVCMode = static_cast<int>(mode->get());
        m_bAtRecordEnd = false;*/
    }

    if (is && !was) {
        vinylStatus->slotSet(VINYL_STATUS_OK);
    } else if (!is) {
        vinylStatus->slotSet(VINYL_STATUS_DISABLED);
    }

    return is;
}

bool VinylControlYwax::writeQualityReport(VinylSignalQualityReport* pReport) {
    if (pReport) {
        pReport->timecode_quality = m_fTimecodeQuality;
        pReport->angle = getAngle();
        // TODO: show scope
        //memcpy(pReport->scope, timecoder.mon, sizeof(pReport->scope));
        return true;
    }
    return false;
}

float VinylControlYwax::getAngle() {
    /*float pos = timecoder_get_position(&timecoder, NULL);

    if (pos == -1) {
        return -1.0;
    }

    const auto rps = static_cast<float>(timecoder_revs_per_sec(&timecoder));
    // Invert angle to make vinyl spin direction correct.
    return 360 - (static_cast<int>(pos / 1000.0f * 360.0f * rps) % 360);*/

    return 0.0;
}
