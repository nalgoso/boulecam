#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Per-camera SHM names — cam index 1..3
// Helper macros to build names at runtime (use the inline helpers below in C++)
#define BOULECAM_SHM_NAME_BASE        "Local\\BouleCam_SharedMemory_Cam"
#define BOULECAM_EVENT_NEW_FRAME_BASE "Local\\BouleCam_Event_NewFrame_Cam"
#define BOULECAM_MUTEX_NAME_BASE      "Local\\BouleCam_Mutex_Cam"
#define BOULECAM_SHM_VERSION_SUFFIX   "_v2"

// Legacy single-cam names (cam 1)
#define BOULECAM_SHM_NAME        "Local\\BouleCam_SharedMemory_Cam1_v2"
#define BOULECAM_EVENT_NEW_FRAME "Local\\BouleCam_Event_NewFrame_Cam1_v2"
#define BOULECAM_EVENT_FRAME_READ "Local\\BouleCam_Event_FrameRead_v1"
#define BOULECAM_MUTEX_NAME      "Local\\BouleCam_Mutex_Cam1_v2"

#define BOULECAM_MAX_CAM_SLOTS 3

#define BOULECAM_MAX_FRAME_WIDTH  3840
#define BOULECAM_MAX_FRAME_HEIGHT 2160
// Maximum buffer size per frame in NV12 / YUY2 / RGB32 (4K NV12 ~12.44 MB)
#define BOULECAM_MAX_FRAME_BUFFER_BYTES (BOULECAM_MAX_FRAME_WIDTH * BOULECAM_MAX_FRAME_HEIGHT * 4)

#define BOULECAM_SHM_RING_SLOTS 3 // Triple-buffering to avoid tearing and locking stalls

#pragma pack(push, 1)

typedef enum BouleCamPixelFormat {
    BOULECAM_PIXFMT_NV12 = 1, // Preferred for Media Foundation (Y plane + interleaved UV)
    BOULECAM_PIXFMT_YUY2 = 2,
    BOULECAM_PIXFMT_RGB32 = 3
} BouleCamPixelFormat;

/**
 * Individual Frame Slot in Shared Memory
 */
typedef struct BouleCamIpcSlot {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t pixel_format; // BouleCamPixelFormat
    uint32_t data_size;
    uint32_t sequence_number;
    uint64_t capture_timestamp_us;
    uint64_t decoded_timestamp_us;
    uint8_t  data[BOULECAM_MAX_FRAME_BUFFER_BYTES];
} BouleCamIpcSlot;

/**
 * Shared Memory Header and Triple-Buffer Layout (one instance per camera slot).
 */
typedef struct BouleCamIpcHeader {
    uint32_t magic;                 // 0x4243414D ("BCAM")
    uint32_t version;               // 2
    uint32_t is_streaming_active;   // 1 if mobile device is connected and feeding frames
    uint32_t current_width;
    uint32_t current_height;
    uint32_t current_fps;
    uint32_t active_slot_index;     // 0, 1, or 2 (Most recently written complete slot)
    uint64_t total_frames_produced;
    BouleCamIpcSlot slots[BOULECAM_SHM_RING_SLOTS];

    // Audio Ring Buffer (16-bit PCM 48kHz mono)
    uint32_t audio_sample_rate;
    uint32_t audio_channels;
    uint32_t audio_bits_per_sample;
    uint32_t audio_write_seq;
    uint32_t audio_write_pos;
    uint32_t audio_data_len;
    uint8_t  audio_data[65536];

    // Audio control flags (written by desktop-service, read by virtual-camera DLL)
    uint32_t audio_muted;           // 1 = muted (send silence to OBS), 0 = active
    int32_t  audio_gain_db_x100;    // Gain in dB * 100 (e.g. +6 dB = 600, -12 dB = -1200, 0 = unity)
} BouleCamIpcHeader;

#pragma pack(pop)

#ifdef __cplusplus
}
#endif
