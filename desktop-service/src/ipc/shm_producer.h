#pragma once

#include <windows.h>
#include <string>
#include <cstdint>
#include <atomic>
#include "boulecam_ipc.h"

namespace boulecam {

class ShmProducer {
public:
    ShmProducer();
    ~ShmProducer();

    // Initialize for a specific camera slot (1..3). Builds the named SHM/event strings automatically.
    bool Initialize(int camIndex = 1);

    // Legacy overload with explicit names
    bool Initialize(const std::wstring& shmName, const std::wstring& eventName);

    void Shutdown();

    // Writes a new decoded frame to the next available ring buffer slot (Triple-Buffering)
    bool WriteFrame(const uint8_t* pFrameData,
                    uint32_t dataSize,
                    uint32_t width,
                    uint32_t height,
                    uint32_t stride,
                    BouleCamPixelFormat pixelFormat,
                    uint64_t captureTimestampUs,
                    uint64_t decodedTimestampUs);

    bool WriteAudio(const uint8_t* pPcmData,
                    uint32_t dataSize,
                    uint32_t sampleRate = 48000,
                    uint32_t channels = 1);

    void SetStreamingActive(bool active);
    void UpdateFormat(uint32_t width, uint32_t height, uint32_t fps);

    // Audio control — written into SHM and consumed by the virtual-camera DLL in real-time
    void SetAudioMuted(bool muted);
    void SetAudioGainDb(float gainDb); // e.g. 0.0 = unity, +6.0 = double, -12.0 = quarter

    bool IsInitialized() const { return m_pIpcHeader != nullptr; }

private:
    HANDLE m_hMapFile;
    HANDLE m_hNewFrameEvent;
    HANDLE m_hMutex;
    BouleCamIpcHeader* m_pIpcHeader;
    std::atomic<uint32_t> m_writeIndex;
    std::atomic<uint64_t> m_framesProduced;
};

} // namespace boulecam
