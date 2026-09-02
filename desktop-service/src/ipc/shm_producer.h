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

    bool Initialize(const std::wstring& shmName = L"Local\\BouleCam_SharedMemory_v1",
                    const std::wstring& eventName = L"Local\\BouleCam_Event_NewFrame_v1");
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

    void SetStreamingActive(bool active);
    void UpdateFormat(uint32_t width, uint32_t height, uint32_t fps);

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
