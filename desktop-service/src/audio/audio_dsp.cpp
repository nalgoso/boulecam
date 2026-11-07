#include "audio_dsp.h"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <iostream>

namespace boulecam {

// =========================================================================
// CLOCK DRIFT COMPENSATOR
// =========================================================================

void ClockDriftCompensator::Reset() {
    m_initialized = false;
    m_totalFramesReceived = 0;
    m_smoothedDriftMs.store(0.0);
    m_correctionsCount.store(0);
    m_lastCorrectionTime = std::chrono::steady_clock::time_point{};
}

void ClockDriftCompensator::Process(std::vector<uint8_t>& pcmBytes, uint32_t sampleRate, uint16_t channels) {
    if (pcmBytes.empty() || sampleRate == 0 || channels == 0) return;

    size_t bytesPerFrame = channels * sizeof(int16_t);
    size_t frameCount = pcmBytes.size() / bytesPerFrame;
    if (frameCount < 10) return;

    auto now = std::chrono::steady_clock::now();
    if (!m_initialized) {
        m_startTime = now;
        m_lastCorrectionTime = now;
        m_initialized = true;
        m_totalFramesReceived = frameCount;
        return;
    }

    m_totalFramesReceived += frameCount;
    double elapsedSec = std::chrono::duration<double>(now - m_startTime).count();

    // Allow 1.5 seconds warm-up before activating drift corrections
    if (elapsedSec < 1.5) return;

    double expectedFrames = elapsedSec * static_cast<double>(sampleRate);
    double instantDriftFrames = static_cast<double>(m_totalFramesReceived) - expectedFrames;
    double instantDriftMs = (instantDriftFrames / static_cast<double>(sampleRate)) * 1000.0;

    // Exponential Moving Average filter (alpha = 0.01) to reject Wi-Fi packet bursts/jitter
    double currentSmoothed = m_smoothedDriftMs.load();
    double newSmoothed = currentSmoothed * 0.99 + instantDriftMs * 0.01;
    m_smoothedDriftMs.store(newSmoothed);

    // Minimum interval between micro-corrections (at least 60 ms apart to ensure inaudibility)
    double timeSinceLastCorrectionMs = std::chrono::duration<double, std::milli>(now - m_lastCorrectionTime).count();
    if (timeSinceLastCorrectionMs < 60.0) {
        return;
    }

    int16_t* samples = reinterpret_cast<int16_t*>(pcmBytes.data());

    // Case 1: Phone mic clock is faster than PC clock (drift > +25 ms) -> Drop 1 frame at zero-crossing
    if (newSmoothed > 25.0) {
        // Find best zero-crossing in the middle of the chunk (frames 4 to frameCount - 4)
        size_t bestIdx = 0;
        int32_t minEnergy = 32768;

        for (size_t f = 4; f < frameCount - 4; ++f) {
            int16_t s0 = samples[f * channels];
            int16_t s1 = samples[(f + 1) * channels];
            // Look for sign change (zero crossing) with lowest absolute amplitude
            if ((s0 <= 0 && s1 >= 0) || (s0 >= 0 && s1 <= 0)) {
                int32_t energy = std::abs(s0) + std::abs(s1);
                if (energy < minEnergy) {
                    minEnergy = energy;
                    bestIdx = f;
                    if (energy < 100) break; // Perfect zero-crossing found
                }
            }
        }

        if (bestIdx == 0) {
            bestIdx = frameCount / 2; // Fallback to center frame
        }

        // Interpolate across the dropped frame
        for (uint16_t c = 0; c < channels; ++c) {
            int32_t prevVal = samples[(bestIdx - 1) * channels + c];
            int32_t nextVal = samples[(bestIdx + 1) * channels + c];
            samples[(bestIdx - 1) * channels + c] = static_cast<int16_t>((prevVal + nextVal) / 2);
        }

        // Shift remaining samples left by 1 frame
        size_t bytesToMove = (frameCount - (bestIdx + 1)) * bytesPerFrame;
        std::memmove(&samples[bestIdx * channels], &samples[(bestIdx + 1) * channels], bytesToMove);
        pcmBytes.resize(pcmBytes.size() - bytesPerFrame);

        m_totalFramesReceived--;
        m_lastCorrectionTime = now;
        m_correctionsCount++;
    }
    // Case 2: Phone mic clock is slower than PC clock (drift < -25 ms) -> Insert 1 interpolated frame
    else if (newSmoothed < -25.0) {
        // Find best zero-crossing in the middle of the chunk
        size_t bestIdx = 0;
        int32_t minEnergy = 32768;

        for (size_t f = 4; f < frameCount - 4; ++f) {
            int16_t s0 = samples[f * channels];
            int16_t s1 = samples[(f + 1) * channels];
            if ((s0 <= 0 && s1 >= 0) || (s0 >= 0 && s1 <= 0)) {
                int32_t energy = std::abs(s0) + std::abs(s1);
                if (energy < minEnergy) {
                    minEnergy = energy;
                    bestIdx = f;
                    if (energy < 100) break;
                }
            }
        }

        if (bestIdx == 0) {
            bestIdx = frameCount / 2;
        }

        // Expand vector by 1 frame
        size_t oldByteSize = pcmBytes.size();
        pcmBytes.resize(oldByteSize + bytesPerFrame);
        int16_t* newSamples = reinterpret_cast<int16_t*>(pcmBytes.data());

        // Shift samples right by 1 frame from bestIdx
        size_t bytesToShift = (frameCount - bestIdx) * bytesPerFrame;
        std::memmove(&newSamples[(bestIdx + 1) * channels], &newSamples[bestIdx * channels], bytesToShift);

        // Calculate interpolated frame
        for (uint16_t c = 0; c < channels; ++c) {
            int32_t sPrev = newSamples[(bestIdx - 1) * channels + c];
            int32_t sNext = newSamples[(bestIdx + 1) * channels + c];
            newSamples[bestIdx * channels + c] = static_cast<int16_t>((sPrev + sNext) / 2);
        }

        m_totalFramesReceived++;
        m_lastCorrectionTime = now;
        m_correctionsCount++;
    }
}

// =========================================================================
// AUDIO DSP PROCESSOR
// =========================================================================

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

void AudioDspProcessor::ProcessPcm16(std::vector<uint8_t>& pcmBuffer, uint32_t sampleRate, uint16_t channels) {
    if (pcmBuffer.empty() || sampleRate == 0 || channels == 0) return;

    // 1. First, compensate clock drift dynamically (may slightly shrink/expand pcmBuffer)
    {
        std::lock_guard<std::mutex> lock(m_filterMutex);
        m_driftCompensator.Process(pcmBuffer, sampleRate, channels);
    }

    // 2. Next, apply High-Pass Filter and Gain processing on the compensated buffer
    size_t sampleCount = pcmBuffer.size() / sizeof(int16_t);
    ProcessPcm16(reinterpret_cast<int16_t*>(pcmBuffer.data()), sampleCount, sampleRate, channels);
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
        // 80 Hz Butterworth High-Pass: eliminates HVAC, stage rumble and handling noise without cutting vocal body
        m_hpfL.ConfigureHighPass(static_cast<float>(sampleRate), 80.0f);
        m_hpfR.ConfigureHighPass(static_cast<float>(sampleRate), 80.0f);
    }

    // Gentle baseline attenuation for ambient mic (-3 dB) to prevent clipping while keeping natural level
    float totalDb = userGain;
    if (isAmbient) {
        totalDb -= 3.0f;
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
