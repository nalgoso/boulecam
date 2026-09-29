#include "dshow_filter.h"
#include <iostream>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <condition_variable>

namespace boulecam {

// =========================================================================
// CUSTOM DIRECTSHOW MEMORY ALLOCATOR & MEDIA SAMPLE
// =========================================================================

static void FreeMediaTypeHelper(AM_MEDIA_TYPE& mt) {
    if (mt.cbFormat > 0 && mt.pbFormat) {
        CoTaskMemFree(mt.pbFormat);
        mt.pbFormat = nullptr;
        mt.cbFormat = 0;
    }
    if (mt.pUnk) {
        mt.pUnk->Release();
        mt.pUnk = nullptr;
    }
}

static void CopyMediaTypeHelper(AM_MEDIA_TYPE* pDest, const AM_MEDIA_TYPE* pSrc) {
    if (!pDest || !pSrc) return;
    *pDest = *pSrc;
    if (pSrc->cbFormat > 0 && pSrc->pbFormat) {
        pDest->pbFormat = (BYTE*)CoTaskMemAlloc(pSrc->cbFormat);
        memcpy(pDest->pbFormat, pSrc->pbFormat, pSrc->cbFormat);
    } else {
        pDest->pbFormat = nullptr;
    }
    if (pDest->pUnk) {
        pDest->pUnk->AddRef();
    }
}

class BouleCamMemAllocator;

class BouleCamMediaSample : public IMediaSample {
public:
    BouleCamMediaSample(BouleCamMemAllocator* pAllocator, BYTE* pBuffer, long bufferSize)
        : m_cRef(0)
        , m_pAllocator(pAllocator)
        , m_pBuffer(pBuffer)
        , m_bufferSize(bufferSize)
        , m_actualLength(bufferSize)
    {
    }

    virtual ~BouleCamMediaSample() {
        if (m_pMediaType) {
            FreeMediaTypeHelper(*m_pMediaType);
            CoTaskMemFree(m_pMediaType);
            m_pMediaType = nullptr;
        }
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IMediaSample) {
            *ppv = static_cast<IMediaSample*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return ++m_cRef;
    }

    STDMETHODIMP_(ULONG) Release() override;

    // IMediaSample
    STDMETHODIMP GetPointer(BYTE** ppBuffer) override {
        if (!ppBuffer) return E_POINTER;
        *ppBuffer = m_pBuffer;
        return S_OK;
    }

    STDMETHODIMP_(long) GetSize() override {
        return m_bufferSize;
    }

    STDMETHODIMP GetTime(REFERENCE_TIME* pTimeStart, REFERENCE_TIME* pTimeEnd) override {
        if (!pTimeStart || !pTimeEnd) return E_POINTER;
        if (!m_hasTime) return VFW_E_SAMPLE_TIME_NOT_SET;
        *pTimeStart = m_timeStart;
        *pTimeEnd = m_timeEnd;
        return S_OK;
    }

    STDMETHODIMP SetTime(REFERENCE_TIME* pTimeStart, REFERENCE_TIME* pTimeEnd) override {
        if (pTimeStart && pTimeEnd) {
            m_timeStart = *pTimeStart;
            m_timeEnd = *pTimeEnd;
            m_hasTime = true;
        } else {
            m_hasTime = false;
        }
        return S_OK;
    }

    STDMETHODIMP IsSyncPoint() override {
        return m_isSyncPoint ? S_OK : S_FALSE;
    }

    STDMETHODIMP SetSyncPoint(BOOL bIsSyncPoint) override {
        m_isSyncPoint = (bIsSyncPoint != FALSE);
        return S_OK;
    }

    STDMETHODIMP IsPreroll() override {
        return m_isPreroll ? S_OK : S_FALSE;
    }

    STDMETHODIMP SetPreroll(BOOL bIsPreroll) override {
        m_isPreroll = (bIsPreroll != FALSE);
        return S_OK;
    }

    STDMETHODIMP_(long) GetActualDataLength() override {
        return m_actualLength;
    }

    STDMETHODIMP SetActualDataLength(long lLen) override {
        if (lLen < 0 || lLen > m_bufferSize) return E_INVALIDARG;
        m_actualLength = lLen;
        return S_OK;
    }

    STDMETHODIMP GetMediaType(AM_MEDIA_TYPE** ppMediaType) override {
        if (!ppMediaType) return E_POINTER;
        if (!m_pMediaType) {
            *ppMediaType = nullptr;
            return S_FALSE;
        }
        *ppMediaType = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
        CopyMediaTypeHelper(*ppMediaType, m_pMediaType);
        return S_OK;
    }

    STDMETHODIMP SetMediaType(AM_MEDIA_TYPE* pMediaType) override {
        if (m_pMediaType) {
            FreeMediaTypeHelper(*m_pMediaType);
            CoTaskMemFree(m_pMediaType);
            m_pMediaType = nullptr;
        }
        if (pMediaType) {
            m_pMediaType = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
            CopyMediaTypeHelper(m_pMediaType, pMediaType);
        }
        return S_OK;
    }

    STDMETHODIMP IsDiscontinuity() override {
        return m_isDiscontinuity ? S_OK : S_FALSE;
    }

    STDMETHODIMP SetDiscontinuity(BOOL bDiscontinuity) override {
        m_isDiscontinuity = (bDiscontinuity != FALSE);
        return S_OK;
    }

    STDMETHODIMP GetMediaTime(LONGLONG* pTimeStart, LONGLONG* pTimeEnd) override {
        if (!pTimeStart || !pTimeEnd) return E_POINTER;
        if (!m_hasMediaTime) return VFW_E_MEDIA_TIME_NOT_SET;
        *pTimeStart = m_mediaTimeStart;
        *pTimeEnd = m_mediaTimeEnd;
        return S_OK;
    }

    STDMETHODIMP SetMediaTime(LONGLONG* pTimeStart, LONGLONG* pTimeEnd) override {
        if (pTimeStart && pTimeEnd) {
            m_mediaTimeStart = *pTimeStart;
            m_mediaTimeEnd = *pTimeEnd;
            m_hasMediaTime = true;
        } else {
            m_hasMediaTime = false;
        }
        return S_OK;
    }

    void ResetSample() {
        m_actualLength = m_bufferSize;
        m_isSyncPoint = true;
        m_isPreroll = false;
        m_isDiscontinuity = false;
        m_hasTime = false;
        m_hasMediaTime = false;
    }

private:
    std::atomic<ULONG> m_cRef{0};
    BouleCamMemAllocator* m_pAllocator;
    BYTE* m_pBuffer;
    long m_bufferSize;
    long m_actualLength;
    REFERENCE_TIME m_timeStart = 0;
    REFERENCE_TIME m_timeEnd = 0;
    LONGLONG m_mediaTimeStart = 0;
    LONGLONG m_mediaTimeEnd = 0;
    bool m_isSyncPoint = true;
    bool m_isPreroll = false;
    bool m_isDiscontinuity = false;
    bool m_hasTime = false;
    bool m_hasMediaTime = false;
    AM_MEDIA_TYPE* m_pMediaType = nullptr;
};

class BouleCamMemAllocator : public IMemAllocator {
public:
    BouleCamMemAllocator() : m_cRef(1) {}
    virtual ~BouleCamMemAllocator() {
        Decommit();
        FreeBuffers();
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IMemAllocator) {
            *ppv = static_cast<IMemAllocator*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return ++m_cRef;
    }

