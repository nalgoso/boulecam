#include "virtual_camera_source.h"
#include <iostream>

namespace boulecam {

VirtualCameraSource::VirtualCameraSource()
    : m_cRef(1)
    , m_pEventQueue(nullptr)
    , m_pPresentationDescriptor(nullptr)
    , m_pStream(nullptr)
    , m_isShutdown(false) {
    
    MFCreateEventQueue(&m_pEventQueue);
    CreateDescriptors();
}

VirtualCameraSource::~VirtualCameraSource() {
    Shutdown();
    if (m_pStream) {
        m_pStream->Release();
        m_pStream = nullptr;
    }
    if (m_pPresentationDescriptor) {
        m_pPresentationDescriptor->Release();
        m_pPresentationDescriptor = nullptr;
    }
    if (m_pEventQueue) {
        m_pEventQueue->Release();
        m_pEventQueue = nullptr;
    }
}

STDMETHODIMP VirtualCameraSource::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IMFMediaEventGenerator || riid == IID_IMFMediaSource) {
        *ppv = static_cast<IMFMediaSource*>(this);
        AddRef();
        return S_OK;
    } else if (riid == IID_IMFGetService) {
        *ppv = static_cast<IMFGetService*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) VirtualCameraSource::AddRef() {
    return ++m_cRef;
}

STDMETHODIMP_(ULONG) VirtualCameraSource::Release() {
    ULONG ref = --m_cRef;
    if (ref == 0) delete this;
    return ref;
}

STDMETHODIMP VirtualCameraSource::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) {
    if (m_isShutdown) return MF_E_SHUTDOWN;
    return m_pEventQueue ? m_pEventQueue->GetEvent(dwFlags, ppEvent) : E_UNEXPECTED;
}

STDMETHODIMP VirtualCameraSource::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState) {
    if (m_isShutdown) return MF_E_SHUTDOWN;
    return m_pEventQueue ? m_pEventQueue->BeginGetEvent(pCallback, punkState) : E_UNEXPECTED;
}

STDMETHODIMP VirtualCameraSource::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) {
    if (m_isShutdown) return MF_E_SHUTDOWN;
    return m_pEventQueue ? m_pEventQueue->EndGetEvent(pResult, ppEvent) : E_UNEXPECTED;
}

STDMETHODIMP VirtualCameraSource::QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue) {
    if (m_isShutdown) return MF_E_SHUTDOWN;
    return m_pEventQueue ? m_pEventQueue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue) : E_UNEXPECTED;
}

STDMETHODIMP VirtualCameraSource::GetCharacteristics(DWORD* pdwCharacteristics) {
    if (!pdwCharacteristics) return E_POINTER;
    if (m_isShutdown) return MF_E_SHUTDOWN;
    // Live camera source characteristics: MFMEDIASOURCE_IS_LIVE
    *pdwCharacteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

HRESULT VirtualCameraSource::CreateDescriptors() {
    // 1. Create Media Types supported by Virtual Camera (1080p60 NV12, 720p60 NV12)
    IMFMediaType* pMediaType1080p = nullptr;
    MFCreateMediaType(&pMediaType1080p);
    pMediaType1080p->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pMediaType1080p->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(pMediaType1080p, MF_MT_FRAME_SIZE, 1920, 1080);
    MFSetAttributeRatio(pMediaType1080p, MF_MT_FRAME_RATE, 60, 1);
    MFSetAttributeRatio(pMediaType1080p, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    pMediaType1080p->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    IMFMediaType* pMediaType720p = nullptr;
    MFCreateMediaType(&pMediaType720p);
    pMediaType720p->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pMediaType720p->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(pMediaType720p, MF_MT_FRAME_SIZE, 1280, 720);
    MFSetAttributeRatio(pMediaType720p, MF_MT_FRAME_RATE, 60, 1);
    MFSetAttributeRatio(pMediaType720p, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    pMediaType720p->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    IMFMediaType* mediaTypes[] = { pMediaType1080p, pMediaType720p };

    // 2. Create Stream Descriptor
    IMFStreamDescriptor* pSD = nullptr;
    MFCreateStreamDescriptor(0, 2, mediaTypes, &pSD);
    pMediaType1080p->Release();
    pMediaType720p->Release();

    // 3. Create Presentation Descriptor
    IMFStreamDescriptor* streamDescriptors[] = { pSD };
    MFCreatePresentationDescriptor(1, streamDescriptors, &m_pPresentationDescriptor);
    m_pPresentationDescriptor->SelectStream(0);

    // 4. Instantiate Virtual Camera Stream
    m_pStream = new VirtualCameraStream(this, pSD);
    pSD->Release();

    return S_OK;
}

STDMETHODIMP VirtualCameraSource::CreatePresentationDescriptor(IMFPresentationDescriptor** ppPresentationDescriptor) {
    if (!ppPresentationDescriptor) return E_POINTER;
    if (m_isShutdown) return MF_E_SHUTDOWN;
    if (m_pPresentationDescriptor) {
        return m_pPresentationDescriptor->Clone(ppPresentationDescriptor);
    }
    return E_FAIL;
}

STDMETHODIMP VirtualCameraSource::Start(IMFPresentationDescriptor* pPresentationDescriptor, const GUID* pguidTimeFormat, const PROPVARIANT* pvarStartPosition) {
    if (m_isShutdown) return MF_E_SHUTDOWN;

    if (m_pStream) {
        m_pStream->Start();
    }

    if (m_pEventQueue) {
        PROPVARIANT var;
        PropVariantInit(&var);
        m_pEventQueue->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &var);
    }
    return S_OK;
}

STDMETHODIMP VirtualCameraSource::Stop() {
    if (m_isShutdown) return MF_E_SHUTDOWN;

    if (m_pStream) {
        m_pStream->Stop();
    }

    if (m_pEventQueue) {
        PROPVARIANT var;
        PropVariantInit(&var);
        m_pEventQueue->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, &var);
    }
    return S_OK;
}

STDMETHODIMP VirtualCameraSource::Pause() {
    return MF_E_INVALIDREQUEST; // Live camera streams do not pause
}

STDMETHODIMP VirtualCameraSource::Shutdown() {
    m_isShutdown = true;
    if (m_pStream) {
        m_pStream->Shutdown();
    }
    if (m_pEventQueue) {
        m_pEventQueue->Shutdown();
    }
    return S_OK;
}

STDMETHODIMP VirtualCameraSource::GetService(REFGUID guidService, REFIID riid, LPVOID* ppvObject) {
    return E_NOTIMPL;
}

} // namespace boulecam
