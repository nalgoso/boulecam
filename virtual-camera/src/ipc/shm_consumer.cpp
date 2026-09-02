#include "shm_consumer.h"
#include <iostream>
#include <cstring>

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

bool ShmConsumer::ReadLatestFrame(uint8_t* pDestBuffer,
                                  uint32_t destBufferSize,
                                  uint32_t& outWidth,
                                  uint32_t& outHeight,
                                  uint32_t& outStride,
                                  BouleCamPixelFormat& outPixelFormat,
                                  uint32_t timeoutMs) {
    if (!m_pIpcHeader || !pDestBuffer) {
        // Try reconnecting in case desktop-service started after the camera consumer
        if (!Open()) return false;
    }

    // Wait for new frame event if available
    if (m_hNewFrameEvent != NULL && timeoutMs > 0) {
        WaitForSingleObject(m_hNewFrameEvent, timeoutMs);
    }

    uint32_t activeSlot = m_pIpcHeader->active_slot_index % BOULECAM_SHM_RING_SLOTS;
    const BouleCamIpcSlot& slot = m_pIpcHeader->slots[activeSlot];

    if (slot.data_size == 0 || slot.data_size > destBufferSize) {
        return false;
    }

    outWidth = slot.width;
    outHeight = slot.height;
    outStride = slot.stride;
    outPixelFormat = static_cast<BouleCamPixelFormat>(slot.pixel_format);

    memcpy(pDestBuffer, slot.data, slot.data_size);
    m_lastReadSequence = slot.sequence_number;

    return true;
}

} // namespace boulecam
