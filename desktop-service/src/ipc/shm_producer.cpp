#include "shm_producer.h"
#include <iostream>
#include <cstring>

namespace boulecam {

ShmProducer::ShmProducer()
    : m_hMapFile(NULL)
    , m_hNewFrameEvent(NULL)
    , m_hMutex(NULL)
    , m_pIpcHeader(nullptr)
    , m_writeIndex(0)
    , m_framesProduced(0) {
}

ShmProducer::~ShmProducer() {
    Shutdown();
}

bool ShmProducer::Initialize(const std::wstring& shmName, const std::wstring& eventName) {
    if (m_pIpcHeader != nullptr) {
        return true; // Already initialized
    }

    // Allocate / Open Shared Memory file mapping
    size_t totalBytes = sizeof(BouleCamIpcHeader);
    m_hMapFile = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        0,
        static_cast<DWORD>(totalBytes),
        shmName.c_str()
    );

    if (m_hMapFile == NULL) {
        std::cerr << "[ShmProducer] Failed to create file mapping. Error: " << GetLastError() << std::endl;
        return false;
    }

    m_pIpcHeader = static_cast<BouleCamIpcHeader*>(
        MapViewOfFile(m_hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, totalBytes)
    );

    if (m_pIpcHeader == nullptr) {
        std::cerr << "[ShmProducer] Failed to map view of file. Error: " << GetLastError() << std::endl;
        CloseHandle(m_hMapFile);
        m_hMapFile = NULL;
        return false;
    }

    // Initialize Header with default zero values
    memset(m_pIpcHeader, 0, sizeof(BouleCamIpcHeader));
    m_pIpcHeader->magic = 0x4243414D; // "BCAM"
    m_pIpcHeader->version = 1;
    m_pIpcHeader->is_streaming_active = 0;
    m_pIpcHeader->current_width = 1920;
    m_pIpcHeader->current_height = 1080;
    m_pIpcHeader->current_fps = 60;
    m_pIpcHeader->active_slot_index = 0;

    // Create / Open New Frame Notification Event (Manual-reset or Auto-reset)
    m_hNewFrameEvent = CreateEventW(NULL, FALSE, FALSE, eventName.c_str());
    if (m_hNewFrameEvent == NULL) {
        std::cerr << "[ShmProducer] Failed to create frame event. Error: " << GetLastError() << std::endl;
        Shutdown();
        return false;
    }

    // Create named Mutex for safe multi-process coordination if required
    m_hMutex = CreateMutexW(NULL, FALSE, L"Local\\BouleCam_Mutex_v1");

    std::cout << "[ShmProducer] Shared memory successfully initialized. (" 
              << (totalBytes / (1024 * 1024)) << " MB allocated for Triple-Buffer)" << std::endl;
    return true;
}

void ShmProducer::Shutdown() {
    if (m_pIpcHeader != nullptr) {
        m_pIpcHeader->is_streaming_active = 0;
        UnmapViewOfFile(m_pIpcHeader);
        m_pIpcHeader = nullptr;
    }
    if (m_hNewFrameEvent != NULL) {
        CloseHandle(m_hNewFrameEvent);
        m_hNewFrameEvent = NULL;
    }
    if (m_hMutex != NULL) {
        CloseHandle(m_hMutex);
        m_hMutex = NULL;
    }
    if (m_hMapFile != NULL) {
        CloseHandle(m_hMapFile);
        m_hMapFile = NULL;
    }
}

void ShmProducer::SetStreamingActive(bool active) {
    if (m_pIpcHeader != nullptr) {
        m_pIpcHeader->is_streaming_active = active ? 1 : 0;
    }
}

void ShmProducer::UpdateFormat(uint32_t width, uint32_t height, uint32_t fps) {
    if (m_pIpcHeader != nullptr) {
        m_pIpcHeader->current_width = width;
        m_pIpcHeader->current_height = height;
        m_pIpcHeader->current_fps = fps;
    }
}

bool ShmProducer::WriteFrame(const uint8_t* pFrameData,
                             uint32_t dataSize,
                             uint32_t width,
                             uint32_t height,
                             uint32_t stride,
                             BouleCamPixelFormat pixelFormat,
                             uint64_t captureTimestampUs,
                             uint64_t decodedTimestampUs) {
    if (!m_pIpcHeader || !pFrameData || dataSize > BOULECAM_MAX_FRAME_BUFFER_BYTES) {
        return false;
    }

    // Determine target slot in triple buffer (advance write index)
    uint32_t currentActive = m_pIpcHeader->active_slot_index;
    uint32_t nextSlot = (currentActive + 1) % BOULECAM_SHM_RING_SLOTS;

    BouleCamIpcSlot& slot = m_pIpcHeader->slots[nextSlot];
    slot.width = width;
    slot.height = height;
    slot.stride = stride;
    slot.pixel_format = static_cast<uint32_t>(pixelFormat);
    slot.data_size = dataSize;
    slot.sequence_number = static_cast<uint32_t>(m_framesProduced.load());
    slot.capture_timestamp_us = captureTimestampUs;
    slot.decoded_timestamp_us = decodedTimestampUs;

    // Fast memory copy of decoded frame data into shared buffer
    memcpy(slot.data, pFrameData, dataSize);

    // Atomically publish newly written slot as active
    m_pIpcHeader->active_slot_index = nextSlot;
    m_pIpcHeader->total_frames_produced = ++m_framesProduced;
    m_pIpcHeader->is_streaming_active = 1;

    // Signal waiting virtual camera consumers (Win32 auto-reset event)
    if (m_hNewFrameEvent != NULL) {
        SetEvent(m_hNewFrameEvent);
    }

    return true;
}

} // namespace boulecam