    STDMETHODIMP_(ULONG) Release() override {
        ULONG ref = --m_cRef;
        if (ref == 0) delete this;
        return ref;
    }

    // IMemAllocator
    STDMETHODIMP SetProperties(ALLOCATOR_PROPERTIES* pRequest, ALLOCATOR_PROPERTIES* pActual) override {
        if (!pRequest || !pActual) return E_POINTER;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_committed) return VFW_E_ALREADY_COMMITTED;

        m_props = *pRequest;
        if (m_props.cBuffers < 1) m_props.cBuffers = 1;
        if (m_props.cbBuffer < 1) m_props.cbBuffer = 1;
        if (m_props.cbAlign < 1) m_props.cbAlign = 1;

        *pActual = m_props;
        return S_OK;
    }

    STDMETHODIMP GetProperties(ALLOCATOR_PROPERTIES* pProps) override {
        if (!pProps) return E_POINTER;
        std::lock_guard<std::mutex> lock(m_mutex);
        *pProps = m_props;
        return S_OK;
    }

    STDMETHODIMP Commit() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_committed) return S_OK;

        FreeBuffers();

        for (long i = 0; i < m_props.cBuffers; ++i) {
            BYTE* buf = new BYTE[m_props.cbBuffer + m_props.cbPrefix];
            m_buffers.push_back(buf);
            auto* sample = new BouleCamMediaSample(this, buf + m_props.cbPrefix, m_props.cbBuffer);
            m_freeSamples.push_back(sample);
            m_allSamples.push_back(sample);
        }

        m_committed = true;
        m_decommitRequested = false;
        return S_OK;
    }

    STDMETHODIMP Decommit() override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_committed = false;
            m_decommitRequested = true;
            m_cv.notify_all();
        }
        return S_OK;
    }

    STDMETHODIMP GetBuffer(IMediaSample** ppBuffer, REFERENCE_TIME*, REFERENCE_TIME*, DWORD) override {
        if (!ppBuffer) return E_POINTER;
        *ppBuffer = nullptr;

        std::unique_lock<std::mutex> lock(m_mutex);
        while (m_committed && m_freeSamples.empty() && !m_decommitRequested) {
            if (m_cv.wait_for(lock, std::chrono::milliseconds(50)) == std::cv_status::timeout) {
                if (!m_committed || m_decommitRequested) return VFW_E_NOT_COMMITTED;
            }
        }

        if (!m_committed || m_decommitRequested) {
            return VFW_E_NOT_COMMITTED;
        }

        if (m_freeSamples.empty()) {
            return VFW_E_TIMEOUT;
        }

        BouleCamMediaSample* pSample = m_freeSamples.back();
        m_freeSamples.pop_back();
        pSample->ResetSample();
        pSample->AddRef();
        *ppBuffer = pSample;
        return S_OK;
    }

    STDMETHODIMP ReleaseBuffer(IMediaSample* pBuffer) override {
        if (!pBuffer) return E_POINTER;
        std::lock_guard<std::mutex> lock(m_mutex);
        auto* sample = static_cast<BouleCamMediaSample*>(pBuffer);
        m_freeSamples.push_back(sample);
        m_cv.notify_one();
        return S_OK;
    }

private:
    void FreeBuffers() {
        for (auto* sample : m_allSamples) {
            delete sample;
        }
        m_allSamples.clear();
        m_freeSamples.clear();

        for (auto* buf : m_buffers) {
            delete[] buf;
        }
        m_buffers.clear();
    }

    std::atomic<ULONG> m_cRef{1};
    std::mutex m_mutex;
    std::condition_variable m_cv;
    ALLOCATOR_PROPERTIES m_props{};
    bool m_committed = false;
    bool m_decommitRequested = false;
    std::vector<BouleCamMediaSample*> m_freeSamples;
    std::vector<BouleCamMediaSample*> m_allSamples;
    std::vector<BYTE*> m_buffers;
};

inline STDMETHODIMP_(ULONG) BouleCamMediaSample::Release() {
    ULONG ref = --m_cRef;
    if (ref == 0) {
        if (m_pAllocator) {
            m_pAllocator->ReleaseBuffer(this);
        }
    }
    return ref;
}

// =========================================================================
// ENUMERATOR HELPERS
// =========================================================================

class BouleCamEnumPins : public IEnumPins {
public:
    BouleCamEnumPins(IPin* pPin) : m_cRef(1), m_pPin(pPin), m_pos(0) {
        if (m_pPin) m_pPin->AddRef();
    }
    virtual ~BouleCamEnumPins() {
        if (m_pPin) m_pPin->Release();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumPins) {
            *ppv = static_cast<IEnumPins*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++m_cRef; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG ref = --m_cRef;
        if (ref == 0) delete this;
        return ref;
    }

    STDMETHODIMP Next(ULONG cPins, IPin** ppPins, ULONG* pcFetched) override {
        if (!ppPins) return E_POINTER;
        if (pcFetched) *pcFetched = 0;
        if (cPins == 0) return S_OK;

        if (m_pos == 0 && m_pPin) {
            ppPins[0] = m_pPin;
            m_pPin->AddRef();
            m_pos = 1;
            if (pcFetched) *pcFetched = 1;
            return (cPins == 1) ? S_OK : S_FALSE;
        }
        return S_FALSE;
    }

    STDMETHODIMP Skip(ULONG cPins) override {
        m_pos += cPins;
        return (m_pos <= 1) ? S_OK : S_FALSE;
    }

    STDMETHODIMP Reset() override {
        m_pos = 0;
        return S_OK;
    }

    STDMETHODIMP Clone(IEnumPins** ppEnum) override {
        if (!ppEnum) return E_POINTER;
        *ppEnum = new BouleCamEnumPins(m_pPin);
        return S_OK;
    }

private:
    std::atomic<ULONG> m_cRef;
    IPin* m_pPin;
    ULONG m_pos;
};

class BouleCamEnumMediaTypes : public IEnumMediaTypes {
public:
    BouleCamEnumMediaTypes(const std::vector<AM_MEDIA_TYPE>& types)
        : m_cRef(1), m_types(types), m_pos(0) {}
    virtual ~BouleCamEnumMediaTypes() {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumMediaTypes) {
            *ppv = static_cast<IEnumMediaTypes*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++m_cRef; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG ref = --m_cRef;
        if (ref == 0) delete this;
        return ref;
    }

