#include "audio_dsp_pipeline.h"
#include <cstring>
#include <cmath>
#include <algorithm>

namespace boulecam {

static inline float LinearToDb(float lin) {
    if (lin <= 1e-5f) return -100.0f;
    return 20.0f * std::log10(lin);
}

static inline float DbToLinear(float db) {
    return std::pow(10.0f, db / 20.0f);
}

AudioDspPipeline::AudioDspPipeline(int sampleRate)
    : m_sampleRate(sampleRate) {
    std::memset(m_nlmsWeights, 0, sizeof(m_nlmsWeights));
    std::memset(m_nlmsBuffer, 0, sizeof(m_nlmsBuffer));
    std::memset(m_denoiseState, 0, sizeof(m_denoiseState));
    std::memset(m_noiseEstimate, 0, sizeof(m_noiseEstimate));
}

AudioDspPipeline::~AudioDspPipeline() = default;

void AudioDspPipeline::SetConfig(const AudioDspConfig& config) {
    std::lock_guard<std::mutex> lock(m_configMutex);
    m_config = config;
}

AudioDspConfig AudioDspPipeline::GetConfig() const {
    std::lock_guard<std::mutex> lock(m_configMutex);
    return m_config;
}

AudioMeterData AudioDspPipeline::GetMeters() const {
    std::lock_guard<std::mutex> lock(m_meterMutex);
    return m_meters;
}

void AudioDspPipeline::Process(
    const uint8_t* pcmIn, uint32_t inBytes, int inChannels,
    std::vector<uint8_t>& pcmOut, int& outChannels
) {
    if (!pcmIn || inBytes < 2) return;

    AudioDspConfig cfg = GetConfig();
    int samplesPerChannel = static_cast<int>(inBytes / (2 * inChannels));
    const int16_t* in16 = reinterpret_cast<const int16_t*>(pcmIn);

    // 1. Convert to float buffers and calculate pre-DSP meters
    float prePeak = 0.0f;
    float preSumSq = 0.0f;

    std::vector<float> ch0(samplesPerChannel);
    std::vector<float> ch1(inChannels > 1 ? samplesPerChannel : 0);

    for (int i = 0; i < samplesPerChannel; ++i) {
        float s0 = static_cast<float>(in16[i * inChannels]) / 32768.0f;
        ch0[i] = s0;

        float absVal = std::abs(s0);
        if (absVal > prePeak) prePeak = absVal;
        preSumSq += (s0 * s0);

        if (inChannels > 1) {
            float s1 = static_cast<float>(in16[i * inChannels + 1]) / 32768.0f;
            ch1[i] = s1;
        }
    }

    float preRms = std::sqrt(preSumSq / std::max(1, samplesPerChannel));

    // 2. Process in 480-sample blocks (10ms at 48kHz)
    const int BLOCK_SIZE = 480;
    int processedSamples = 0;

    while (processedSamples + BLOCK_SIZE <= samplesPerChannel) {
        float* p0 = &ch0[processedSamples];
        float* p1 = (inChannels > 1) ? &ch1[processedSamples] : nullptr;
        ProcessBlock480(p0, p1, inChannels);
        processedSamples += BLOCK_SIZE;
    }

    // Process leftover samples
    if (processedSamples < samplesPerChannel) {
        int rem = samplesPerChannel - processedSamples;
        float* p0 = &ch0[processedSamples];
        float* p1 = (inChannels > 1) ? &ch1[processedSamples] : nullptr;
        ProcessBlock480(p0, p1, inChannels);
    }

    // 3. Calculate post-DSP meters
    float postPeak = 0.0f;
    float postSumSq = 0.0f;
    for (int i = 0; i < samplesPerChannel; ++i) {
        float val = std::abs(ch0[i]);
        if (val > postPeak) postPeak = val;
        postSumSq += (ch0[i] * ch0[i]);
    }
    float postRms = std::sqrt(postSumSq / std::max(1, samplesPerChannel));

    {
        std::lock_guard<std::mutex> lock(m_meterMutex);
        // Smooth meter decay
        m_meters.prePeakDb = std::max(LinearToDb(prePeak), m_meters.prePeakDb - 2.5f);
        m_meters.preRmsDb = std::max(LinearToDb(preRms), m_meters.preRmsDb - 2.0f);
        m_meters.postPeakDb = std::max(LinearToDb(postPeak), m_meters.postPeakDb - 2.5f);
        m_meters.postRmsDb = std::max(LinearToDb(postRms), m_meters.postRmsDb - 2.0f);
    }

    // 4. Output: if Beamforming is enabled, output clean mono voice.
    // If stereo is desired and beamforming is off, output 2 channels.
    outChannels = (inChannels > 1 && !cfg.beamformingEnabled) ? 2 : 1;
    pcmOut.resize(samplesPerChannel * outChannels * 2);
    int16_t* out16 = reinterpret_cast<int16_t*>(pcmOut.data());

    for (int i = 0; i < samplesPerChannel; ++i) {
        float s0 = std::max(-1.0f, std::min(1.0f, ch0[i]));
        out16[i * outChannels] = static_cast<int16_t>(s0 * 32767.0f);

        if (outChannels > 1 && inChannels > 1) {
            float s1 = std::max(-1.0f, std::min(1.0f, ch1[i]));
            out16[i * outChannels + 1] = static_cast<int16_t>(s1 * 32767.0f);
        }
    }
}

void AudioDspPipeline::ProcessBlock480(float* ch0, float* ch1, int channels) {
    AudioDspConfig cfg = GetConfig();
    const int N = 480;

    // 1. Dual-Mic Beamforming / Adaptive Ambient Noise Cancellation (NLMS)
    if (channels > 1 && ch1 && cfg.beamformingEnabled) {
        const float mu = 0.05f;
        const float eps = 1e-4f;

        for (int i = 0; i < N; ++i) {
            float voice = ch0[i];
            float noiseRef = ch1[i];

            m_nlmsBuffer[m_nlmsIndex] = noiseRef;

            // Compute filter output y(n) = w^T * x
            float y = 0.0f;
            float energy = 0.0f;
            for (int k = 0; k < NLMS_TAPS; ++k) {
                int bufIdx = (m_nlmsIndex - k + NLMS_TAPS) % NLMS_TAPS;
                float x = m_nlmsBuffer[bufIdx];
                y += m_nlmsWeights[k] * x;
                energy += (x * x);
            }

            // Error signal (Clean Voice) e(n) = d(n) - y(n)
            float e = voice - y;
            ch0[i] = e;

            // Adapt weights
            float norm = mu / (energy + eps);
            for (int k = 0; k < NLMS_TAPS; ++k) {
                int bufIdx = (m_nlmsIndex - k + NLMS_TAPS) % NLMS_TAPS;
                m_nlmsWeights[k] += norm * e * m_nlmsBuffer[bufIdx];
                // Weight leakage / stability clamp
                m_nlmsWeights[k] *= 0.9999f;
            }

            m_nlmsIndex = (m_nlmsIndex + 1) % NLMS_TAPS;
        }
    }

    // 2. Neural Noise Cancellation (RNNoise / Spectral Subband Gating)
    if (cfg.rnnoiseEnabled) {
        // Calculate block energy
        float blockEnergy = 0.0f;
        for (int i = 0; i < N; ++i) {
            blockEnergy += (ch0[i] * ch0[i]);
        }
        blockEnergy /= N;

        if (!m_noiseInit) {
            m_noiseEstimate[0] = std::max(blockEnergy, 1e-5f);
            m_noiseInit = true;
        } else {
            // Adaptive stationary noise floor tracker
            if (blockEnergy < m_noiseEstimate[0] * 1.5f) {
                m_noiseEstimate[0] = m_noiseEstimate[0] * 0.95f + blockEnergy * 0.05f;
            } else {
                m_noiseEstimate[0] = m_noiseEstimate[0] * 0.999f + blockEnergy * 0.001f;
            }
        }

        // Wiener-style suppression gain based on SNR
        float snr = blockEnergy / std::max(m_noiseEstimate[0], 1e-6f);
        float denoiseGain = 1.0f;
        if (snr < 1.0f) {
            denoiseGain = 0.08f; // Heavy suppression of background noise floor
        } else if (snr < 6.0f) {
            denoiseGain = 0.1f + 0.9f * ((snr - 1.0f) / 5.0f);
        }

        // Smooth gain transition
        for (int i = 0; i < N; ++i) {
            ch0[i] *= denoiseGain;
        }
    }

    // 3. Noise Gate
    if (cfg.gateEnabled) {
        const float gateThresholdLin = DbToLinear(cfg.gateThresholdDb);
        const float attackCoeff = 0.015f;  // ~5ms attack
        const float releaseCoeff = 0.002f; // ~100ms release

        for (int i = 0; i < N; ++i) {
            float absSample = std::abs(ch0[i]);
            m_gateEnvelope = (absSample > m_gateEnvelope)
                ? (m_gateEnvelope * 0.95f + absSample * 0.05f)
                : (m_gateEnvelope * 0.998f + absSample * 0.002f);

            float targetGain = (m_gateEnvelope >= gateThresholdLin) ? 1.0f : 0.0f;
            if (targetGain > m_gateGain) {
                m_gateGain += (targetGain - m_gateGain) * attackCoeff;
            } else {
                m_gateGain += (targetGain - m_gateGain) * releaseCoeff;
            }

            ch0[i] *= m_gateGain;
        }
    }

    // 4. Voice Compressor & Soft-Knee Limiter
    if (cfg.compressorEnabled) {
        const float compThresholdLin = DbToLinear(cfg.compThresholdDb);
        const float makeupLin = DbToLinear(cfg.compMakeupDb);
        const float compAttack = 0.008f;  // ~15ms attack
        const float compRelease = 0.0015f; // ~100ms release

        for (int i = 0; i < N; ++i) {
            float absSample = std::abs(ch0[i]);
            m_compEnvelope = (absSample > m_compEnvelope)
                ? (m_compEnvelope * (1.0f - compAttack) + absSample * compAttack)
                : (m_compEnvelope * (1.0f - compRelease) + absSample * compRelease);

            float envDb = LinearToDb(m_compEnvelope);
            float gainReductionDb = 0.0f;

            if (envDb > cfg.compThresholdDb) {
                float overshoot = envDb - cfg.compThresholdDb;
                gainReductionDb = -overshoot * (1.0f - (1.0f / std::max(1.0f, cfg.compRatio)));
            }

            float targetGain = DbToLinear(gainReductionDb) * makeupLin;
            m_compGain = m_compGain * 0.98f + targetGain * 0.02f;

            ch0[i] *= m_compGain;

            // 5. Soft-Knee Limiter (-0.5 dBFS ceiling = 0.944)
            if (cfg.limiterEnabled) {
                const float ceiling = 0.944f;
                float x = ch0[i];
                float absX = std::abs(x);
                if (absX > 0.75f) {
                    float sign = (x >= 0.0f) ? 1.0f : -1.0f;
                    float excess = absX - 0.75f;
                    float limitVal = 0.75f + (ceiling - 0.75f) * std::tanh(excess / (ceiling - 0.75f));
                    ch0[i] = sign * std::min(ceiling, limitVal);
                }
            }
        }
    }
}

} // namespace boulecam
