#include "video_decoder.h"
#include <iostream>
#include <chrono>
#include <mferror.h>
#include <codecapi.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "wmcodecdspuuid.lib")
#pragma comment(lib, "strmiids.lib")

namespace boulecam {

static uint64_t GetCurrentTimeMicroseconds() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
}

VideoDecoder::VideoDecoder()
    : m_width(1920)
    , m_height(1080)
    , m_outputStride(1920)
    , m_outputSliceHeight(1080)
    , m_codecType(BOULECAM_CODEC_H264)
    , m_isInitialized(false)
    , m_hasSyncedKeyframe(false)
    , m_droppedPreSyncFrames(0)
    , m_pDecoderTransform(nullptr)
    , m_inputStreamId(0)
    , m_outputStreamId(0)
    , m_frameCounter(0) {
    
    MFStartup(MF_VERSION);
}

VideoDecoder::~VideoDecoder() {
    Shutdown();
    MFShutdown();
}

bool VideoDecoder::Initialize(uint32_t width, uint32_t height, BouleCamCodecType codecType, DecodedFrameCallback callback) {
    m_width = width;
    m_height = height;
    m_outputStride = width;
    m_outputSliceHeight = height;
    m_codecType = codecType;
    m_callback = callback;
    m_hasSyncedKeyframe = false;
    m_droppedPreSyncFrames = 0;

    size_t nv12Size = (m_width * m_height * 3) / 2;
    m_fallbackNv12Buffer.resize(nv12Size);
    memset(m_fallbackNv12Buffer.data(), 0x10, m_width * m_height);
    memset(m_fallbackNv12Buffer.data() + (m_width * m_height), 0x80, (m_width * m_height) / 2);

    SetupMFDecoder(width, height);
    m_isInitialized = true;
    return true;
}

void VideoDecoder::ResetStreamSync() {
    m_hasSyncedKeyframe = false;
    m_droppedPreSyncFrames = 0;
    if (m_pDecoderTransform) {
        m_pDecoderTransform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        m_pDecoderTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        m_pDecoderTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }
}

void VideoDecoder::Shutdown() {
    if (m_pDecoderTransform) {
        m_pDecoderTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_pDecoderTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        m_pDecoderTransform->Release();
        m_pDecoderTransform = nullptr;
    }
    m_isInitialized = false;
    m_hasSyncedKeyframe = false;
}