    STDMETHODIMP Next(ULONG cMediaTypes, AM_MEDIA_TYPE** ppMediaTypes, ULONG* pcFetched) override {
        if (!ppMediaTypes) return E_POINTER;
        if (pcFetched) *pcFetched = 0;
        ULONG count = 0;

        while (m_pos < m_types.size() && count < cMediaTypes) {
            AM_MEDIA_TYPE* pmt = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
            *pmt = m_types[m_pos];
            if (m_types[m_pos].cbFormat > 0 && m_types[m_pos].pbFormat) {
                pmt->pbFormat = (BYTE*)CoTaskMemAlloc(m_types[m_pos].cbFormat);
                memcpy(pmt->pbFormat, m_types[m_pos].pbFormat, m_types[m_pos].cbFormat);
            } else {
                pmt->pbFormat = nullptr;
            }
            ppMediaTypes[count++] = pmt;
            m_pos++;
        }
        if (pcFetched) *pcFetched = count;
        return (count == cMediaTypes) ? S_OK : S_FALSE;
    }

    STDMETHODIMP Skip(ULONG cMediaTypes) override {
        m_pos += cMediaTypes;
        return (m_pos <= m_types.size()) ? S_OK : S_FALSE;
    }

    STDMETHODIMP Reset() override {
        m_pos = 0;
        return S_OK;
    }

    STDMETHODIMP Clone(IEnumMediaTypes** ppEnum) override {
        if (!ppEnum) return E_POINTER;
        *ppEnum = new BouleCamEnumMediaTypes(m_types);
        return S_OK;
    }

private:
    std::atomic<ULONG> m_cRef;
    std::vector<AM_MEDIA_TYPE> m_types;
    size_t m_pos;
};

// =========================================================================
// VIDEO PIN IMPLEMENTATION
// =========================================================================

BouleCamVideoPin::BouleCamVideoPin(BouleCamVideoFilter* pFilter, int camIndex)
    : m_pFilter(pFilter)
    , m_camIndex(camIndex) {
    m_shmConsumer.Open(camIndex);
}

BouleCamVideoPin::~BouleCamVideoPin() {
    StopStreaming();
    Disconnect();
}

STDMETHODIMP BouleCamVideoPin::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IPin) {
        *ppv = static_cast<IPin*>(this);
        AddRef();
        return S_OK;
    } else if (riid == IID_IAMStreamConfig) {
        *ppv = static_cast<IAMStreamConfig*>(this);
        AddRef();
        return S_OK;
    } else if (riid == IID_IKsPropertySet) {
        *ppv = static_cast<IKsPropertySet*>(this);
        AddRef();
        return S_OK;
    } else if (riid == IID_IQualityControl) {
        *ppv = static_cast<IQualityControl*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) BouleCamVideoPin::AddRef() { return ++m_cRef; }
STDMETHODIMP_(ULONG) BouleCamVideoPin::Release() {
    ULONG ref = --m_cRef;
    if (ref == 0) delete this;
    return ref;
}

bool BouleCamVideoPin::CreateMediaTypes(AM_MEDIA_TYPE* pmt, int index) {
    if (!pmt) return false;
    memset(pmt, 0, sizeof(AM_MEDIA_TYPE));

    pmt->majortype = MEDIATYPE_Video;
    pmt->formattype = FORMAT_VideoInfo;
    pmt->bFixedSizeSamples = TRUE;
    pmt->bTemporalCompression = FALSE;
    pmt->cbFormat = sizeof(VIDEOINFOHEADER);
    pmt->pbFormat = (BYTE*)CoTaskMemAlloc(sizeof(VIDEOINFOHEADER));
    if (!pmt->pbFormat) return false;

    VIDEOINFOHEADER* pvih = (VIDEOINFOHEADER*)pmt->pbFormat;
    memset(pvih, 0, sizeof(VIDEOINFOHEADER));
    pvih->rcSource = RECT{0, 0, 0, 0};
    pvih->rcTarget = RECT{0, 0, 0, 0};
    pvih->dwBitRate = 1920 * 1080 * 12 * 60 / 8;
    pvih->AvgTimePerFrame = 166666; // 60 FPS

    BITMAPINFOHEADER& bmi = pvih->bmiHeader;
    bmi.biSize = sizeof(BITMAPINFOHEADER);
    bmi.biWidth = 1920;
    bmi.biHeight = 1080;
    bmi.biPlanes = 1;

    if (index == 0) {
        // NV12 format (Native hardware format)
        pmt->subtype = MEDIASUBTYPE_NV12;
        bmi.biBitCount = 12;
        bmi.biCompression = MAKEFOURCC('N', 'V', '1', '2');
        bmi.biSizeImage = 1920 * 1080 * 3 / 2;
        pmt->lSampleSize = bmi.biSizeImage;
    } else {
        // RGB24 fallback format
        pmt->subtype = MEDIASUBTYPE_RGB24;
        bmi.biBitCount = 24;
        bmi.biCompression = BI_RGB;
        bmi.biSizeImage = 1920 * 1080 * 3;
        pmt->lSampleSize = bmi.biSizeImage;
    }
    return true;
}

STDMETHODIMP BouleCamVideoPin::Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) {
    if (!pReceivePin) return E_POINTER;
    std::lock_guard<std::mutex> lock(m_pinMutex);
    if (m_pConnectedPin) return VFW_E_ALREADY_CONNECTED;

    AM_MEDIA_TYPE mt{};
    if (pmt) {
        mt = *pmt;
    } else {
        CreateMediaTypes(&mt, 0);
    }

    HRESULT hr = pReceivePin->ReceiveConnection(this, &mt);
    if (SUCCEEDED(hr)) {
        m_pConnectedPin = pReceivePin;
        m_pConnectedPin->AddRef();
        m_currentMediaType = mt;
        m_currentSubtype = mt.subtype;

        // Query IMemInputPin for sample delivery
        pReceivePin->QueryInterface(IID_IMemInputPin, (void**)&m_pMemInputPin);
        if (m_pMemInputPin) {
            HRESULT hrAlloc = m_pMemInputPin->GetAllocator(&m_pAllocator);
            if (FAILED(hrAlloc) || !m_pAllocator) {
                // Downstream pin (OBS libdshowcapture) returns VFW_E_NO_ALLOCATOR.
                // We provide our own allocator.
                m_pAllocator = new BouleCamMemAllocator();
            }

            if (m_pAllocator) {
                ALLOCATOR_PROPERTIES props{}, actual{};
                props.cBuffers = 3;
                props.cbBuffer = (m_currentSubtype == MEDIASUBTYPE_RGB24) ? (1920 * 1080 * 3) : (1920 * 1088 * 3 / 2);
                props.cbAlign = 1;
                props.cbPrefix = 0;
                m_pAllocator->SetProperties(&props, &actual);
                m_pAllocator->Commit();
                m_pMemInputPin->NotifyAllocator(m_pAllocator, FALSE);
            }
        }
    }
    return hr;
}

