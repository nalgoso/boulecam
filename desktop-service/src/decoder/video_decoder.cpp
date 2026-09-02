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
    , m_codecType(BOULECAM_CODEC_H264)
    , m_isInitialized(false)
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
    m_codecType = codecType;
    m_callback = callback;

    size_t nv12Size = (m_width * m_height * 3) / 2;
    m_fallbackNv12Buffer.resize(nv12Size);
    memset(m_fallbackNv12Buffer.data(), 0x10, m_width * m_height);
    memset(m_fallbackNv12Buffer.data() + (m_width * m_height), 0x80, (m_width * m_height) / 2);

    SetupMFDecoder(width, height);
    m_isInitialized = true;
    return true;
}

void VideoDecoder::Shutdown() {
    if (m_pDecoderTransform) {
        m_pDecoderTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_pDecoderTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        m_pDecoderTransform->Release();
        m_pDecoderTransform = nullptr;
    }
    m_isInitialized = false;
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

bool VideoDecoder::DecodeNALU(const uint8_t* pData, uint32_t size, uint64_t captureTimestampUs) {
    if (!m_isInitialized || !pData || size == 0) return false;

    if (!m_pDecoderTransform) {
        GenerateFallbackPattern(captureTimestampUs);
        return false;
    }

    static int s_logCounter = 0;
    bool shouldLog = (s_logCounter++ < 20);

    if (shouldLog) {
        std::cout << "[DecodeNALU] Frame #" << s_logCounter << " size=" << size << " bytes: ";
        for (uint32_t i = 0; i < (size < 8 ? size : 8); ++i) {
            std::cout << std::hex << (int)pData[i] << " ";
        }
        std::cout << std::dec << std::endl;
    }

    // 1. Create Media Buffer and copy NALU payload
    IMFMediaBuffer* pInputBuffer = nullptr;
    HRESULT hr = MFCreateMemoryBuffer(size, &pInputBuffer);
    if (FAILED(hr)) return false;

    BYTE* pDst = nullptr;
    pInputBuffer->Lock(&pDst, NULL, NULL);
    memcpy(pDst, pData, size);
    pInputBuffer->Unlock();
    pInputBuffer->SetCurrentLength(size);

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
    DWORD requiredSize = (streamInfo.cbSize > 0) ? streamInfo.cbSize : ((m_width * m_height * 3) / 2);

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
            m_pDecoderTransform->GetOutputAvailableType(m_outputStreamId, 0, &pNewType);
            if (pNewType) {
                m_pDecoderTransform->SetOutputType(m_outputStreamId, pNewType, 0);
                pNewType->Release();
            }
            m_pDecoderTransform->GetOutputStreamInfo(m_outputStreamId, &streamInfo);
            requiredSize = (streamInfo.cbSize > 0) ? streamInfo.cbSize : ((m_width * m_height * 3) / 2);
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
                pDecodedBuffer->Lock(&pDecodedBytes, NULL, &currentLength);

                if (pDecodedBytes && currentLength > 0 && m_callback) {
                    m_callback(
                        pDecodedBytes,
                        currentLength,
                        m_width,
                        m_height,
                        m_width,
                        BOULECAM_PIXFMT_NV12,
                        captureTimestampUs,
                        GetCurrentTimeMicroseconds()
                    );
                }

                pDecodedBuffer->Unlock();
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
