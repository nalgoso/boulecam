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

using NeedKeyframeCallback = std::function<void(uint32_t droppedFrames, bool missingSpsPps)>;

class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();

    bool Initialize(uint32_t width, uint32_t height, BouleCamCodecType codecType, DecodedFrameCallback callback);
    void Shutdown();
    void ResetStreamSync();
    void SetNeedKeyframeCallback(NeedKeyframeCallback callback) { m_needKeyframeCallback = callback; }

    // Feeds raw NAL unit / Annex B byte stream
    bool DecodeNALU(const uint8_t* pData, uint32_t size, uint64_t captureTimestampUs);

private:
    bool SetupMFDecoder(uint32_t width, uint32_t height);
    void GenerateFallbackPattern(uint64_t captureTimestampUs);

    uint32_t m_width;
    uint32_t m_height;
    uint32_t m_outputStride;
    uint32_t m_outputSliceHeight;
    BouleCamCodecType m_codecType;
    DecodedFrameCallback m_callback;
    NeedKeyframeCallback m_needKeyframeCallback;
    bool m_isInitialized;
    bool m_hasSyncedKeyframe;
    uint32_t m_droppedPreSyncFrames;

    IMFTransform* m_pDecoderTransform;
    DWORD m_inputStreamId;
    DWORD m_outputStreamId;

    std::vector<uint8_t> m_spsNalu;
    std::vector<uint8_t> m_ppsNalu;
    std::vector<uint8_t> m_prependedInputBuffer;
    std::vector<uint8_t> m_fallbackNv12Buffer;
    std::vector<uint8_t> m_compactBuffer;
    uint32_t m_frameCounter;
};

} // namespace boulecam