STDMETHODIMP BouleCamVideoPin::ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt) {
    return E_UNEXPECTED; // Output pin does not accept incoming connections
}

STDMETHODIMP BouleCamVideoPin::Disconnect() {
    std::lock_guard<std::mutex> lock(m_pinMutex);
    if (m_pAllocator) {
        m_pAllocator->Decommit();
        m_pAllocator->Release();
        m_pAllocator = nullptr;
    }
    if (m_pMemInputPin) {
        m_pMemInputPin->Release();
        m_pMemInputPin = nullptr;
    }
    if (m_pConnectedPin) {
        m_pConnectedPin->Release();
        m_pConnectedPin = nullptr;
    }
    return S_OK;
}

STDMETHODIMP BouleCamVideoPin::ConnectedTo(IPin** pPin) {
    if (!pPin) return E_POINTER;
    std::lock_guard<std::mutex> lock(m_pinMutex);
    if (!m_pConnectedPin) return VFW_E_NOT_CONNECTED;
    *pPin = m_pConnectedPin;
    (*pPin)->AddRef();
    return S_OK;
}

STDMETHODIMP BouleCamVideoPin::ConnectionMediaType(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    std::lock_guard<std::mutex> lock(m_pinMutex);
    if (!m_pConnectedPin) return VFW_E_NOT_CONNECTED;
    *pmt = m_currentMediaType;
    if (m_currentMediaType.cbFormat > 0 && m_currentMediaType.pbFormat) {
        pmt->pbFormat = (BYTE*)CoTaskMemAlloc(m_currentMediaType.cbFormat);
        memcpy(pmt->pbFormat, m_currentMediaType.pbFormat, m_currentMediaType.cbFormat);
    }
    return S_OK;
}

STDMETHODIMP BouleCamVideoPin::QueryPinInfo(PIN_INFO* pInfo) {
    if (!pInfo) return E_POINTER;
    pInfo->pFilter = m_pFilter;
    if (pInfo->pFilter) pInfo->pFilter->AddRef();
    pInfo->dir = PINDIR_OUTPUT;
    wcscpy_s(pInfo->achName, L"Capture");
    return S_OK;
}

STDMETHODIMP BouleCamVideoPin::QueryDirection(PIN_DIRECTION* pPinDir) {
    if (!pPinDir) return E_POINTER;
    *pPinDir = PINDIR_OUTPUT;
    return S_OK;
}

STDMETHODIMP BouleCamVideoPin::QueryId(LPWSTR* Id) {
    if (!Id) return E_POINTER;
    *Id = (LPWSTR)CoTaskMemAlloc(sizeof(L"Capture"));
    wcscpy_s(*Id, 8, L"Capture");
    return S_OK;
}

STDMETHODIMP BouleCamVideoPin::QueryAccept(const AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    if (pmt->majortype == MEDIATYPE_Video &&
        (pmt->subtype == MEDIASUBTYPE_NV12 || pmt->subtype == MEDIASUBTYPE_RGB24)) {
        return S_OK;
    }
    return S_FALSE;
}

STDMETHODIMP BouleCamVideoPin::EnumMediaTypes(IEnumMediaTypes** ppEnum) {
    if (!ppEnum) return E_POINTER;
    std::vector<AM_MEDIA_TYPE> types(2);
    CreateMediaTypes(&types[0], 0); // NV12
    CreateMediaTypes(&types[1], 1); // RGB24
    *ppEnum = new BouleCamEnumMediaTypes(types);
    return S_OK;
}

STDMETHODIMP BouleCamVideoPin::QueryInternalConnections(IPin** apPin, ULONG* nPin) {
    return E_NOTIMPL;
}

STDMETHODIMP BouleCamVideoPin::EndOfStream() { return S_OK; }
STDMETHODIMP BouleCamVideoPin::BeginFlush() { return S_OK; }
STDMETHODIMP BouleCamVideoPin::EndFlush() { return S_OK; }
STDMETHODIMP BouleCamVideoPin::NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) { return S_OK; }

// IAMStreamConfig
STDMETHODIMP BouleCamVideoPin::SetFormat(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    if (QueryAccept(pmt) != S_OK) return E_FAIL;
    m_currentMediaType = *pmt;
    m_currentSubtype = pmt->subtype;
    return S_OK;
}

STDMETHODIMP BouleCamVideoPin::GetFormat(AM_MEDIA_TYPE** ppmt) {
    if (!ppmt) return E_POINTER;
    *ppmt = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
    CreateMediaTypes(*ppmt, (m_currentSubtype == MEDIASUBTYPE_RGB24) ? 1 : 0);
    return S_OK;
}

STDMETHODIMP BouleCamVideoPin::GetNumberOfCapabilities(int* piCount, int* piSize) {
    if (!piCount || !piSize) return E_POINTER;
    *piCount = 2; // NV12 and RGB24
    *piSize = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    return S_OK;
}

STDMETHODIMP BouleCamVideoPin::GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) {
    if (!ppmt || !pSCC) return E_POINTER;
    if (iIndex < 0 || iIndex >= 2) return E_INVALIDARG;

    *ppmt = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
    CreateMediaTypes(*ppmt, iIndex);

    VIDEO_STREAM_CONFIG_CAPS* caps = (VIDEO_STREAM_CONFIG_CAPS*)pSCC;
    memset(caps, 0, sizeof(VIDEO_STREAM_CONFIG_CAPS));
    caps->guid = FORMAT_VideoInfo;
    caps->VideoStandard = 0;
    caps->InputSize = SIZE{1920, 1080};
    caps->MinCroppingSize = SIZE{1920, 1080};
    caps->MaxCroppingSize = SIZE{1920, 1080};
    caps->CropGranularityX = 1;
    caps->CropGranularityY = 1;
    caps->CropAlignX = 1;
    caps->CropAlignY = 1;
    caps->MinOutputSize = SIZE{640, 360};
    caps->MaxOutputSize = SIZE{1920, 1080};
    caps->OutputGranularityX = 2;
    caps->OutputGranularityY = 2;
    caps->StretchTapsX = 0;
    caps->StretchTapsY = 0;
    caps->ShrinkTapsX = 0;
    caps->ShrinkTapsY = 0;
    caps->MinFrameInterval = 166666; // 60 FPS
    caps->MaxFrameInterval = 333333; // 30 FPS
    caps->MinBitsPerSecond = 10000000;
    caps->MaxBitsPerSecond = 100000000;
    return S_OK;
}