bool VideoDecoder::SetupMFDecoder(uint32_t width, uint32_t height) {
    // Create Media Foundation H.264 Video Decoder MFT
    HRESULT hr = CoCreateInstance(
        CLSID_CMSH264DecoderMFT,
        NULL,
        CLSCTX_INPROC_SERVER,
        IID_IMFTransform,
        (void**)&m_pDecoderTransform
    );

    if (FAILED(hr) || !m_pDecoderTransform) {
        std::cerr << "[VideoDecoder] Failed to create Media Foundation H.264 MFT: 0x" 
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    // Set Input Media Type (H.264)
    IMFMediaType* pInputType = nullptr;
    MFCreateMediaType(&pInputType);
    pInputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pInputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(pInputType, MF_MT_FRAME_SIZE, width, height);

    hr = m_pDecoderTransform->SetInputType(m_inputStreamId, pInputType, 0);
    pInputType->Release();

    if (FAILED(hr)) {
        std::cerr << "[VideoDecoder] SetInputType failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    // Set Output Media Type (NV12)
    IMFMediaType* pOutputType = nullptr;
    MFCreateMediaType(&pOutputType);
    pOutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pOutputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(pOutputType, MF_MT_FRAME_SIZE, width, height);

    hr = m_pDecoderTransform->SetOutputType(m_outputStreamId, pOutputType, 0);
    pOutputType->Release();

    if (FAILED(hr)) {
        std::cerr << "[VideoDecoder] SetOutputType failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    // Enable Media Foundation Hardware Low Latency Mode (Zero-frame buffer)
    ICodecAPI* pCodecApi = nullptr;
    if (SUCCEEDED(m_pDecoderTransform->QueryInterface(IID_ICodecAPI, (void**)&pCodecApi))) {
        VARIANT var;
        VariantInit(&var);
        var.vt = VT_UI4;
        var.ulVal = 1;
        pCodecApi->SetValue(&CODECAPI_AVLowLatencyMode, &var);
        pCodecApi->Release();
        std::cout << "[VideoDecoder] Hardware Low-Latency Mode ENABLED (Zero Buffering)" << std::endl;
    }

    // Notify streaming start
    m_pDecoderTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_pDecoderTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    std::cout << "[VideoDecoder] Hardware-accelerated H.264 decoder active (" << width << "x" << height << ")" << std::endl;
    return true;
}

static uint32_t UnpadH264Dimension(uint32_t dim) {
    if (dim == 1088) return 1080;
    if (dim == 736) return 720;
    if (dim == 2176) return 2160;
    return dim;
}

bool VideoDecoder::DecodeNALU(const uint8_t* pData, uint32_t size, uint64_t captureTimestampUs) {
    if (!m_isInitialized || !pData || size == 0) return false;

    if (!m_pDecoderTransform) {
        GenerateFallbackPattern(captureTimestampUs);
        return false;
    }

    // 0. Inspect Annex-B NAL units for SPS (7), PPS (8), IDR (5), and Non-IDR (1)
    bool hasSps = false;
    bool hasPps = false;
    bool hasIdr = false;
    bool hasNonIdr = false;

    struct NaluSpan {
        uint32_t startOffset;
        uint32_t headerOffset;
        uint32_t endOffset;
    };
    std::vector<NaluSpan> spans;

    uint32_t i = 0;
    while (i + 3 < size) {
        if (pData[i] == 0x00 && pData[i + 1] == 0x00) {
            if (pData[i + 2] == 0x01) {
                if (!spans.empty()) spans.back().endOffset = i;
                spans.push_back({ i, i + 3, size });
                i += 3;
                continue;
            } else if (i + 4 < size && pData[i + 2] == 0x00 && pData[i + 3] == 0x01) {
                if (!spans.empty()) spans.back().endOffset = i;
                spans.push_back({ i, i + 4, size });
                i += 4;
                continue;
            }
        }
        ++i;
    }

    for (const auto& sp : spans) {
        if (sp.headerOffset < sp.endOffset) {
            uint8_t nalType = pData[sp.headerOffset] & 0x1F;
            if (nalType == 7) {
                hasSps = true;
                m_spsNalu.assign(pData + sp.startOffset, pData + sp.endOffset);
            } else if (nalType == 8) {
                hasPps = true;
                m_ppsNalu.assign(pData + sp.startOffset, pData + sp.endOffset);
            } else if (nalType == 5) {
                hasIdr = true;
            } else if (nalType == 1) {
                hasNonIdr = true;
            }
        }
    }

    bool haveCachedSpsPps = (!m_spsNalu.empty() && !m_ppsNalu.empty());

    // Gate 1: Never feed slices into CLSID_CMSH264DecoderMFT before SPS + PPS are known!
    // Feeding P-frames or bare IDR frames without SPS/PPS causes MFT to output a green macroblock wall.
    if (!haveCachedSpsPps) {
        m_droppedPreSyncFrames++;
        if (m_needKeyframeCallback) {
            m_needKeyframeCallback(m_droppedPreSyncFrames, true);
        }
        return false;
    }

    // Gate 2: Before the first valid IDR sync point, drop orphan P-frames (NAL type 1)
    if (!m_hasSyncedKeyframe) {
        if (hasIdr) {
            m_hasSyncedKeyframe = true;
            m_droppedPreSyncFrames = 0;
        } else if (hasNonIdr && !hasSps && !hasPps) {
            m_droppedPreSyncFrames++;
            if (m_needKeyframeCallback) {
                m_needKeyframeCallback(m_droppedPreSyncFrames, false);
            }
            return false;
        }
    }

    // Ensure every IDR keyframe fed to MFT carries SPS + PPS immediately before it
    const uint8_t* pInputBytes = pData;
    uint32_t inputSize = size;
    if (hasIdr && (!hasSps || !hasPps) && haveCachedSpsPps) {
        m_prependedInputBuffer.clear();
        m_prependedInputBuffer.reserve(m_spsNalu.size() + m_ppsNalu.size() + size);
        m_prependedInputBuffer.insert(m_prependedInputBuffer.end(), m_spsNalu.begin(), m_spsNalu.end());
        m_prependedInputBuffer.insert(m_prependedInputBuffer.end(), m_ppsNalu.begin(), m_ppsNalu.end());
        m_prependedInputBuffer.insert(m_prependedInputBuffer.end(), pData, pData + size);
        pInputBytes = m_prependedInputBuffer.data();
        inputSize = static_cast<uint32_t>(m_prependedInputBuffer.size());
    }

    static int s_logCounter = 0;
    bool shouldLog = (s_logCounter++ < 20);

    if (shouldLog) {
        std::cout << "[DecodeNALU] Frame #" << s_logCounter << " size=" << inputSize << " bytes: ";
        for (uint32_t k = 0; k < (inputSize < 8 ? inputSize : 8); ++k) {
            std::cout << std::hex << (int)pInputBytes[k] << " ";
        }
        std::cout << std::dec << std::endl;
    }

    // 1. Create Media Buffer and copy NALU payload
    IMFMediaBuffer* pInputBuffer = nullptr;
    HRESULT hr = MFCreateMemoryBuffer(inputSize, &pInputBuffer);
    if (FAILED(hr)) return false;

    BYTE* pDst = nullptr;
    pInputBuffer->Lock(&pDst, NULL, NULL);
    memcpy(pDst, pInputBytes, inputSize);
    pInputBuffer->Unlock();
    pInputBuffer->SetCurrentLength(inputSize);

    // 2. Create Sample and attach Buffer
    IMFSample* pInputSample = nullptr;
    hr = MFCreateSample(&pInputSample);
    if (FAILED(hr)) {
        pInputBuffer->Release();
        return false;
    }

    pInputSample->AddBuffer(pInputBuffer);
    pInputSample->SetSampleTime(static_cast<LONGLONG>(captureTimestampUs * 10)); // 100ns units

    // 3. Process Input into Decoder MFT
    hr = m_pDecoderTransform->ProcessInput(m_inputStreamId, pInputSample, 0);
    pInputBuffer->Release();
    pInputSample->Release();

    if (shouldLog) {
        std::cout << "[DecodeNALU] ProcessInput hr=0x" << std::hex << hr << std::dec << std::endl;
    }

    if (FAILED(hr)) {
        return false;
    }

    // 4. Check Output Stream Info
    MFT_OUTPUT_STREAM_INFO streamInfo{};
    hr = m_pDecoderTransform->GetOutputStreamInfo(m_outputStreamId, &streamInfo);
    DWORD requiredSize = (streamInfo.cbSize > 0) ? streamInfo.cbSize : ((m_outputStride * m_outputSliceHeight * 3) / 2);

    // 5. Drain Output Samples from Decoder MFT
    while (true) {
        MFT_OUTPUT_DATA_BUFFER outputDataBuffer{};
        outputDataBuffer.dwStreamID = m_outputStreamId;

        IMFSample* pOutputSample = nullptr;
        IMFMediaBuffer* pOutputMediaBuffer = nullptr;

        bool mftProvidesSamples = (streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;

        if (!mftProvidesSamples) {
            MFCreateSample(&pOutputSample);
            MFCreateMemoryBuffer(requiredSize, &pOutputMediaBuffer);
            pOutputSample->AddBuffer(pOutputMediaBuffer);
            outputDataBuffer.pSample = pOutputSample;
        }

        DWORD dwStatus = 0;
        hr = m_pDecoderTransform->ProcessOutput(0, 1, &outputDataBuffer, &dwStatus);

        if (shouldLog) {
            std::cout << "[DecodeNALU] ProcessOutput hr=0x" << std::hex << hr << std::dec 
                      << " providesSamples=" << mftProvidesSamples 
                      << " reqSize=" << requiredSize << std::endl;
        }

        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            IMFMediaType* pNewType = nullptr;
            for (DWORD typeIdx = 0; ; ++typeIdx) {
                IMFMediaType* pCandidate = nullptr;
                if (FAILED(m_pDecoderTransform->GetOutputAvailableType(m_outputStreamId, typeIdx, &pCandidate)) || !pCandidate) {
                    break;
                }
                GUID subtype = GUID_NULL;
                if (SUCCEEDED(pCandidate->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == MFVideoFormat_NV12) {
                    pNewType = pCandidate;
                    break;
                }
                if (typeIdx == 0) {
                    pNewType = pCandidate;
                } else {
                    pCandidate->Release();
                }
            }
            if (pNewType) {
                UINT32 codedW = 0, codedH = 0;
                if (SUCCEEDED(MFGetAttributeSize(pNewType, MF_MT_FRAME_SIZE, &codedW, &codedH)) && codedW > 0 && codedH > 0) {
                    uint32_t visW = UnpadH264Dimension(codedW);
                    uint32_t visH = UnpadH264Dimension(codedH);

                    MFVideoArea aperture{};
                    if (SUCCEEDED(pNewType->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8*)&aperture, sizeof(aperture), NULL)) ||
                        SUCCEEDED(pNewType->GetBlob(MF_MT_GEOMETRIC_APERTURE, (UINT8*)&aperture, sizeof(aperture), NULL))) {
                        if (aperture.Area.cx > 0 && aperture.Area.cy > 0 &&
                            static_cast<uint32_t>(aperture.Area.cx) <= codedW &&
                            static_cast<uint32_t>(aperture.Area.cy) <= codedH) {
                            visW = static_cast<uint32_t>(aperture.Area.cx);
                            visH = static_cast<uint32_t>(aperture.Area.cy);
                        }
                    }

                    m_width = visW;
                    m_height = visH;
                    m_outputSliceHeight = (codedH >= visH) ? codedH : visH;

                    INT32 defaultStride = 0;
                    if (SUCCEEDED(pNewType->GetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32*)&defaultStride)) && defaultStride > 0 &&
                        static_cast<uint32_t>(defaultStride) >= m_width) {
                        m_outputStride = static_cast<uint32_t>(defaultStride);
                    } else {
                        m_outputStride = (codedW >= m_width) ? codedW : m_width;
                    }
                }
                m_pDecoderTransform->SetOutputType(m_outputStreamId, pNewType, 0);
                pNewType->Release();
            }
            m_pDecoderTransform->GetOutputStreamInfo(m_outputStreamId, &streamInfo);
            requiredSize = (streamInfo.cbSize > 0) ? streamInfo.cbSize : ((m_outputStride * m_outputSliceHeight * 3) / 2);
            if (pOutputMediaBuffer) pOutputMediaBuffer->Release();
            if (pOutputSample) pOutputSample->Release();
            continue;
        }

        if (FAILED(hr)) {
            if (pOutputMediaBuffer) pOutputMediaBuffer->Release();
            if (pOutputSample) pOutputSample->Release();
            break;
        }

        // Output Sample ready
        IMFSample* pReadySample = mftProvidesSamples ? outputDataBuffer.pSample : pOutputSample;
        if (pReadySample) {
            IMFMediaBuffer* pDecodedBuffer = nullptr;
            pReadySample->GetBufferByIndex(0, &pDecodedBuffer);
            if (pDecodedBuffer) {
                BYTE* pDecodedBytes = nullptr;
                DWORD currentLength = 0;
                uint32_t actualStride = (m_outputStride >= m_width) ? m_outputStride : m_width;

                IMF2DBuffer* p2DBuffer = nullptr;
                LONG pitch2D = 0;
                if (SUCCEEDED(pDecodedBuffer->QueryInterface(IID_IMF2DBuffer, (void**)&p2DBuffer)) && p2DBuffer) {
                    if (SUCCEEDED(p2DBuffer->Lock2D(&pDecodedBytes, &pitch2D)) && pitch2D > 0) {
                        actualStride = static_cast<uint32_t>(pitch2D);
                        pDecodedBuffer->GetCurrentLength(&currentLength);
                    } else {
                        p2DBuffer->Release();
                        p2DBuffer = nullptr;
                    }
                }
                if (!p2DBuffer) {
                    pDecodedBuffer->Lock(&pDecodedBytes, NULL, &currentLength);
                }

                if (pDecodedBytes && currentLength > 0 && m_callback) {
                    uint32_t exactSize = m_width * m_height * 3 / 2;
                    uint32_t sliceHeight = m_outputSliceHeight;
                    if (actualStride > 0 && currentLength >= (actualStride * m_height * 3) / 2) {
                        uint32_t inferredSliceH = (currentLength * 2) / (actualStride * 3);
                        if (inferredSliceH >= m_height) {
                            sliceHeight = inferredSliceH;
                        }
                    }

                    const uint8_t* pFinalNv12 = pDecodedBytes;
                    if (actualStride != m_width || sliceHeight != m_height || currentLength != exactSize) {
                        if (m_compactBuffer.size() < exactSize) {
                            m_compactBuffer.resize(exactSize);
                        }
                        uint8_t* dstY = m_compactBuffer.data();
                        const uint8_t* srcY = pDecodedBytes;
                        for (uint32_t row = 0; row < m_height; ++row) {
                            memcpy(dstY + row * m_width, srcY + row * actualStride, m_width);
                        }

                        uint8_t* dstUV = m_compactBuffer.data() + (m_width * m_height);
                        const uint8_t* srcUV = pDecodedBytes + (actualStride * sliceHeight);
                        uint32_t uvRows = m_height / 2;
                        for (uint32_t row = 0; row < uvRows; ++row) {
                            memcpy(dstUV + row * m_width, srcUV + row * actualStride, m_width);
                        }
                        pFinalNv12 = m_compactBuffer.data();
                    }

                    // Mathematical UV Chroma Guard:
                    // For every valid RGB color in [0..255]^3, BT.601/709 UV satisfies U + V >= 64.
                    // Uninitialized / corrupted MFT surfaces have zeroed UV bytes (U < 16 && V < 16 -> neon green wall).
                    const uint8_t* uvPlane = pFinalNv12 + (m_width * m_height);
                    uint32_t uvLen = (m_width * m_height) / 2;
                    uint32_t corruptSamples = 0;
                    const uint32_t kNumSamples = 64;
                    uint32_t step = (uvLen / (kNumSamples * 2)) * 2;
                    if (step >= 2) {
                        for (uint32_t s = 0; s < kNumSamples; ++s) {
                            uint32_t idx = s * step;
                            if (idx + 1 < uvLen) {
                                if (uvPlane[idx] < 16 && uvPlane[idx + 1] < 16) {
                                    corruptSamples++;
                                }
                            }
                        }
                    }

                    if (corruptSamples > (kNumSamples / 2)) {
                        std::cerr << "[VideoDecoder] Corrupted green NV12 surface detected (" << corruptSamples
                                  << "/" << kNumSamples << " zero-chroma samples). Flushing MFT and requesting sync frame." << std::endl;
                        if (p2DBuffer) {
                            p2DBuffer->Unlock2D();
                            p2DBuffer->Release();
                        } else {
                            pDecodedBuffer->Unlock();
                        }
                        pDecodedBuffer->Release();
                        if (outputDataBuffer.pEvents) outputDataBuffer.pEvents->Release();
                        if (pOutputMediaBuffer) pOutputMediaBuffer->Release();
                        if (pOutputSample) pOutputSample->Release();
                        if (mftProvidesSamples && outputDataBuffer.pSample) outputDataBuffer.pSample->Release();
                        ResetStreamSync();
                        if (m_needKeyframeCallback) {
                            m_needKeyframeCallback(20, false);
                        }
                        return false;
                    }

                    m_callback(
                        pFinalNv12,
                        exactSize,
                        m_width,
                        m_height,
                        m_width,
                        BOULECAM_PIXFMT_NV12,
                        captureTimestampUs,
                        GetCurrentTimeMicroseconds()
                    );
                }

                if (p2DBuffer) {
                    p2DBuffer->Unlock2D();
                    p2DBuffer->Release();
                } else {
                    pDecodedBuffer->Unlock();
                }
                pDecodedBuffer->Release();
            }
        }

        if (outputDataBuffer.pEvents) outputDataBuffer.pEvents->Release();
        if (pOutputMediaBuffer) pOutputMediaBuffer->Release();
        if (pOutputSample) pOutputSample->Release();
        if (mftProvidesSamples && outputDataBuffer.pSample) outputDataBuffer.pSample->Release();
    }

    return true;
}

void VideoDecoder::GenerateFallbackPattern(uint64_t captureTimestampUs) {
    if (!m_callback) return;
    uint32_t dataSize = static_cast<uint32_t>((m_width * m_height * 3) / 2);
    m_callback(
        m_fallbackNv12Buffer.data(),
        dataSize,
        m_width,
        m_height,
        m_width,
        BOULECAM_PIXFMT_NV12,
        captureTimestampUs,
        GetCurrentTimeMicroseconds()
    );
}

} // namespace boulecam
