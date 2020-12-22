#include "ywax.h"

using namespace std::complex_literals;

Ywax::Ywax(VinylType vinylType, int sampleRate, double rpmNominal) :
  m_sampleRate(sampleRate),
  m_rpmNominal(rpmNominal),
  phaseError(M_PI),
  phaseErrorAverage(M_PI),
  phaseEstimate(0.0),
  freqEstimate(0.0),
  sampleNormSquaredAverage(0.0){
    m_vinylSettings = allVinylSettings.at(vinylType);
    levelDetectionScaling = 1. / (levelDetectionWindow * sampleRate / m_vinylSettings.toneFreq + 1);
}

bool Ywax::submitPcmData(float *pcm, size_t npcm) { // 2-Norm of stereo sample

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

        cplex complexSample(primary, secondary);

        // Updating running average of square norm
        sampleNormSquaredAverage = std::norm(complexSample) * levelDetectionScaling
                + sampleNormSquaredAverage * (1. - levelDetectionScaling);

        processSample(complexSample);

        pcm += TIMECODER_CHANNELS;
    }

    if (sampleNormSquaredAverage < kMinSignal) {
        // Level is too low, resetting PLL
        sampleNormSquaredAverage = 0.0;
        resetPll();
        return false;
    }

    return true;
}

bool Ywax::processSample(cplex sample) {
    return updatePll(sample);
}

void Ywax::resetPll() {
    phaseError = M_PI;
    phaseErrorAverage = M_PI;
    phaseEstimate = 0.0;
    freqEstimate = 0.0;
}

bool Ywax::updatePll(cplex sample) {
    cplex ref_signal = static_cast<cplex>(
        std::exp(1i * static_cast<double>(phaseEstimate)));
    phaseError = std::arg(sample * std::conj(ref_signal));
    phaseEstimate += PLL_ALPHA * phaseError;
    freqEstimate += PLL_BETA * phaseError;

    phaseEstimate += freqEstimate; // Forward phase for next cycle

    phaseErrorAverage = phaseError * phaseRunningAverageScaling
            + phaseErrorAverage * (1. - phaseRunningAverageScaling);
    return true;
}

bool Ywax::getToneFreq(double &toneFreq_Hz) {
    if (phaseErrorAverage > PLL_PHASE_ERROR_THRES) {
        // If too much phase estimation error
        return false;
    }
    toneFreq_Hz = freqEstimate * m_sampleRate / (2 * M_PI);
    return true;
}

bool Ywax::getPitch(double &pitch) {
    double currentToneFreq;
    if (!getToneFreq(currentToneFreq)) {
        return false;
    }

    pitch = currentToneFreq / m_vinylSettings.toneFreq;
    return true;
}

bool Ywax::getRevPerSecond(double &rps) {
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