// IKsPropertySet (OBS queries AMPROPERTY_PIN_CATEGORY to verify it's a camera)
STDMETHODIMP BouleCamVideoPin::Set(REFGUID, DWORD, LPVOID, DWORD, LPVOID, DWORD) { return E_NOTIMPL; }

STDMETHODIMP BouleCamVideoPin::Get(REFGUID guidPropSet, DWORD dwPropID, LPVOID, DWORD, LPVOID pPropData, DWORD cbPropData, DWORD* pcbReturned) {
    if (guidPropSet == AMPROPSETID_Pin && dwPropID == 0) { // AMPROPERTY_PIN_CATEGORY
        if (cbPropData < sizeof(GUID)) return E_UNEXPECTED;
        if (!pPropData) return E_POINTER;
        *(GUID*)pPropData = PIN_CATEGORY_CAPTURE;
        if (pcbReturned) *pcbReturned = sizeof(GUID);
        return S_OK;
    }
    return E_PROP_ID_UNSUPPORTED;
}

STDMETHODIMP BouleCamVideoPin::QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD* pTypeSupport) {
    if (guidPropSet == AMPROPSETID_Pin && dwPropID == 0) {
        if (pTypeSupport) *pTypeSupport = KSPROPERTY_SUPPORT_GET;
        return S_OK;
    }
    return E_PROP_ID_UNSUPPORTED;
}

STDMETHODIMP BouleCamVideoPin::Notify(IBaseFilter*, Quality) { return S_OK; }
STDMETHODIMP BouleCamVideoPin::SetSink(IQualityControl*) { return S_OK; }

void BouleCamVideoPin::StartStreaming() {
    if (m_isStreaming.exchange(true)) return;
    m_streamThread = std::thread(&BouleCamVideoPin::StreamingWorker, this);
}

void BouleCamVideoPin::StopStreaming() {
    if (!m_isStreaming.exchange(false)) return;
    if (m_streamThread.joinable()) {
        m_streamThread.join();
    }
}

void BouleCamVideoPin::StreamingWorker() {
    REFERENCE_TIME sampleTime = 0;
    REFERENCE_TIME frameDuration = 166666; // 60 FPS

    while (m_isStreaming.load()) {
        auto tStart = std::chrono::steady_clock::now();

        IMemAllocator* pAlloc = m_pAllocator;
        IMemInputPin* pInput = m_pMemInputPin;

        if (pAlloc && pInput) {
            IMediaSample* pSample = nullptr;
            HRESULT hr = pAlloc->GetBuffer(&pSample, nullptr, nullptr, 0);
            if (SUCCEEDED(hr) && pSample) {
                BYTE* pDst = nullptr;
                pSample->GetPointer(&pDst);
                long dstSize = pSample->GetSize();

                uint32_t width = 1920, height = 1080, stride = 1920;
                BouleCamPixelFormat fmt = BOULECAM_PIXFMT_NV12;

                bool gotFrame = m_shmConsumer.ReadLatestFrame(pDst, dstSize, width, height, stride, fmt, 16);
                if (!gotFrame) {
                    // Clean idle pattern: dark charcoal studio background
                    size_t ySize = 1920 * 1080;
                    if ((size_t)dstSize >= ySize * 3 / 2) {
                        memset(pDst, 28, ySize); // Dark charcoal Y
                        memset(pDst + ySize, 128, ySize / 2); // Neutral UV
                    }
                }

                pSample->SetActualDataLength((m_currentSubtype == MEDIASUBTYPE_RGB24) ? (1920 * 1080 * 3) : (1920 * 1080 * 3 / 2));
                REFERENCE_TIME t1 = sampleTime;
                REFERENCE_TIME t2 = sampleTime + frameDuration;
                pSample->SetTime(&t1, &t2);
                pSample->SetSyncPoint(TRUE);
                pSample->SetPreroll(FALSE);
                pSample->SetDiscontinuity(FALSE);

                pInput->Receive(pSample);
                pSample->Release();
                sampleTime += frameDuration;
            }
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - tStart
        ).count();
        if (elapsed < 16) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16 - elapsed));
        }
    }
}

// =========================================================================
// VIDEO FILTER IMPLEMENTATION
// =========================================================================

BouleCamVideoFilter::BouleCamVideoFilter(int camIndex)
    : m_camIndex(camIndex) {
    m_pPin = new BouleCamVideoPin(this, camIndex);
}

BouleCamVideoFilter::~BouleCamVideoFilter() {
    Stop();
    if (m_pPin) {
        m_pPin->Release();
        m_pPin = nullptr;
    }
}

STDMETHODIMP BouleCamVideoFilter::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IPersist || riid == IID_IMediaFilter || riid == IID_IBaseFilter) {
        *ppv = static_cast<IBaseFilter*>(this);
        AddRef();
        return S_OK;
    } else if (riid == IID_IAMFilterMiscFlags) {
        *ppv = static_cast<IAMFilterMiscFlags*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) BouleCamVideoFilter::AddRef() { return ++m_cRef; }
STDMETHODIMP_(ULONG) BouleCamVideoFilter::Release() {
    ULONG ref = --m_cRef;
    if (ref == 0) delete this;
    return ref;
}

STDMETHODIMP BouleCamVideoFilter::GetClassID(CLSID* pClassID) {
    if (!pClassID) return E_POINTER;
    if (m_camIndex == 2) *pClassID = CLSID_BouleCamVirtualCam2;
    else if (m_camIndex == 3) *pClassID = CLSID_BouleCamVirtualCam3;
    else *pClassID = CLSID_BouleCamVirtualCam;
    return S_OK;
}

STDMETHODIMP BouleCamVideoFilter::Stop() {
    std::lock_guard<std::mutex> lock(m_filterMutex);
    m_state = State_Stopped;
    if (m_pPin) m_pPin->StopStreaming();
    return S_OK;
}

STDMETHODIMP BouleCamVideoFilter::Pause() {
    std::lock_guard<std::mutex> lock(m_filterMutex);
    m_state = State_Paused;
    if (m_pPin) m_pPin->StartStreaming();
    return S_OK;
}

STDMETHODIMP BouleCamVideoFilter::Run(REFERENCE_TIME) {
    std::lock_guard<std::mutex> lock(m_filterMutex);
    m_state = State_Running;
    if (m_pPin) m_pPin->StartStreaming();
    return S_OK;
}

STDMETHODIMP BouleCamVideoFilter::GetState(DWORD, FILTER_STATE* State) {
    if (!State) return E_POINTER;
    std::lock_guard<std::mutex> lock(m_filterMutex);
    *State = m_state;
    return S_OK;
}

STDMETHODIMP BouleCamVideoFilter::SetSyncSource(IReferenceClock* pClock) {
    std::lock_guard<std::mutex> lock(m_filterMutex);
    if (m_pClock) m_pClock->Release();
    m_pClock = pClock;
    if (m_pClock) m_pClock->AddRef();
    return S_OK;
}

