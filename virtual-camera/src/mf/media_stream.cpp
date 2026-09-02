#include "media_stream.h"
#include "virtual_camera_source.h"
#include <iostream>

namespace boulecam {

VirtualCameraStream::VirtualCameraStream(VirtualCameraSource* pSource, IMFStreamDescriptor* pSD)
    : m_cRef(1)
    , m_pSource(pSource)
    , m_pStreamDescriptor(pSD)
    , m_pEventQueue(nullptr)
    , m_sampleTimestamp100ns(0)
    , m_isStreaming(false)
    , m_isShutdown(false) {
    
    if (m_pStreamDescriptor) m_pStreamDescriptor->AddRef();
    MFCreateEventQueue(&m_pEventQueue);

    // Allocate frame buffer (4K NV12 maximum size: ~12.5 MB)
    m_frameBuffer.resize(3840 * 2160 * 2, 0x10);

    m_shmConsumer.Open();
}

VirtualCameraStream::~VirtualCameraStream() {
    Shutdown();
    if (m_pStreamDescriptor) {
        m_pStreamDescriptor->Release();
        m_pStreamDescriptor = nullptr;
    }
    if (m_pEventQueue) {
        m_pEventQueue->Release();
        m_pEventQueue = nullptr;
    }
}

STDMETHODIMP VirtualCameraStream::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IMFMediaEventGenerator || riid == IID_IMFMediaStream) {
        *ppv = static_cast<IMFMediaStream*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) VirtualCameraStream::AddRef() {
    return ++m_cRef;
}

STDMETHODIMP_(ULONG) VirtualCameraStream::Release() {
    ULONG ref = --m_cRef;
    if (ref == 0) delete this;
    return ref;
}

STDMETHODIMP VirtualCameraStream::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) {
    if (m_isShutdown) return MF_E_SHUTDOWN;
    return m_pEventQueue ? m_pEventQueue->GetEvent(dwFlags, ppEvent) : E_UNEXPECTED;
}

STDMETHODIMP VirtualCameraStream::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState) {
    if (m_isShutdown) return MF_E_SHUTDOWN;
    return m_pEventQueue ? m_pEventQueue->BeginGetEvent(pCallback, punkState) : E_UNEXPECTED;
}

STDMETHODIMP VirtualCameraStream::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) {
    if (m_isShutdown) return MF_E_SHUTDOWN;
    return m_pEventQueue ? m_pEventQueue->EndGetEvent(pResult, ppEvent) : E_UNEXPECTED;
}

STDMETHODIMP VirtualCameraStream::QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue) {
    if (m_isShutdown) return MF_E_SHUTDOWN;
    return m_pEventQueue ? m_pEventQueue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue) : E_UNEXPECTED;
}

STDMETHODIMP VirtualCameraStream::GetMediaSource(IMFMediaSource** ppMediaSource) {
    if (!ppMediaSource) return E_POINTER;
    if (m_isShutdown) return MF_E_SHUTDOWN;
    if (m_pSource) {
        *ppMediaSource = (IMFMediaSource*)m_pSource;
        (*ppMediaSource)->AddRef();
        return S_OK;
    }
    return E_FAIL;
}

STDMETHODIMP VirtualCameraStream::GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor) {
    if (!ppStreamDescriptor) return E_POINTER;
    if (m_isShutdown) return MF_E_SHUTDOWN;
    if (m_pStreamDescriptor) {
        *ppStreamDescriptor = m_pStreamDescriptor;
        m_pStreamDescriptor->AddRef();
        return S_OK;
    }
    return E_FAIL;
}

STDMETHODIMP VirtualCameraStream::RequestSample(IUnknown* pToken) {
    if (m_isShutdown) return MF_E_SHUTDOWN;
    if (!m_isStreaming) return MF_E_INVALIDREQUEST;

    DeliverFrameSample();
    return S_OK;
}

void VirtualCameraStream::DeliverFrameSample() {
    uint32_t width = 1920, height = 1080, stride = 1920;
    BouleCamPixelFormat pixFmt = BOULECAM_PIXFMT_NV12;

    bool hasFrame = m_shmConsumer.ReadLatestFrame(
        m_frameBuffer.data(),
        static_cast<uint32_t>(m_frameBuffer.size()),
        width,
        height,
        stride,
        pixFmt,
        15 // 15ms wait timeout for fresh frame
    );

    uint32_t frameSize = (width * height * 3) / 2; // NV12
    if (!hasFrame) {
        // Generate placeholder dark test screen if stream temporarily paused
        memset(m_frameBuffer.data(), 0x10, width * height);
        memset(m_frameBuffer.data() + (width * height), 0x80, (width * height) / 2);
    }

    // Create IMFSample
    IMFSample* pSample = nullptr;
    HRESULT hr = MFCreateSample(&pSample);
    if (FAILED(hr) || !pSample) return;

    IMFMediaBuffer* pBuffer = nullptr;
    hr = MFCreateMemoryBuffer(frameSize, &pBuffer);
    if (SUCCEEDED(hr) && pBuffer) {
        BYTE* pData = nullptr;
        DWORD maxLen = 0, curLen = 0;
        if (SUCCEEDED(pBuffer->Lock(&pData, &maxLen, &curLen))) {
            memcpy(pData, m_frameBuffer.data(), frameSize);
            pBuffer->Unlock();
            pBuffer->SetCurrentLength(frameSize);
        }
        pSample->AddBuffer(pBuffer);
        pBuffer->Release();
    }

    // Set sample presentation time & duration (60 FPS: ~166,666 units of 100ns)
    LONGLONG duration100ns = 166666;
    pSample->SetSampleTime(m_sampleTimestamp100ns);
    pSample->SetSampleDuration(duration100ns);
    m_sampleTimestamp100ns += duration100ns;

    // Dispatch sample event to host application (Zoom, Teams, Meet, OBS)
    m_pEventQueue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, pSample);
    pSample->Release();
}

HRESULT VirtualCameraStream::Start() {
    m_isStreaming = true;
    m_sampleTimestamp100ns = 0;
    if (m_pEventQueue) {
        PROPVARIANT var;
        PropVariantInit(&var);
        m_pEventQueue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, &var);
    }
    return S_OK;
}

HRESULT VirtualCameraStream::Stop() {
    m_isStreaming = false;
    if (m_pEventQueue) {
        PROPVARIANT var;
        PropVariantInit(&var);
        m_pEventQueue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, &var);
    }
    return S_OK;
}

HRESULT VirtualCameraStream::Shutdown() {
    m_isShutdown = true;
    m_isStreaming = false;
    if (m_pEventQueue) {
        m_pEventQueue->Shutdown();
    }
    m_shmConsumer.Close();
    return S_OK;
}

} // namespace boulecam
