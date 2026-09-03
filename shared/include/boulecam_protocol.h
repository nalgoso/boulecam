#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(push, 1)

#define BOULECAM_MAGIC 0x4243414D // "BCAM" in ASCII (0x42, 0x43, 0x41, 0x4D)
#define BOULECAM_DEFAULT_TCP_PORT 8088
#define BOULECAM_DEFAULT_UDP_PORT 8089
#define BOULECAM_DEFAULT_WS_PORT 8090
#define BOULECAM_MDNS_SERVICE_TYPE "_boulecam._tcp"

/**
 * Codec identifier
 */
typedef enum BouleCamCodecType {
    BOULECAM_CODEC_H264 = 1,
    BOULECAM_CODEC_H265 = 2,
    BOULECAM_CODEC_MJPEG = 3
} BouleCamCodecType;

/**
 * Packet types
 */
typedef enum BouleCamPacketType {
    BOULECAM_PKT_HANDSHAKE_REQ  = 0x01,
    BOULECAM_PKT_HANDSHAKE_RESP = 0x02,
    BOULECAM_PKT_FRAME_DATA     = 0x10,
    BOULECAM_PKT_FRAME_SPS_PPS  = 0x11,
    BOULECAM_PKT_PING           = 0x20,
    BOULECAM_PKT_PONG           = 0x21,
    BOULECAM_PKT_CAMERA_CMD     = 0x30, // Desktop -> Mobile (Control settings)
    BOULECAM_PKT_CAMERA_STATE   = 0x31, // Mobile -> Desktop (Telemetry & capabilities)
    BOULECAM_PKT_AUDIO_DATA     = 0x40, // Mobile -> Desktop (PCM/AAC audio)
    BOULECAM_PKT_DISCONNECT     = 0xFF
} BouleCamPacketType;

/**
 * Camera Actions for Remote Control
 */
typedef enum BouleCamCameraAction {
    BOULECAM_ACTION_SET_LENS        = 1, // 0 = Back, 1 = Front, 2 = UltraWide
    BOULECAM_ACTION_SET_TORCH       = 2, // 0 = Off, 1 = On (Torch)
    BOULECAM_ACTION_SET_ISO         = 3, // -1 = Auto, or manual value (100 - 6400)
    BOULECAM_ACTION_SET_EXPOSURE    = 4, // -1 = Auto, or nanoseconds (e.g. 20000000 = 1/50s)
    BOULECAM_ACTION_SET_EV          = 5, // -12 to +12 (EV compensation steps)
    BOULECAM_ACTION_SET_WB          = 6, // 0 = Auto, 1 = Incandescent, 2 = Fluorescent, 3 = Daylight, 4 = Cloudy, 5 = Shade
    BOULECAM_ACTION_SET_FOCUS       = 7, // 0 = Auto Continuous, 1 = Manual (float_param1 = 0.0f..1.0f)
    BOULECAM_ACTION_SET_MIC         = 8, // 0 = Mute/Disabled, 1 = Enabled
    BOULECAM_ACTION_REQUEST_KEYFRAME= 9, // Request IDR Sync Frame
    BOULECAM_ACTION_SET_DIM_SCREEN  = 10, // 0 = Normal brightness, 1 = Dim screen (Power saving)
    BOULECAM_ACTION_SET_ZOOM        = 11, // float_param1 = zoom ratio (e.g. 1.0f to 10.0f)
    BOULECAM_ACTION_SET_MIC_CAPSULE = 12, // 0 = Auto/Default, 1 = Bottom, 2 = Back/Camera, 3 = Front/Selfie
    BOULECAM_ACTION_SET_MIC_STEREO  = 13, // 0 = Mono, 1 = Real Stereo
    BOULECAM_ACTION_SET_MIC_BEAMFORMING = 14 // 0 = Off, 1 = On (Hardware Beamforming)
} BouleCamCameraAction;

/**
 * Handshake Request (Mobile -> Desktop) - 80 bytes
 */
typedef struct BouleCamHandshakeReq {
    uint32_t magic;             // BOULECAM_MAGIC
    uint8_t  packet_type;       // BOULECAM_PKT_HANDSHAKE_REQ
    uint8_t  version_major;     // 1
    uint8_t  version_minor;     // 0
    uint8_t  codec_type;        // BouleCamCodecType
    uint32_t width;             // e.g. 1920
    uint32_t height;            // e.g. 1080
    uint32_t target_fps;        // e.g. 60
    uint32_t target_bitrate;    // in bps, e.g. 8000000 (8 Mbps)
    char     device_name[64];   // e.g. "Samsung Galaxy S23", "Pixel 8"
} BouleCamHandshakeReq;