STDMETHODIMP BouleCamVideoFilter::GetSyncSource(IReferenceClock** pClock) {
    if (!pClock) return E_POINTER;
    std::lock_guard<std::mutex> lock(m_filterMutex);
    *pClock = m_pClock;
    if (*pClock) (*pClock)->AddRef();
    return S_OK;
}

STDMETHODIMP BouleCamVideoFilter::EnumPins(IEnumPins** ppEnum) {
    if (!ppEnum) return E_POINTER;
    *ppEnum = new BouleCamEnumPins(m_pPin);
    return S_OK;
}

STDMETHODIMP BouleCamVideoFilter::FindPin(LPCWSTR Id, IPin** ppPin) {
    if (!ppPin) return E_POINTER;
    if (Id && wcscmp(Id, L"Capture") == 0) {
        *ppPin = m_pPin;
        if (*ppPin) (*ppPin)->AddRef();
        return S_OK;
    }
    *ppPin = nullptr;
    return VFW_E_NOT_FOUND;
}

STDMETHODIMP BouleCamVideoFilter::QueryFilterInfo(FILTER_INFO* pInfo) {
    if (!pInfo) return E_POINTER;
    if (m_camIndex == 2)      wcscpy_s(pInfo->achName, L"BouleCam Virtual Camera 2");
    else if (m_camIndex == 3) wcscpy_s(pInfo->achName, L"BouleCam Virtual Camera 3");
    else                      wcscpy_s(pInfo->achName, L"BouleCam Virtual Camera");
    pInfo->pGraph = m_pGraph;
    if (pInfo->pGraph) pInfo->pGraph->AddRef();
    return S_OK;
}

STDMETHODIMP BouleCamVideoFilter::JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR) {
    std::lock_guard<std::mutex> lock(m_filterMutex);
    m_pGraph = pGraph;
    return S_OK;
}

STDMETHODIMP BouleCamVideoFilter::QueryVendorInfo(LPWSTR* pVendorInfo) {
    if (!pVendorInfo) return E_POINTER;
    *pVendorInfo = (LPWSTR)CoTaskMemAlloc(sizeof(L"BouleCam"));
    wcscpy_s(*pVendorInfo, 9, L"BouleCam");
    return S_OK;
}

STDMETHODIMP_(ULONG) BouleCamVideoFilter::GetMiscFlags() {
    return AM_FILTER_MISC_FLAGS_IS_SOURCE;
}

// =========================================================================
// AUDIO PIN IMPLEMENTATION
// =========================================================================

BouleCamAudioPin::BouleCamAudioPin(BouleCamAudioFilter* pFilter, int camIndex)
    : m_pFilter(pFilter)
    , m_camIndex(camIndex) {
    m_shmConsumer.Open(camIndex);
}

BouleCamAudioPin::~BouleCamAudioPin() {
    StopStreaming();
    Disconnect();
}

STDMETHODIMP BouleCamAudioPin::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IPin) {
        *ppv = static_cast<IPin*>(this);
        AddRef();
        return S_OK;
    } else if (riid == IID_IAMStreamConfig) {
        *ppv = static_cast<IAMStreamConfig*>(this);
        AddRef();
        return S_OK;
    } else if (riid == IID_IKsPropertySet) {
        *ppv = static_cast<IKsPropertySet*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) BouleCamAudioPin::AddRef() { return ++m_cRef; }
STDMETHODIMP_(ULONG) BouleCamAudioPin::Release() {
    ULONG ref = --m_cRef;
    if (ref == 0) delete this;
    return ref;
}

static void CreateAudioMediaType(AM_MEDIA_TYPE* pmt) {
    memset(pmt, 0, sizeof(AM_MEDIA_TYPE));
    pmt->majortype = MEDIATYPE_Audio;
    pmt->subtype = MEDIASUBTYPE_PCM;
    pmt->formattype = FORMAT_WaveFormatEx;
    pmt->bFixedSizeSamples = TRUE;
    pmt->bTemporalCompression = FALSE;
    pmt->cbFormat = sizeof(WAVEFORMATEX);
    pmt->pbFormat = (BYTE*)CoTaskMemAlloc(sizeof(WAVEFORMATEX));

    WAVEFORMATEX* pwfx = (WAVEFORMATEX*)pmt->pbFormat;
    memset(pwfx, 0, sizeof(WAVEFORMATEX));
    pwfx->wFormatTag = WAVE_FORMAT_PCM;
    pwfx->nChannels = 1;
    pwfx->nSamplesPerSec = 48000;
    pwfx->wBitsPerSample = 16;
    pwfx->nBlockAlign = 2;
    pwfx->nAvgBytesPerSec = 96000;
    pmt->lSampleSize = 2;
}

STDMETHODIMP BouleCamAudioPin::Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) {
    if (!pReceivePin) return E_POINTER;
    std::lock_guard<std::mutex> lock(m_pinMutex);
    if (m_pConnectedPin) return VFW_E_ALREADY_CONNECTED;

    AM_MEDIA_TYPE mt{};
    if (pmt) {
        mt = *pmt;
    } else {
        CreateAudioMediaType(&mt);
    }

    HRESULT hr = pReceivePin->ReceiveConnection(this, &mt);
    if (SUCCEEDED(hr)) {
        m_pConnectedPin = pReceivePin;
        m_pConnectedPin->AddRef();
        m_currentMediaType = mt;

        pReceivePin->QueryInterface(IID_IMemInputPin, (void**)&m_pMemInputPin);
        if (m_pMemInputPin) {
            HRESULT hrAlloc = m_pMemInputPin->GetAllocator(&m_pAllocator);
            if (FAILED(hrAlloc) || !m_pAllocator) {
                m_pAllocator = new BouleCamMemAllocator();
            }

            if (m_pAllocator) {
                ALLOCATOR_PROPERTIES props{}, actual{};
                props.cBuffers = 4;
                props.cbBuffer = 1920; // 20ms @ 48kHz 16-bit mono = 960 samples * 2 = 1920 bytes
                props.cbAlign = 2;
                props.cbPrefix = 0;
                m_pAllocator->SetProperties(&props, &actual);
                m_pAllocator->Commit();
                m_pMemInputPin->NotifyAllocator(m_pAllocator, FALSE);
            }
        }
    }
    return hr;
}

STDMETHODIMP BouleCamAudioPin::ReceiveConnection(IPin*, const AM_MEDIA_TYPE*) { return E_UNEXPECTED; }

STDMETHODIMP BouleCamAudioPin::Disconnect() {
    std::lock_guard<std::mutex> lock(m_pinMutex);
    if (m_pAllocator) {
        m_pAllocator->Decommit();
        m_pAllocator->Release();
        m_pAllocator = nullptr;
    }
    if (m_pMemInputPin) {
        m_pMemInputPin->Release();
        m_pMemInputPin = nullptr;
    }
    if (m_pConnectedPin) {
        m_pConnectedPin->Release();
        m_pConnectedPin = nullptr;
    }
    return S_OK;
}

