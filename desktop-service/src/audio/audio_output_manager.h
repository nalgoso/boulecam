#pragma once

#include <windows.h>
#include <mmsystem.h>
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <cmath>
#include <algorithm>

namespace boulecam {

class AudioOutputManager {
public:
    AudioOutputManager();
    ~AudioOutputManager();

    bool Initialize(uint32_t sampleRate = 48000, uint16_t channels = 1, uint16_t bitsPerSample = 16);
    void Shutdown();

    void WriteAudio(const uint8_t* pcmData, uint32_t size);

    void SetMuted(bool muted) { m_isMuted.store(muted); }
    bool IsMuted() const { return m_isMuted.load(); }

    void SetVolume(float volume) { m_volume.store(std::clamp(volume, 0.0f, 1.0f)); }
    float GetVolume() const { return m_volume.load(); }

    float GetCurrentLevel() const { return m_currentLevel.load(); }
    std::string GetDeviceName() const { return m_activeDeviceName; }
    bool IsCableActive() const { return m_isCableDevice; }

    static std::vector<std::pair<int, std::string>> EnumerateOutputDevices();
    bool SelectDevice(int deviceIndex);

private:
    void CleanCompletedHeaders();

    HWAVEOUT m_hWaveOut = nullptr;
    WAVEFORMATEX m_wfx{};
    int m_deviceIndex = -1; // -1 for WAVE_MAPPER
    std::string m_activeDeviceName = "WAVE_MAPPER (Default)";
    bool m_isCableDevice = false;

    std::atomic<bool> m_isInitialized{false};
    std::atomic<bool> m_isMuted{false};
    std::atomic<float> m_volume{1.0f};
    std::atomic<float> m_currentLevel{0.0f};

    std::mutex m_mutex;

    static constexpr size_t NUM_BUFFERS = 16;
    struct AudioBuffer {
        WAVEHDR header{};
        std::vector<uint8_t> data;
        bool inUse = false;
    };
    std::vector<AudioBuffer> m_buffers;
    size_t m_currentBufferIndex = 0;
};

} // namespace boulecam
