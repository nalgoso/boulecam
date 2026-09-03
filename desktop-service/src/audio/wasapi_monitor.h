#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>

namespace boulecam {

class WasapiMonitor {
public:
    WasapiMonitor();
    ~WasapiMonitor();

    bool Start(const std::wstring& deviceId = L"");
    void Stop();

    void SetVolume(float vol); // 0.0f to 1.0f
    float GetVolume() const { return m_volume.load(); }

    void SetMute(bool mute) { m_isMuted.store(mute); }
    bool IsMuted() const { return m_isMuted.load(); }

    void PushPcm(const uint8_t* pcm16, uint32_t bytes, int channels);

    struct AudioOutputDevice {
        std::string id;
        std::string name;
        bool isDefault;
    };

    static std::vector<AudioOutputDevice> EnumerateOutputDevices();

private:
    void PlayoutWorker();

    std::atomic<bool> m_isRunning{false};
    std::atomic<float> m_volume{0.8f};
    std::atomic<bool> m_isMuted{false};

    std::thread m_playoutThread;

    IMMDeviceEnumerator* m_pEnumerator = nullptr;
    IMMDevice* m_pDevice = nullptr;
    IAudioClient* m_pAudioClient = nullptr;
    IAudioRenderClient* m_pRenderClient = nullptr;
    WAVEFORMATEX* m_pMixFormat = nullptr;

    // Lock-free circular buffer for 16-bit PCM samples
    static const size_t RING_BUFFER_SIZE = 48000 * 4; // ~1 second buffer
    std::vector<int16_t> m_ringBuffer;
    std::atomic<size_t> m_writeIndex{0};
    std::atomic<size_t> m_readIndex{0};
    std::atomic<size_t> m_availableSamples{0};
    std::mutex m_pushMutex;
};

} // namespace boulecam
