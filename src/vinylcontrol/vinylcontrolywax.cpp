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
    QString strVinylSpeed = m_pConfig->getValueString(
            ConfigKey(group,"vinylcontrol_speed_type"));

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
    ScopedTimer t("VinylControlYwax::analyzeSamples");

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
    CSAMPLE pSamplesWithGain[nFrames];
    memcpy(pSamplesWithGain, pSamples, nFrames);

    // Applying gain, but limiting to [-1, 1] range
    for (int i = 0; i < static_cast<int>(samplesSize); ++i) {
        pSamplesWithGain[i] = CSAMPLE_clamp(pSamples[i] * gain);
    }

    bool bHaveSignal = ywax->submitPcmData(pSamplesWithGain, nFrames);

    float dVinylPitch;
    if (!ywax->getPitch(dVinylPitch)) {
        // The PLL did not succeed retrieving the pitch
        return;
    }
    qDebug() << "Pitch: " << dVinylPitch;    
    // TODO: report quality of phase sync


    // Has a new track been loaded? Currently we use track duration which is
    // integer seconds in the song. However, for calculations we need the
    // higher-accuracy duration found by dividing the track samples by the
    // samplerate.
    // TODO(XXX): we should really sync on all track changes
    // TODO(rryan): Should we calculate the true duration to check if it
    // changed?  It's just an extra division by trackSampleRate.
    double duration_inaccurate = duration->get();
    if (duration_inaccurate != m_dOldDurationInaccurate) {
        m_bForceResync = true;
        m_bTrackSelectMode = false; //just in case
        m_dOldDurationInaccurate = duration_inaccurate;
        m_dOldDuration = trackSamples->get() / 2 / trackSampleRate->get();

        // we were at record end, so turn it off and restore mode
        if(m_bAtRecordEnd) {
            disableRecordEndMode();
            if (m_iOldVCMode == MIXXX_VCMODE_CONSTANT) {
                m_iVCMode = MIXXX_VCMODE_RELATIVE;
            } else {
                m_iVCMode = m_iOldVCMode;
            }
        }
    }

    // make sure m_dVinylPosition only has good values
    if (m_iPosition != -1) {
        m_dVinylPosition = static_cast<double>(m_iPosition) / 1000.0 - m_iLeadInTime;
    }

    // Initialize drift control to zero in case we don't get any position data
    // to calculate it with.
    double dDriftControl = 0.0;

    // Get the playback position in the file in seconds.
    double filePosition = playPos->get() * m_dOldDuration;

    int reportedMode = static_cast<int>(mode->get());
    bool reportedPlayButton = playButton->toBool();

    if (m_iVCMode != reportedMode) {
        //if we are playing, don't allow change
        //to absolute mode (would cause sudden track skip)
        if (reportedPlayButton && reportedMode == MIXXX_VCMODE_ABSOLUTE) {
            m_iVCMode = MIXXX_VCMODE_RELATIVE;
            mode->slotSet((double)m_iVCMode);
        } else {
            // go ahead and switch
            m_iVCMode = reportedMode;
            if (reportedMode == MIXXX_VCMODE_ABSOLUTE) {
                m_bForceResync = true;
            }
        }

        //if we are out of error mode...
        if (vinylStatus->get() == VINYL_STATUS_ERROR &&
            m_iVCMode == MIXXX_VCMODE_RELATIVE) {
            vinylStatus->slotSet(VINYL_STATUS_OK);
        }
    }

    //if looping has been enabled, don't allow absolute mode
    if (loopEnabled->toBool() && m_iVCMode == MIXXX_VCMODE_ABSOLUTE) {
        m_iVCMode = MIXXX_VCMODE_RELATIVE;
        mode->slotSet((double)m_iVCMode);
    }

    // Don't allow cueing mode to be enabled in absolute mode.
    if (m_iVCMode == MIXXX_VCMODE_ABSOLUTE &&
        cueing->get() != MIXXX_RELATIVE_CUE_OFF) {
        cueing->set(MIXXX_RELATIVE_CUE_OFF);
    }

    //are we newly playing near the end of the record?  (in absolute mode, this happens
    //when the filepos is past safe (more accurate),
    //but it can also happen in relative mode if the vinylpos is nearing the end
    //If so, change to constant mode so DJ can move the needle safely

    if (!m_bAtRecordEnd && reportedPlayButton) {
        if (m_iVCMode == MIXXX_VCMODE_ABSOLUTE &&
            (filePosition + m_iLeadInTime) * 1000.0 > m_uiSafeZone &&
            !m_bForceResync) {
            // corner case: we are waiting for resync so don't enable just yet
            enableRecordEndMode();
        } else if (m_iVCMode != MIXXX_VCMODE_ABSOLUTE &&
                   m_iPosition != -1 &&
                   m_iPosition > static_cast<int>(m_uiSafeZone)) {
            enableRecordEndMode();
        }
    }

    if (m_bAtRecordEnd) {
        //if m_bAtRecordEnd was true, maybe it no longer applies:

        if (!reportedPlayButton) {
            //if we turned off play button, also disable
            disableRecordEndMode();
        } else if (m_iPosition != -1 &&
                   m_iPosition <= static_cast<int>(m_uiSafeZone) &&
                   m_dVinylPosition > 0 &&
                   checkSteadyPitch(dVinylPitch, filePosition) > 0.5) {
            //if good position, and safe, and not in leadin, and steady,
            //disable
            disableRecordEndMode();
        }

        if (m_bAtRecordEnd) {
            //ok, it's still valid, blink
            if ((reportedPlayButton && (int)(filePosition * 2.0) % 2) ||
                (!reportedPlayButton && (int)(m_iPosition / 500.0) % 2))
                vinylStatus->slotSet(VINYL_STATUS_WARNING);
            else
                vinylStatus->slotSet(VINYL_STATUS_DISABLED);
        }
    }

    //check here for position > safe, and if no record end mode,
    //then trigger track selection mode.  just pass position to it
    //and ignore pitch

    if (!m_bAtRecordEnd) {
        if (m_iPosition != -1 && m_iPosition > static_cast<int>(m_uiSafeZone)) {
            //only enable if pitch is steady, though.  Heavy scratching can
            //produce crazy results and trigger this mode
            if (m_bTrackSelectMode || checkSteadyPitch(dVinylPitch, filePosition) > 0.1) {
                //until I can figure out how to detect "track 2" on serato CD,
                //don't try track selection
                if (!m_bCDControl) {
                    if (!m_bTrackSelectMode) {
                        qDebug() << "position greater than safe, select mode" << m_iPosition << m_uiSafeZone;
                        m_bTrackSelectMode = true;
                        togglePlayButton(false);
                        resetSteadyPitch(0.0, 0.0);
                        m_pVCRate->set(0.0);
                    }
                    doTrackSelection(true, dVinylPitch, m_iPosition);
                }

                //hm I wonder if track will keep playing while this happens?
                //not sure what we want to do here...  probably enforce
                //stopped deck.

                //but if constant mode...  nah, force stop.
                return;
            }
            //if it's not steady yet we process as normal
        } else {
            //so we're not unsafe.... but
            //if no position, but we were in select mode, do select mode
            if (m_iPosition == -1 && m_bTrackSelectMode) {
                //qDebug() << "no position, but were in select mode";
                doTrackSelection(false, dVinylPitch, m_iPosition);

                //again, force stop?
                return;
            } else if (m_bTrackSelectMode) {
                //qDebug() << "discontinuing select mode, selecting track";
                if (m_pControlTrackLoader == NULL) {
                    m_pControlTrackLoader = new ControlProxy(
                            m_group, "LoadSelectedTrack", this);
                }

                m_pControlTrackLoader->slotSet(1.0);
                m_pControlTrackLoader->slotSet(0.0); // I think I have to do this...

                // if position is known and safe then no track select mode
                m_bTrackSelectMode = false;
            }
        }
    }

    if (m_iVCMode == MIXXX_VCMODE_CONSTANT) {
        // when we enabled constant mode we set the rate slider
        // now we just either set scratch val to 0 (stops playback)
        // or 1 (plays back at that rate)

        double newScratch = reportedPlayButton ? m_pRateRatio->get() : 0.0;
        m_pVCRate->set(newScratch);

        // is there any reason we'd need to do anything else?
        return;
    }

    // CONSTANT MODE NO LONGER APPLIES...

    // When there's a timecode signal available
    // This is set when we analyze samples (no need for lock I think)
    if(bHaveSignal) {
        //POSITION: MAYBE  PITCH: YES

        //We have pitch, but not position.  so okay signal but not great (scratching / cueing?)
        //qDebug() << "Pitch" << dVinylPitch;

        if (m_iPosition != -1) {
            //POSITION: YES  PITCH: YES
            //add a value to the pitch ring (for averaging / smoothing the pitch)
            //qDebug() << fabs(((m_dVinylPosition - m_dVinylPositionOld) * (dVinylPitch / fabs(dVinylPitch))));

            bool reversed = static_cast<bool>(reverseButton->get());
            if (!reversed && m_bWasReversed) {
                resetSteadyPitch(dVinylPitch, m_dVinylPosition);
            }
            m_bWasReversed = reversed;

            //save the absolute amount of drift for when we need to estimate vinyl position
            m_dDriftAmt = m_dVinylPosition - filePosition;

            //qDebug() << "drift" << m_dDriftAmt;

            if (m_bForceResync) {
                //if forceresync was set but we're no longer absolute,
                //it no longer applies
                //if we're in relative mode then we'll do a sync
                //because it might select a cue
                if (m_iVCMode == MIXXX_VCMODE_ABSOLUTE ||
                    (m_iVCMode == MIXXX_VCMODE_RELATIVE && cueing->toBool())) {
                    syncPosition();
                    resetSteadyPitch(dVinylPitch, m_dVinylPosition);
                }
                m_bForceResync = false;
            } else if (fabs(m_dVinylPosition - filePosition) > 0.1 &&
                       m_dVinylPosition < -2.0) {
                //At first I thought it was a bug to resync to leadin in relative mode,
                //but after using it that way it's actually pretty convenient.
                //qDebug() << "Vinyl leadin";
                syncPosition();
                resetSteadyPitch(dVinylPitch, m_dVinylPosition);
                if (uiUpdateTime(filePosition)) {
                    m_pRateRatio->set(fabs(dVinylPitch));
                }
            } else if (m_iVCMode == MIXXX_VCMODE_ABSOLUTE &&
                       (fabs(m_dVinylPosition - m_dVinylPositionOld) >= 5.0)) {
                //If the position from the timecode is more than a few seconds off, resync the position.
                //qDebug() << "resync position (>15.0 sec)";
                //qDebug() << m_dVinylPosition << m_dVinylPositionOld << m_dVinylPosition - m_dVinylPositionOld;
                syncPosition();
                resetSteadyPitch(dVinylPitch, m_dVinylPosition);
            } else if (m_iVCMode == MIXXX_VCMODE_ABSOLUTE && m_bCDControl &&
                       fabs(m_dVinylPosition - m_dVinylPositionOld) >= 0.1) {
                //qDebug() << "CDJ resync position (>0.1 sec)";
                syncPosition();
                resetSteadyPitch(dVinylPitch, m_dVinylPosition);
            } else if (playPos->get() >= 1.0 && dVinylPitch > 0) {
                //end of track, force stop
                togglePlayButton(false);
                resetSteadyPitch(0.0, 0.0);
                m_pVCRate->set(0.0);
                m_iPitchRingPos = 0;
                m_iPitchRingFilled = 0;
                return;
            } else {
                togglePlayButton(checkSteadyPitch(dVinylPitch, filePosition) > 0.5);
            }

            // Calculate how much the vinyl's position has drifted from it's timecode and compensate for it.
            // (This is caused by the manufacturing process of the vinyl.)
            if (m_iVCMode == MIXXX_VCMODE_ABSOLUTE &&
                fabs(m_dDriftAmt) > 0.1 && fabs(m_dDriftAmt) < 5.0) {
                dDriftControl = m_dDriftAmt * .01;
            } else {
                dDriftControl = 0.0;
            }

            m_dVinylPositionOld = m_dVinylPosition;
        } else {
            //POSITION: NO  PITCH: YES
            //if we don't have valid position, we're not playing so reset time to current
            //estimate vinyl position

            if (playPos->get() >= 1.0 && dVinylPitch > 0) {
                //end of track, force stop
                togglePlayButton(false);
                resetSteadyPitch(0.0, 0.0);
                m_pVCRate->set(0.0);
                m_iPitchRingPos = 0;
                m_iPitchRingFilled = 0;
                return;
            }

            if (m_iVCMode == MIXXX_VCMODE_ABSOLUTE &&
                fabs(dVinylPitch) < 0.05 &&
                fabs(m_dDriftAmt) >= 0.3) {
                //qDebug() << "slow, out of sync, syncing position";
                syncPosition();
            }

            m_dVinylPositionOld = filePosition + m_dDriftAmt;

            if (dVinylPitch > 0.2) {
                togglePlayButton(checkSteadyPitch(dVinylPitch, filePosition) > 0.5);
            }
        }

        //playbutton status may have changed
        reportedPlayButton = playButton->toBool();

        if (reportedPlayButton) {
            // Only add to the ring if pitch is stable
            m_pPitchRing[m_iPitchRingPos] = dVinylPitch;
            if (m_iPitchRingFilled < m_iPitchRingSize) {
                m_iPitchRingFilled++;
            }
            m_iPitchRingPos = (m_iPitchRingPos + 1) % m_iPitchRingSize;
        } else {
            // Reset ring if pitch isn't steady
            m_iPitchRingPos = 0;
            m_iPitchRingFilled = 0;
        }

        //only smooth when we have good position (no smoothing for scratching)
        double averagePitch = 0.0;
        if (m_iPosition != -1 && reportedPlayButton) {
            for (int i = 0; i < m_iPitchRingFilled; ++i) {
                averagePitch += m_pPitchRing[i];
            }
            averagePitch /= m_iPitchRingFilled;
            // Round out some of the noise
            averagePitch = round(averagePitch * 10000.0);
            averagePitch /= 10000.0;
        } else {
            averagePitch = dVinylPitch;
        }

        m_pVCRate->set(averagePitch + dDriftControl);
        if (uiUpdateTime(filePosition)) {
            double true_pitch = averagePitch + dDriftControl;
            double pitch_difference = true_pitch - m_dDisplayPitch;

            // The true pitch can show a misleading amount of variance --
            // differences of .1% or less can show up as 1 or 2 bpm changes.
            // Therefore we react slowly to bpm changes to show a more steady
            // number to the user.
            if (fabs(pitch_difference) > 0.5) {
                // For large changes in pitch (start/stop, usually), immediately
                // update the display.
                m_dDisplayPitch = true_pitch;
            } else if (fabs(pitch_difference) > 0.005) {
                // For medium changes in pitch, take 4 callback loops to
                // converge on the correct amount.
                m_dDisplayPitch += pitch_difference * .25;
            } else {
                // For extremely small changes, converge very slowly.
                m_dDisplayPitch += pitch_difference * .01;
            }
            // Don't show extremely high or low speeds in the UI.
            if (reportedPlayButton && !scratching->toBool() &&
                m_dDisplayPitch < 1.9 && m_dDisplayPitch > 0.2) {
                m_pRateRatio->set(m_dDisplayPitch);
            } else {
                m_pRateRatio->set(1.0);
            }
            m_dUiUpdateTime = filePosition;
        }

        m_dOldFilePos = filePosition;
    } else {
        // No pitch data available (the needle is up/stopped.... or *really*
        // crappy signal)

        //POSITION: NO  PITCH: NO
        //if it's been a long time, we're stopped.
        //if it hasn't been long,
        //let the track play a wee bit more before deciding we've stopped

        m_pRateRatio->set(1.0);

        if (m_iVCMode == MIXXX_VCMODE_ABSOLUTE &&
            fabs(m_dVinylPosition - filePosition) >= 0.1) {
            //qDebug() << "stopped, out of sync, syncing position";
            syncPosition();
        }

        if(fabs(filePosition - m_dOldFilePos) >= 0.1 ||
           filePosition == m_dOldFilePos) {
            //We are not playing any more
            togglePlayButton(false);
            resetSteadyPitch(0.0, 0.0);
            m_pVCRate->set(0.0);
            //resetSteadyPitch(dVinylPitch, filePosition);
            // Notify the UI that the timecode quality is garbage/missing.
            m_fTimecodeQuality = 0.0f;
            m_iPitchRingPos = 0;
            m_iPitchRingFilled = 0;
            m_iQualPos = 0;
            m_iQualFilled = 0;
            m_bForceResync = true;
            vinylStatus->slotSet(VINYL_STATUS_OK);
        }
    }
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

        togglePlayButton(playButton->toBool() || fabs(m_pVCRate->get()) > 0.05);
        m_pVCRate->set(m_pRateRatio->get());
        resetSteadyPitch(0.0, 0.0);
        m_bForceResync = true;
        if (!was) {
            m_dOldFilePos = 0.0;
        }
        m_iVCMode = static_cast<int>(mode->get());
        m_bAtRecordEnd = false;
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