STDMETHODIMP BouleCamAudioPin::ConnectedTo(IPin** pPin) {
    if (!pPin) return E_POINTER;
    std::lock_guard<std::mutex> lock(m_pinMutex);
    if (!m_pConnectedPin) return VFW_E_NOT_CONNECTED;
    *pPin = m_pConnectedPin;
    (*pPin)->AddRef();
    return S_OK;
}

STDMETHODIMP BouleCamAudioPin::ConnectionMediaType(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    std::lock_guard<std::mutex> lock(m_pinMutex);
    if (!m_pConnectedPin) return VFW_E_NOT_CONNECTED;
    *pmt = m_currentMediaType;
    if (m_currentMediaType.cbFormat > 0 && m_currentMediaType.pbFormat) {
        pmt->pbFormat = (BYTE*)CoTaskMemAlloc(m_currentMediaType.cbFormat);
        memcpy(pmt->pbFormat, m_currentMediaType.pbFormat, m_currentMediaType.cbFormat);
    }
    return S_OK;
}

STDMETHODIMP BouleCamAudioPin::QueryPinInfo(PIN_INFO* pInfo) {
    if (!pInfo) return E_POINTER;
    pInfo->pFilter = m_pFilter;
    if (pInfo->pFilter) pInfo->pFilter->AddRef();
    pInfo->dir = PINDIR_OUTPUT;
    wcscpy_s(pInfo->achName, L"Audio Capture");
    return S_OK;
}

STDMETHODIMP BouleCamAudioPin::QueryDirection(PIN_DIRECTION* pPinDir) {
    if (!pPinDir) return E_POINTER;
    *pPinDir = PINDIR_OUTPUT;
    return S_OK;
}

STDMETHODIMP BouleCamAudioPin::QueryId(LPWSTR* Id) {
    if (!Id) return E_POINTER;
    *Id = (LPWSTR)CoTaskMemAlloc(sizeof(L"Audio Capture"));
    wcscpy_s(*Id, 14, L"Audio Capture");
    return S_OK;
}

STDMETHODIMP BouleCamAudioPin::QueryAccept(const AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    if (pmt->majortype == MEDIATYPE_Audio && pmt->subtype == MEDIASUBTYPE_PCM) return S_OK;
    return S_FALSE;
}

STDMETHODIMP BouleCamAudioPin::EnumMediaTypes(IEnumMediaTypes** ppEnum) {
    if (!ppEnum) return E_POINTER;
    std::vector<AM_MEDIA_TYPE> types(1);
    CreateAudioMediaType(&types[0]);
    *ppEnum = new BouleCamEnumMediaTypes(types);
    return S_OK;
}

STDMETHODIMP BouleCamAudioPin::QueryInternalConnections(IPin**, ULONG*) { return E_NOTIMPL; }
STDMETHODIMP BouleCamAudioPin::EndOfStream() { return S_OK; }
STDMETHODIMP BouleCamAudioPin::BeginFlush() { return S_OK; }
STDMETHODIMP BouleCamAudioPin::EndFlush() { return S_OK; }
STDMETHODIMP BouleCamAudioPin::NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) { return S_OK; }

STDMETHODIMP BouleCamAudioPin::SetFormat(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    if (QueryAccept(pmt) != S_OK) return E_FAIL;
    m_currentMediaType = *pmt;
    return S_OK;
}

STDMETHODIMP BouleCamAudioPin::GetFormat(AM_MEDIA_TYPE** ppmt) {
    if (!ppmt) return E_POINTER;
    *ppmt = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
    CreateAudioMediaType(*ppmt);
    return S_OK;
}

STDMETHODIMP BouleCamAudioPin::GetNumberOfCapabilities(int* piCount, int* piSize) {
    if (!piCount || !piSize) return E_POINTER;
    *piCount = 1;
    *piSize = sizeof(AUDIO_STREAM_CONFIG_CAPS);
    return S_OK;
}

STDMETHODIMP BouleCamAudioPin::GetStreamCaps(int, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) {
    if (!ppmt || !pSCC) return E_POINTER;
    *ppmt = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
    CreateAudioMediaType(*ppmt);

    AUDIO_STREAM_CONFIG_CAPS* caps = (AUDIO_STREAM_CONFIG_CAPS*)pSCC;
    memset(caps, 0, sizeof(AUDIO_STREAM_CONFIG_CAPS));
    caps->guid = FORMAT_WaveFormatEx;
    caps->MinimumChannels = 1;
    caps->MaximumChannels = 2;
    caps->ChannelsGranularity = 1;
    caps->MinimumBitsPerSample = 16;
    caps->MaximumBitsPerSample = 16;
    caps->BitsPerSampleGranularity = 0;
    caps->MinimumSampleFrequency = 48000;
    caps->MaximumSampleFrequency = 48000;
    caps->SampleFrequencyGranularity = 0;
    return S_OK;
}

STDMETHODIMP BouleCamAudioPin::Set(REFGUID, DWORD, LPVOID, DWORD, LPVOID, DWORD) { return E_NOTIMPL; }

STDMETHODIMP BouleCamAudioPin::Get(REFGUID guidPropSet, DWORD dwPropID, LPVOID, DWORD, LPVOID pPropData, DWORD cbPropData, DWORD* pcbReturned) {
    if (guidPropSet == AMPROPSETID_Pin && dwPropID == 0) {
        if (cbPropData < sizeof(GUID)) return E_UNEXPECTED;
        if (!pPropData) return E_POINTER;
        *(GUID*)pPropData = PIN_CATEGORY_CAPTURE;
        if (pcbReturned) *pcbReturned = sizeof(GUID);
        return S_OK;
    }
    return E_PROP_ID_UNSUPPORTED;
}

STDMETHODIMP BouleCamAudioPin::QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD* pTypeSupport) {
    if (guidPropSet == AMPROPSETID_Pin && dwPropID == 0) {
        if (pTypeSupport) *pTypeSupport = KSPROPERTY_SUPPORT_GET;
        return S_OK;
    }
    return E_PROP_ID_UNSUPPORTED;
}

void BouleCamAudioPin::StartStreaming() {
    if (m_isStreaming.exchange(true)) return;
    m_streamThread = std::thread(&BouleCamAudioPin::StreamingWorker, this);
}

void BouleCamAudioPin::StopStreaming() {
    if (!m_isStreaming.exchange(false)) return;
    if (m_streamThread.joinable()) {
        m_streamThread.join();
    }
}

