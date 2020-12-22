#include "ywax.h"

using namespace std::complex_literals;

Ywax::Ywax(VinylType vinylType, int sampleRate, float rpmNominal) :
  m_sampleRate(sampleRate),
  m_rpmNominal(rpmNominal),
  phaseError(M_PI),
  phaseErrorAverage(M_PI),
  phaseEstimate(0.0),
  freqEstimate (0.0) {
    m_vinylSettings = allVinylSettings.at(vinylType);
}

bool Ywax::submitPcmData(float *pcm, size_t npcm) {
    bool bHaveSignal = pcm[0]*pcm[0] + pcm[1]*pcm[1] > kMinSignal; // 2-Norm of stereo sample
    if (!bHaveSignal) {
        return false; // No signal yet
        // TODO: trigger state transition directly to no phase sync
    }

    float left, right, primary, secondary;
    while (npcm--) {

        left = pcm[0];
        right = pcm[1];

        if (m_vinylSettings.flags & SWITCH_PRIMARY) {
            primary = left;
            secondary = right;
        } else {
            primary = right;
            secondary = left;
        }

        std::complex<float> complexSample(primary, secondary);

        processSample(complexSample);

        pcm += TIMECODER_CHANNELS;
    }

    return true;
}

bool Ywax::processSample(std::complex<float> sample) {
    return updatePll(sample);
}

bool Ywax::updatePll(std::complex<float> sample) {
    std::complex<float> ref_signal = static_cast<std::complex<float>>(
        std::exp(1i * static_cast<double>(phaseEstimate)));
    phaseError = std::arg(sample * std::conj(ref_signal));
    phaseEstimate += PLL_ALPHA * phaseError;
    freqEstimate += PLL_BETA * phaseError;

    phaseEstimate += freqEstimate; // Forward phase for next cycle

    phaseErrorAverage = phaseError * runningAverageScaling
            + phaseErrorAverage * (1. - runningAverageScaling);
    return true;
}

bool Ywax::getToneFreq(float &toneFreq_Hz) {
    if (phaseErrorAverage > PLL_PHASE_ERROR_THRES) {
        // If too much phase estimation error
        return false;
    }
    toneFreq_Hz = freqEstimate * m_sampleRate / (2 * M_PI);
    return true;
}

bool Ywax::getPitch(double &pitch) {
    float currentToneFreq;
    if (!getToneFreq(currentToneFreq)) {
        return false;
    }

    pitch = currentToneFreq / m_vinylSettings.toneFreq;
    return true;
}

bool Ywax::getRevPerSecond(float &rps) {
    double pitch;
    if (!getPitch(pitch)) {
        return false;
    }
    rps = m_rpmNominal * pitch / 60.0f;
    return true;
}

int Ywax::getPosition() {
    // TODO: implement absolute mode :)
    return -1;
}
