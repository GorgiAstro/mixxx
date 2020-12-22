#pragma once

#include <stddef.h>
#include <stdint.h>
#include <map>
#include <complex>
#include <cmath>

class Ywax
{
public:
    enum VinylType {
        serato_2a = 0,
        serato_2b,
        serato_cd,
        traktor_a,
        traktor_b,
        traktor_mk2_a,
        traktor_mk2_b,
        mixvibes_v2,
        mixvibes_7inch
    };

    Ywax(VinylType vinylType, int sampleRate, float rpmNominal);

    /**
     * @brief submitPcmData submit stereo data to the decoder.
     * @param pcm PCM audio data in float. Must be normalized to [-1, 1]
     * @param npcm number of stereo frames, i.e. 2 times the length of pcm
     * @return true if signal strength was high enough
     */
    bool submitPcmData(float* pcm, size_t npcm);

    /**
     * @brief getToneFreq return the detected tone frequency in Hz
     * @param[out] toneFreq_Hz the currently detected tone frequency in Hz
     * @return true if PLL is successful, false otherwise
     */
    bool getToneFreq(float &toneFreq_Hz);

    /**
     * @brief getPitch Gives current pitch
     * @param[out] pitch current pitch
     * @return true if PLL is healthy, based on the phase error
     */
    bool getPitch(double &pitch);

    float getPhaseError() {
        return phaseErrorAverage;
    }

    int getPosition();

    bool getRevPerSecond(float &rps);

private:
    struct VinylSettings {
        uint16_t toneFreq; // Tone frequency in Hertz
        int flags;
    };

    static constexpr int TIMECODER_CHANNELS = 2;
    static constexpr int SWITCH_PHASE  = 0x1; /* tone phase difference of 270 (not 90) degrees */
    static constexpr int SWITCH_PRIMARY = 0x2; /* use left channel (not right) as primary */
    static constexpr int SWITCH_POLARITY = 0x4; /* read bit values in negative (not positive) */

    const std::map <VinylType, VinylSettings> allVinylSettings {
        {
            serato_2a,
            {
                .toneFreq = 1000,
                .flags = 0,
            }
        },
        {
            serato_2b,
            {
                .toneFreq = 1000,
                .flags = 0,
            }
        },
        {
            serato_cd,
            {
                .toneFreq = 1000,
                .flags = 0,
            }
        },
        {
            traktor_a,
            {
                .toneFreq = 2000,
                .flags = SWITCH_PRIMARY | SWITCH_POLARITY | SWITCH_PHASE,
            }
        },
        {
            traktor_b,
            {
                .toneFreq = 2000,
                .flags = SWITCH_PRIMARY | SWITCH_POLARITY | SWITCH_PHASE,
            }
        },
        {
            traktor_mk2_a,
            {
                .toneFreq = 2500,
                .flags = SWITCH_POLARITY | SWITCH_PHASE,
            }
        },
        {
            traktor_mk2_b,
            {
                .toneFreq = 2500,
                .flags = SWITCH_POLARITY | SWITCH_PHASE,
            }
        },
        {
            mixvibes_v2,
            {
                .toneFreq = 1300,
                .flags = SWITCH_PHASE,
            }
        },
        {
            mixvibes_7inch,
            {
                .toneFreq = 1300,
                .flags = SWITCH_PHASE,
            }
        }
    };

    static constexpr float kMinSignal = 0.001; // Minimum squared norm required for input signal (normalized in [0-1] range)
    static constexpr float PLL_ALPHA = 0.02;
    static constexpr float PLL_BETA = 0.0002;
    static constexpr float PLL_PHASE_ERROR_THRES = M_PI / 36; // 5 degree
    static constexpr uint16_t phaseErrorAverageSteps = 100;
    static constexpr float phaseRunningAverageScaling = 1. / (phaseErrorAverageSteps + 1);
    static constexpr float levelDetectionWindow = 10.0; // Averaging window, counted in tone cycles. For input level detection

    /**
     * Settings defined in constructor
     */
    VinylSettings m_vinylSettings;
    int m_sampleRate; // in samples per second
    float m_rpmNominal; // Nominal vinyl rotation speed in rotations per minute
    float levelDetectionScaling; // Running average scaling factor for input level detection

    /**
     * Variables updated in decoding process
     */
    float phaseError; // in radians
    float phaseErrorAverage;
    float phaseEstimate;
    float freqEstimate; // in radians per sample, not Hz
    float sampleNormSquaredAverage;

    bool processSample(std::complex<float> sample);
    void resetPll();
    bool updatePll(std::complex<float> sample);
};