void BouleCamAudioPin::StreamingWorker() {
    REFERENCE_TIME sampleTime = 0;
    REFERENCE_TIME packetDuration = 200000; // 20ms = 200,000 in 100ns units

    while (m_isStreaming.load()) {
        auto tStart = std::chrono::steady_clock::now();

        IMemAllocator* pAlloc = m_pAllocator;
        IMemInputPin* pInput = m_pMemInputPin;

        if (pAlloc && pInput) {
            IMediaSample* pSample = nullptr;
            HRESULT hr = pAlloc->GetBuffer(&pSample, nullptr, nullptr, 0);
            if (SUCCEEDED(hr) && pSample) {
                BYTE* pDst = nullptr;
                pSample->GetPointer(&pDst);
                long dstSize = pSample->GetSize();

                uint32_t outSize = 0, sampleRate = 48000, channels = 1;
                bool gotAudio = m_shmConsumer.ReadLatestAudio(pDst, dstSize, outSize, sampleRate, channels);
                if (!gotAudio) {
                    memset(pDst, 0, dstSize);
                    outSize = dstSize;
                }

                pSample->SetActualDataLength(outSize > 0 ? outSize : dstSize);
                REFERENCE_TIME t1 = sampleTime;
                REFERENCE_TIME t2 = sampleTime + packetDuration;
                pSample->SetTime(&t1, &t2);
                pSample->SetSyncPoint(TRUE);
                pSample->SetPreroll(FALSE);
                pSample->SetDiscontinuity(FALSE);

                pInput->Receive(pSample);
                pSample->Release();
                sampleTime += packetDuration;
            }
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - tStart
        ).count();
        if (elapsed < 20) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20 - elapsed));
        }
    }
}

// =========================================================================
// AUDIO FILTER IMPLEMENTATION
// =========================================================================

BouleCamAudioFilter::BouleCamAudioFilter(int camIndex)
    : m_camIndex(camIndex) {
    m_pPin = new BouleCamAudioPin(this, camIndex);
}

BouleCamAudioFilter::~BouleCamAudioFilter() {
    Stop();
    if (m_pPin) {
        m_pPin->Release();
        m_pPin = nullptr;
    }
}

STDMETHODIMP BouleCamAudioFilter::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IPersist || riid == IID_IMediaFilter || riid == IID_IBaseFilter) {
        *ppv = static_cast<IBaseFilter*>(this);
        AddRef();
        return S_OK;
    } else if (riid == IID_IAMFilterMiscFlags) {
        *ppv = static_cast<IAMFilterMiscFlags*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) BouleCamAudioFilter::AddRef() { return ++m_cRef; }
STDMETHODIMP_(ULONG) BouleCamAudioFilter::Release() {
    ULONG ref = --m_cRef;
    if (ref == 0) delete this;
    return ref;
}

STDMETHODIMP BouleCamAudioFilter::GetClassID(CLSID* pClassID) {
    if (!pClassID) return E_POINTER;
    if (m_camIndex == 2) *pClassID = CLSID_BouleCamAudioSource2;
    else if (m_camIndex == 3) *pClassID = CLSID_BouleCamAudioSource3;
    else *pClassID = CLSID_BouleCamAudioSource;
    return S_OK;
}

STDMETHODIMP BouleCamAudioFilter::Stop() {
    std::lock_guard<std::mutex> lock(m_filterMutex);
    m_state = State_Stopped;
    if (m_pPin) m_pPin->StopStreaming();
    return S_OK;
}

STDMETHODIMP BouleCamAudioFilter::Pause() {
    std::lock_guard<std::mutex> lock(m_filterMutex);
    m_state = State_Paused;
    if (m_pPin) m_pPin->StartStreaming();
    return S_OK;
}

STDMETHODIMP BouleCamAudioFilter::Run(REFERENCE_TIME) {
    std::lock_guard<std::mutex> lock(m_filterMutex);
    m_state = State_Running;
    if (m_pPin) m_pPin->StartStreaming();
    return S_OK;
}

STDMETHODIMP BouleCamAudioFilter::GetState(DWORD, FILTER_STATE* State) {
    if (!State) return E_POINTER;
    std::lock_guard<std::mutex> lock(m_filterMutex);
    *State = m_state;
    return S_OK;
}

STDMETHODIMP BouleCamAudioFilter::SetSyncSource(IReferenceClock* pClock) {
    std::lock_guard<std::mutex> lock(m_filterMutex);
    if (m_pClock) m_pClock->Release();
    m_pClock = pClock;
    if (m_pClock) m_pClock->AddRef();
    return S_OK;
}

STDMETHODIMP BouleCamAudioFilter::GetSyncSource(IReferenceClock** pClock) {
    if (!pClock) return E_POINTER;
    std::lock_guard<std::mutex> lock(m_filterMutex);
    *pClock = m_pClock;
    if (*pClock) (*pClock)->AddRef();
    return S_OK;
}

STDMETHODIMP BouleCamAudioFilter::EnumPins(IEnumPins** ppEnum) {
    if (!ppEnum) return E_POINTER;
    *ppEnum = new BouleCamEnumPins(m_pPin);
    return S_OK;
}

STDMETHODIMP BouleCamAudioFilter::FindPin(LPCWSTR Id, IPin** ppPin) {
    if (!ppPin) return E_POINTER;
    if (Id && wcscmp(Id, L"Audio Capture") == 0) {
        *ppPin = m_pPin;
        if (*ppPin) (*ppPin)->AddRef();
        return S_OK;
    }
    *ppPin = nullptr;
    return VFW_E_NOT_FOUND;
}

STDMETHODIMP BouleCamAudioFilter::QueryFilterInfo(FILTER_INFO* pInfo) {
    if (!pInfo) return E_POINTER;
    if (m_camIndex == 2)      wcscpy_s(pInfo->achName, L"BouleCam Audio 2");
    else if (m_camIndex == 3) wcscpy_s(pInfo->achName, L"BouleCam Audio 3");
    else                      wcscpy_s(pInfo->achName, L"BouleCam Audio");
    pInfo->pGraph = m_pGraph;
    if (pInfo->pGraph) pInfo->pGraph->AddRef();
    return S_OK;
}

STDMETHODIMP BouleCamAudioFilter::JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR) {
    std::lock_guard<std::mutex> lock(m_filterMutex);
    m_pGraph = pGraph;
    return S_OK;
}

STDMETHODIMP BouleCamAudioFilter::QueryVendorInfo(LPWSTR* pVendorInfo) {
    if (!pVendorInfo) return E_POINTER;
    *pVendorInfo = (LPWSTR)CoTaskMemAlloc(sizeof(L"BouleCam"));
    wcscpy_s(*pVendorInfo, 9, L"BouleCam");
    return S_OK;
}

STDMETHODIMP_(ULONG) BouleCamAudioFilter::GetMiscFlags() {
    return AM_FILTER_MISC_FLAGS_IS_SOURCE;
}

} // namespace boulecam
