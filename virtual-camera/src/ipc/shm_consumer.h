#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include "boulecam_ipc.h"

namespace boulecam {

class ShmConsumer {
public:
    ShmConsumer();
    ~ShmConsumer();

    bool Open(const std::wstring& shmName = L"Local\\BouleCam_SharedMemory_v1",
              const std::wstring& eventName = L"Local\\BouleCam_Event_NewFrame_v1");
    void Close();

    // Waits up to timeoutMs for a new frame. Returns true if frame copied.
    bool ReadLatestFrame(uint8_t* pDestBuffer,
                         uint32_t destBufferSize,
                         uint32_t& outWidth,
                         uint32_t& outHeight,
                         uint32_t& outStride,
                         BouleCamPixelFormat& outPixelFormat,
                         uint32_t timeoutMs = 33);

    bool IsConnected() const { return m_pIpcHeader != nullptr; }
    bool IsStreamingActive() const;

private:
    HANDLE m_hMapFile;
    HANDLE m_hNewFrameEvent;
    BouleCamIpcHeader* m_pIpcHeader;
    uint32_t m_lastReadSequence;
};

} // namespace boulecam
