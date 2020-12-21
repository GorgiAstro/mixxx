#include "ywax.h"

using namespace std::complex_literals;

Ywax::Ywax(VinylType vinylType, int sampleRate) :
  m_sampleRate(sampleRate),
  phaseError(0.0),
  phaseErrorAverage(0.0),
  phaseEstimate(0.0),
  freqEstimate (0.0) {
    m_vinylSettings = allVinylSettings.at(vinylType);
}

bool Ywax::submitPcmData(float *pcm, size_t npcm) {
    bool bHaveSignal = pcm[0]*pcm[0] + pcm[1]*pcm[1] > kMinSignal; // 2-Norm of stereo sample
    if (!bHaveSignal) {
        return false; // No signal yet
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
    return true;
}

bool Ywax::getPitch(float &pitch) {
    // TODO: replace by average phase error
    if (phaseError > PLL_PHASE_ERROR_THRES) {
        // If too much phase estimation error
        return false;
    }
    float freq_hz = freqEstimate * m_sampleRate / (2 * M_PI);
    pitch = freq_hz / m_vinylSettings.toneFreq;
    return true;
}
