#pragma once

#include <cstdint>
#include <vector>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <mutex>

namespace boulecam {

/**
 * 2nd-order Butterworth Biquad Filter (Robert Bristow-Johnson Audio EQ Cookbook)
 * Direct Form II Transposed for numerical precision and zero phase-delay transients.
 */
class BiquadFilter {
public:
    BiquadFilter() = default;

    void ConfigureHighPass(float sampleRate, float cutoffHz, float q = 0.7071f) {
        if (sampleRate <= 0.0f || cutoffHz <= 0.0f) return;
        cutoffHz = std::clamp(cutoffHz, 20.0f, sampleRate * 0.45f);

        const float pi = 3.14159265358979323846f;
        float w0 = 2.0f * pi * (cutoffHz / sampleRate);
        float cosW0 = std::cos(w0);
        float sinW0 = std::sin(w0);
        float alpha = sinW0 / (2.0f * q);

        float a0 = 1.0f + alpha;
        m_b0 = ((1.0f + cosW0) / 2.0f) / a0;
        m_b1 = (-(1.0f + cosW0)) / a0;
        m_b2 = ((1.0f + cosW0) / 2.0f) / a0;
        m_a1 = (-2.0f * cosW0) / a0;
        m_a2 = (1.0f - alpha) / a0;
        Reset();
    }

    void Reset() {
        m_s1 = 0.0f;
        m_s2 = 0.0f;
    }

    inline float Process(float in) {
        float out = m_b0 * in + m_s1;
        m_s1 = m_b1 * in - m_a1 * out + m_s2;
        m_s2 = m_b2 * in - m_a2 * out;
        return out;
    }

private:
    float m_b0 = 1.0f;
    float m_b1 = 0.0f;
    float m_b2 = 0.0f;
    float m_a1 = 0.0f;
    float m_a2 = 0.0f;
    float m_s1 = 0.0f;
    float m_s2 = 0.0f;
};

/**
 * Dedicated Church Audio DSP Processor
 * - Modo Principal: Flat response (unfiltered), full dynamic spectrum.
 * - Modo Ambiente (Congregación): High-Pass Filter (Low-Cut a 220 Hz) para eliminar
 *   retumbos de sala, rebote de subwoofers del PA y choque de fase/peine,
 *   junto con atenuación de ganancia ambiental de -16 dB para integrarse
 *   perfectamente con la consola principal sin opacarla.
 */
class AudioDspProcessor {
public:
    AudioDspProcessor();
    ~AudioDspProcessor() = default;

    void SetAmbientMode(bool ambient);
    bool IsAmbientMode() const { return m_isAmbient.load(); }

    void SetUserGainDb(float gainDb) { m_userGainDb.store(std::clamp(gainDb, -40.0f, 20.0f)); }
    float GetUserGainDb() const { return m_userGainDb.load(); }

    void ProcessPcm16(int16_t* samples, size_t sampleCount, uint32_t sampleRate, uint16_t channels);

private:
    std::atomic<bool> m_isAmbient{false};
    std::atomic<float> m_userGainDb{0.0f};

    std::mutex m_filterMutex;
    uint32_t m_configuredSampleRate = 0;
    BiquadFilter m_hpfL;
    BiquadFilter m_hpfR;
};

} // namespace boulecam
