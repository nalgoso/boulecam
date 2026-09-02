#pragma once

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <cstdint>
#include <functional>
#include <vector>
#include "boulecam_protocol.h"
#include "boulecam_ipc.h"

namespace boulecam {

using DecodedFrameCallback = std::function<void(
    const uint8_t* pDecodedData,
    uint32_t dataSize,
    uint32_t width,
    uint32_t height,
    uint32_t stride,
    BouleCamPixelFormat pixelFormat,
    uint64_t captureTimestampUs,
    uint64_t decodedTimestampUs
)>;

class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();

    bool Initialize(uint32_t width, uint32_t height, BouleCamCodecType codecType, DecodedFrameCallback callback);
    void Shutdown();

    // Feeds raw NAL unit / Annex B byte stream
    bool DecodeNALU(const uint8_t* pData, uint32_t size, uint64_t captureTimestampUs);

private:
    bool SetupMFDecoder(uint32_t width, uint32_t height);
    void GenerateFallbackPattern(uint64_t captureTimestampUs);

    uint32_t m_width;
    uint32_t m_height;
    BouleCamCodecType m_codecType;
    DecodedFrameCallback m_callback;
    bool m_isInitialized;

    IMFTransform* m_pDecoderTransform;
    DWORD m_inputStreamId;
    DWORD m_outputStreamId;

    std::vector<uint8_t> m_fallbackNv12Buffer;
    uint32_t m_frameCounter;
};

} // namespace boulecam
