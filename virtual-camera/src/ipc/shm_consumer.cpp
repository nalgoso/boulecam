#include "shm_consumer.h"
#include <iostream>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <algorithm>

namespace boulecam {

ShmConsumer::ShmConsumer()
    : m_hMapFile(NULL)
    , m_hNewFrameEvent(NULL)
    , m_pIpcHeader(nullptr)
    , m_lastReadSequence(0) {
}

ShmConsumer::~ShmConsumer() {
    Close();
}

// Open by camera index (1..3)
bool ShmConsumer::Open(int camIndex) {
    m_camIndex = camIndex;
    if (m_pIpcHeader != nullptr) return true;

    char shmNameA[128], eventNameA[128];
    snprintf(shmNameA,  sizeof(shmNameA),  "Local\\BouleCam_SharedMemory_Cam%d_v2", camIndex);
    snprintf(eventNameA, sizeof(eventNameA), "Local\\BouleCam_Event_NewFrame_Cam%d_v2", camIndex);

    // Convert to wide
    wchar_t shmNameW[128], eventNameW[128];
    mbstowcs(shmNameW,  shmNameA,  128);
    mbstowcs(eventNameW, eventNameA, 128);

    return Open(std::wstring(shmNameW), std::wstring(eventNameW));
}

// Open by explicit name (legacy / internal)
bool ShmConsumer::Open(const std::wstring& shmName, const std::wstring& eventName) {
    if (m_pIpcHeader != nullptr) return true;

    m_hMapFile = OpenFileMappingW(FILE_MAP_READ, FALSE, shmName.c_str());
    if (m_hMapFile == NULL) {
        return false;
    }

    size_t totalBytes = sizeof(BouleCamIpcHeader);
    m_pIpcHeader = static_cast<BouleCamIpcHeader*>(
        MapViewOfFile(m_hMapFile, FILE_MAP_READ, 0, 0, totalBytes)
    );

    if (m_pIpcHeader == nullptr) {
        CloseHandle(m_hMapFile);
        m_hMapFile = NULL;
        return false;
    }

    // Open notification event
    m_hNewFrameEvent = OpenEventW(SYNCHRONIZE, FALSE, eventName.c_str());
    return true;
}

void ShmConsumer::Close() {
    if (m_pIpcHeader != nullptr) {
        UnmapViewOfFile(m_pIpcHeader);
        m_pIpcHeader = nullptr;
    }
    if (m_hNewFrameEvent != NULL) {
        CloseHandle(m_hNewFrameEvent);
        m_hNewFrameEvent = NULL;
    }
    if (m_hMapFile != NULL) {
        CloseHandle(m_hMapFile);
        m_hMapFile = NULL;
    }
}

bool ShmConsumer::IsStreamingActive() const {
    if (!m_pIpcHeader) return false;
    return m_pIpcHeader->is_streaming_active == 1;
}

bool ShmConsumer::IsAudioMuted() const {
    if (!m_pIpcHeader) return false;
    return m_pIpcHeader->audio_muted != 0;
}

float ShmConsumer::GetAudioGainDb() const {
    if (!m_pIpcHeader) return 0.0f;
    return static_cast<float>(m_pIpcHeader->audio_gain_db_x100) / 100.0f;
}

bool ShmConsumer::ReadLatestFrame(uint8_t* pDestBuffer,
                                  uint32_t destBufferSize,
                                  uint32_t& outWidth,
                                  uint32_t& outHeight,
                                  uint32_t& outStride,
                                  BouleCamPixelFormat& outPixelFormat,
                                  uint32_t timeoutMs) {
    if (!m_pIpcHeader || !pDestBuffer) {
        // Try reconnecting in case desktop-service started after the camera consumer
        if (!Open(m_camIndex)) return false;
    }

    // Wait for new frame event if available
    if (m_hNewFrameEvent != NULL && timeoutMs > 0) {
        WaitForSingleObject(m_hNewFrameEvent, timeoutMs);
    }

    uint32_t activeSlot = m_pIpcHeader->active_slot_index % BOULECAM_SHM_RING_SLOTS;
    const BouleCamIpcSlot& slot = m_pIpcHeader->slots[activeSlot];

    if (slot.data_size == 0) {
        return false;
    }

    outWidth = slot.width;
    outHeight = slot.height;
    outStride = slot.stride;
    outPixelFormat = static_cast<BouleCamPixelFormat>(slot.pixel_format);

    uint32_t bytesToCopy = (slot.data_size <= destBufferSize) ? slot.data_size : destBufferSize;
    memcpy(pDestBuffer, slot.data, bytesToCopy);
    m_lastReadSequence = slot.sequence_number;

    return true;
}

bool ShmConsumer::ReadLatestAudio(uint8_t* pDestBuffer, uint32_t destBufferSize,
                                  uint32_t& outSize, uint32_t& outSampleRate, uint32_t& outChannels) {
    if (!m_pIpcHeader) {
        if (!Open(m_camIndex)) return false;
    }

    uint32_t len = m_pIpcHeader->audio_data_len;
    if (len == 0 || len > destBufferSize) return false;

    outSampleRate = m_pIpcHeader->audio_sample_rate > 0 ? m_pIpcHeader->audio_sample_rate : 48000;
    outChannels   = m_pIpcHeader->audio_channels   > 0 ? m_pIpcHeader->audio_channels   : 1;
    outSize       = len;

    // Read the most recent packet from the ring buffer
    uint32_t writePos = m_pIpcHeader->audio_write_pos;
    if (writePos >= len) {
        memcpy(pDestBuffer, m_pIpcHeader->audio_data + writePos - len, len);
    } else {
        uint32_t fromEnd = len - writePos;
        memcpy(pDestBuffer, m_pIpcHeader->audio_data + 65536 - fromEnd, fromEnd);
        memcpy(pDestBuffer + fromEnd, m_pIpcHeader->audio_data, writePos);
    }

    // Apply mute: send silence
    if (m_pIpcHeader->audio_muted) {
        memset(pDestBuffer, 0, len);
        return true;
    }

    // Apply gain if non-zero
    int32_t gainX100 = m_pIpcHeader->audio_gain_db_x100;
    if (gainX100 != 0) {
        float gainDb  = static_cast<float>(gainX100) / 100.0f;
        float gainLin = powf(10.0f, gainDb / 20.0f);
        int16_t* samples = reinterpret_cast<int16_t*>(pDestBuffer);
        uint32_t sampleCount = len / 2;
        for (uint32_t i = 0; i < sampleCount; ++i) {
            float v = static_cast<float>(samples[i]) * gainLin;
            if (v >  32767.0f) v =  32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            samples[i] = static_cast<int16_t>(v);
        }
    }

    return true;
}

} // namespace boulecam