/**
 * Handshake Response (Desktop -> Mobile) - 18 bytes
 */
typedef struct BouleCamHandshakeResp {
    uint32_t magic;             // BOULECAM_MAGIC
    uint8_t  packet_type;       // BOULECAM_PKT_HANDSHAKE_RESP
    uint8_t  status_code;       // 0 = OK, 1 = Unsupported Codec, 2 = Busy
    uint32_t negotiated_width;  // Desktop accepted width
    uint32_t negotiated_height; // Desktop accepted height
    uint32_t negotiated_fps;    // Desktop accepted FPS
} BouleCamHandshakeResp;

/**
 * High-performance Video Frame Packet Header - 24 bytes (no padding)
 */
typedef struct BouleCamFrameHeader {
    uint32_t magic;             // BOULECAM_MAGIC (0x4243414D)
    uint8_t  packet_type;       // BOULECAM_PKT_FRAME_DATA (0x10) or BOULECAM_PKT_FRAME_SPS_PPS (0x11)
    uint8_t  is_keyframe;       // 1 = IDR/Keyframe, 0 = Non-keyframe
    uint16_t rotation_degrees;  // 0, 90, 180, 270 degrees
    uint32_t sequence_number;   // Frame sequence number for drop detection
    uint64_t capture_timestamp_us; // Capture timestamp in microseconds (PTS)
    uint32_t payload_size;      // Size in bytes of H.264/H.265 NAL units following
} BouleCamFrameHeader;

/**
 * Audio Packet Header - 12 bytes
 */
typedef struct BouleCamAudioHeader {
    uint32_t magic;             // BOULECAM_MAGIC (0x4243414D)
    uint8_t  packet_type;       // BOULECAM_PKT_AUDIO_DATA (0x40)
    uint8_t  channels;          // 1 = Mono, 2 = Stereo
    uint16_t sample_rate;       // e.g. 48000
    uint32_t payload_size;      // Size in bytes of raw PCM 16-bit LE following
} BouleCamAudioHeader;

/**
 * Camera Remote Control Command (Desktop -> Mobile) - 22 bytes
 */
typedef struct BouleCamCameraCmd {
    uint32_t magic;             // BOULECAM_MAGIC (0x4243414D)
    uint8_t  packet_type;       // BOULECAM_PKT_CAMERA_CMD (0x30)
    uint8_t  action;            // BouleCamCameraAction
    int32_t  int_param1;        // e.g. lens (0/1/2/3), torch (0/1), iso, ev, wb_mode, mic (0/1), mic_capsule, mic_stereo
    int64_t  long_param1;       // e.g. exposure_time_ns
    float    float_param1;      // e.g. focus_distance (0.0f..1.0f) or zoom_ratio (1.0f..10.0f)
} BouleCamCameraCmd;

/**
 * Camera Telemetry / State (Mobile -> Desktop) - 48 bytes
 */
typedef struct BouleCamCameraState {
    uint32_t magic;             // BOULECAM_MAGIC
    uint8_t  packet_type;       // BOULECAM_PKT_CAMERA_STATE
    uint8_t  current_lens;      // 0 = Back, 1 = Front, 2 = UltraWide, 3 = Tele/Macro
    uint8_t  torch_on;          // 0 = Off, 1 = On
    int32_t  current_iso;       // Current ISO
    int64_t  current_exposure_ns;// Current exposure in nanoseconds
    int32_t  current_ev;        // Current EV compensation
    uint8_t  current_wb;        // Current WB mode
    float    current_focus;     // Current focus distance
    uint8_t  mic_enabled;       // Mic status (0 = muted, 1 = active)
    float    battery_level;     // 0.0 - 100.0 (battery percentage)
    uint8_t  dim_screen_active; // 0 = Screen normal, 1 = Screen dimmed
    float    device_temperature;// Device temperature in Celsius (e.g. 33.5f)
    uint8_t  available_lenses_mask; // Bitmask: 1=Back, 2=Front, 4=UltraWide, 8=Tele/Macro
    float    current_zoom;      // Current zoom ratio (1.0f - 10.0f)
    uint8_t  mic_channels;      // 1 = Mono, 2 = Stereo
    uint8_t  mic_capsule;       // 0 = Auto/Default, 1 = Bottom, 2 = Back, 3 = Front
    uint8_t  mic_beamforming;   // 0 = Off, 1 = On
} BouleCamCameraState;

#pragma pack(pop)

#ifdef __cplusplus
}
#endif
