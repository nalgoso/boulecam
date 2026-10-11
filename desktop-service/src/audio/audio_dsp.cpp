#include "audio_dsp.h"
#include <cmath>
#include <algorithm>

namespace boulecam {

AudioDspProcessor::AudioDspProcessor()
    : m_isAmbient(false)
    , m_userGainDb(0.0f)
    , m_configuredSampleRate(0) {
}

void AudioDspProcessor::SetAmbientMode(bool ambient) {
    if (m_isAmbient.load() == ambient) return;
    m_isAmbient.store(ambient);

    std::lock_guard<std::mutex> lock(m_filterMutex);
    m_hpfL.Reset();
    m_hpfR.Reset();
}

void AudioDspProcessor::ProcessPcm16(int16_t* samples, size_t sampleCount, uint32_t sampleRate, uint16_t channels) {
    if (!samples || sampleCount == 0 || channels == 0 || sampleRate == 0) return;

    bool isAmbient = m_isAmbient.load();
    float userGain = m_userGainDb.load();

    // If in main mode and unity gain, pure pass-through
    if (!isAmbient && std::abs(userGain) < 0.05f) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_filterMutex);

    // Reconfigure high-pass filter if sample rate changed
    if (sampleRate != m_configuredSampleRate) {
        m_configuredSampleRate = sampleRate;
        // 220 Hz Butterworth High-Pass: eliminates subwoofers, kick drum bleed and room rumble
        m_hpfL.ConfigureHighPass(static_cast<float>(sampleRate), 220.0f);
        m_hpfR.ConfigureHighPass(static_cast<float>(sampleRate), 220.0f);
    }

    // Baseline attenuation for ambient mic (-16 dB) so congregation mic sits behind the console
    float totalDb = userGain;
    if (isAmbient) {
        totalDb -= 16.0f;
    }

    float linearGain = std::pow(10.0f, totalDb / 20.0f);

    if (channels == 1) {
        for (size_t i = 0; i < sampleCount; ++i) {
            float val = static_cast<float>(samples[i]);
            if (isAmbient) {
                val = m_hpfL.Process(val);
            }
            val *= linearGain;
            int32_t clamped = std::clamp(static_cast<int32_t>(val), -32768, 32767);
            samples[i] = static_cast<int16_t>(clamped);
        }
    } else {
        // Interleaved Stereo: L, R, L, R...
        for (size_t i = 0; i < sampleCount; i += channels) {
            float valL = static_cast<float>(samples[i]);
            if (isAmbient) {
                valL = m_hpfL.Process(valL);
            }
            valL *= linearGain;
            int32_t clampedL = std::clamp(static_cast<int32_t>(valL), -32768, 32767);
            samples[i] = static_cast<int16_t>(clampedL);

            if (i + 1 < sampleCount) {
                float valR = static_cast<float>(samples[i + 1]);
                if (isAmbient) {
                    valR = m_hpfR.Process(valR);
                }
                valR *= linearGain;
                int32_t clampedR = std::clamp(static_cast<int32_t>(valR), -32768, 32767);
                samples[i + 1] = static_cast<int16_t>(clampedR);
            }
        }
    }
}

} // namespace boulecam
