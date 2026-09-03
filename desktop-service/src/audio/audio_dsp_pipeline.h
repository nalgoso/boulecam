#pragma once

#include <vector>
#include <string>
#include <mutex>
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace boulecam {

struct AudioDspConfig {
    bool beamformingEnabled = false;
    bool rnnoiseEnabled = true;
    bool gateEnabled = true;
    float gateThresholdDb = -45.0f;   // -60 to -15 dBFS
    bool compressorEnabled = true;
    float compThresholdDb = -20.0f;   // -35 to -5 dBFS
    float compRatio = 3.0f;           // 1.5 to 8.0:1
    float compMakeupDb = 3.0f;        // 0.0 to +18.0 dB
    bool limiterEnabled = true;
};

struct AudioMeterData {
    float prePeakDb = -100.0f;
    float preRmsDb = -100.0f;
    float postPeakDb = -100.0f;
    float postRmsDb = -100.0f;
};

class AudioDspPipeline {
public:
    AudioDspPipeline(int sampleRate = 48000);
    ~AudioDspPipeline();

    // Process incoming 16-bit LE PCM chunk. Output is also 16-bit LE PCM.
    void Process(const uint8_t* pcmIn, uint32_t inBytes, int inChannels, std::vector<uint8_t>& pcmOut, int& outChannels);

    void SetConfig(const AudioDspConfig& config);
    AudioDspConfig GetConfig() const;

    AudioMeterData GetMeters() const;

private:
    void ProcessBlock480(float* channel0, float* channel1, int channels);

    int m_sampleRate;
    AudioDspConfig m_config;
    mutable std::mutex m_configMutex;

    // Metering
    mutable std::mutex m_meterMutex;
    AudioMeterData m_meters;

    // Intermediate float buffer for 480-sample blocks (10ms @ 48kHz)
    std::vector<float> m_floatInCh0;
    std::vector<float> m_floatInCh1;

    // Dual-Mic Beamforming NLMS Adaptive Filter State
    static const int NLMS_TAPS = 64;
    float m_nlmsWeights[NLMS_TAPS];
    float m_nlmsBuffer[NLMS_TAPS];
    int m_nlmsIndex = 0;

    // Noise Gate State
    float m_gateEnvelope = 0.0f;
    float m_gateGain = 1.0f;

    // Compressor State
    float m_compEnvelope = 0.0f;
    float m_compGain = 1.0f;

    // Neural Denoiser (RNNoise / Spectral Model) State
    float m_denoiseState[64];
    float m_noiseEstimate[32];
    bool m_noiseInit = false;
};

} // namespace boulecam
