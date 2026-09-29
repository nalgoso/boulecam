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

    // Open shared memory for a specific camera slot (1..3).
    // When camIndex == 0 the default (cam 1) names are used.
    bool Open(int camIndex = 1);

    // Legacy overload kept for compatibility
    bool Open(const std::wstring& shmName,
              const std::wstring& eventName = L"Local\\BouleCam_Event_NewFrame_Cam1_v2");

    void Close();

    // Waits up to timeoutMs for a new frame. Returns true if frame copied.
    bool ReadLatestFrame(uint8_t* pDestBuffer,
                         uint32_t destBufferSize,
                         uint32_t& outWidth,
                         uint32_t& outHeight,
                         uint32_t& outStride,
                         BouleCamPixelFormat& outPixelFormat,
                         uint32_t timeoutMs = 33);

    bool ReadLatestAudio(uint8_t* pDestBuffer,
                         uint32_t destBufferSize,
                         uint32_t& outSize,
                         uint32_t& outSampleRate,
                         uint32_t& outChannels);

    bool IsConnected() const { return m_pIpcHeader != nullptr; }
    bool IsStreamingActive() const;

    // Audio control accessors (read from SHM flags set by desktop-service)
    bool  IsAudioMuted() const;
    float GetAudioGainDb() const;

private:
    HANDLE m_hMapFile;
    HANDLE m_hNewFrameEvent;
    BouleCamIpcHeader* m_pIpcHeader;
    uint32_t m_lastReadSequence;
    int m_camIndex = 1;
};

} // namespace boulecam
