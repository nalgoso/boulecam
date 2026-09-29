#pragma once

#include <windows.h>
#include <dshow.h>
#include <initguid.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include "ipc/shm_consumer.h"

namespace boulecam {

// BouleCam DirectShow Filter GUIDs
// Cam 1 (primary) — original GUIDs, always registered
// Video: {6B47C010-85A4-4D6C-9A52-2A1E7F19D3B1}
DEFINE_GUID(CLSID_BouleCamVirtualCam,
    0x6b47c010, 0x85a4, 0x4d6c, 0x9a, 0x52, 0x2a, 0x1e, 0x7f, 0x19, 0xd3, 0xb1);

// Audio: {6B47C020-85A4-4D6C-9A52-2A1E7F19D3B1}
DEFINE_GUID(CLSID_BouleCamAudioSource,
    0x6b47c020, 0x85a4, 0x4d6c, 0x9a, 0x52, 0x2a, 0x1e, 0x7f, 0x19, 0xd3, 0xb1);

// Cam 2 — Video: {6B47C011-85A4-4D6C-9A52-2A1E7F19D3B1}
DEFINE_GUID(CLSID_BouleCamVirtualCam2,
    0x6b47c011, 0x85a4, 0x4d6c, 0x9a, 0x52, 0x2a, 0x1e, 0x7f, 0x19, 0xd3, 0xb1);

// Cam 2 — Audio: {6B47C021-85A4-4D6C-9A52-2A1E7F19D3B1}
DEFINE_GUID(CLSID_BouleCamAudioSource2,
    0x6b47c021, 0x85a4, 0x4d6c, 0x9a, 0x52, 0x2a, 0x1e, 0x7f, 0x19, 0xd3, 0xb1);

// Cam 3 — Video: {6B47C012-85A4-4D6C-9A52-2A1E7F19D3B1}
DEFINE_GUID(CLSID_BouleCamVirtualCam3,
    0x6b47c012, 0x85a4, 0x4d6c, 0x9a, 0x52, 0x2a, 0x1e, 0x7f, 0x19, 0xd3, 0xb1);

// Cam 3 — Audio: {6B47C022-85A4-4D6C-9A52-2A1E7F19D3B1}
DEFINE_GUID(CLSID_BouleCamAudioSource3,
    0x6b47c022, 0x85a4, 0x4d6c, 0x9a, 0x52, 0x2a, 0x1e, 0x7f, 0x19, 0xd3, 0xb1);

// Forward declarations
class BouleCamVideoFilter;
class BouleCamVideoPin;
class BouleCamAudioFilter;
class BouleCamAudioPin;

// =========================================================================
// VIDEO FILTER & PIN
// =========================================================================

class BouleCamVideoPin : public IPin,
                         public IAMStreamConfig,
                         public IKsPropertySet,
                         public IQualityControl {
public:
    // camIndex 1..3 — selects which SHM slot to read from
    BouleCamVideoPin(BouleCamVideoFilter* pFilter, int camIndex = 1);
    virtual ~BouleCamVideoPin();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IPin
    STDMETHODIMP Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP Disconnect() override;
    STDMETHODIMP ConnectedTo(IPin** pPin) override;
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP QueryPinInfo(PIN_INFO* pInfo) override;
    STDMETHODIMP QueryDirection(PIN_DIRECTION* pPinDir) override;
    STDMETHODIMP QueryId(LPWSTR* Id) override;
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** ppEnum) override;
    STDMETHODIMP QueryInternalConnections(IPin** apPin, ULONG* nPin) override;
    STDMETHODIMP EndOfStream() override;
    STDMETHODIMP BeginFlush() override;
    STDMETHODIMP EndFlush() override;
    STDMETHODIMP NewSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double dRate) override;

    // IAMStreamConfig
    STDMETHODIMP SetFormat(AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE** ppmt) override;
    STDMETHODIMP GetNumberOfCapabilities(int* piCount, int* piSize) override;
    STDMETHODIMP GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) override;

    // IKsPropertySet (OBS queries pin category)
    STDMETHODIMP Set(REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData, DWORD cbInstanceData, LPVOID pPropData, DWORD cbPropData) override;
    STDMETHODIMP Get(REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData, DWORD cbInstanceData, LPVOID pPropData, DWORD cbPropData, DWORD* pcbReturned) override;
    STDMETHODIMP QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD* pTypeSupport) override;

    // IQualityControl
    STDMETHODIMP Notify(IBaseFilter* pSelf, Quality q) override;
    STDMETHODIMP SetSink(IQualityControl* piqc) override;

    // Streaming Thread
    void StartStreaming();
    void StopStreaming();

private:
    void StreamingWorker();
    bool CreateMediaTypes(AM_MEDIA_TYPE* pmt, int index);

    std::atomic<ULONG> m_cRef{1};
    BouleCamVideoFilter* m_pFilter = nullptr;
    IPin* m_pConnectedPin = nullptr;
    IMemInputPin* m_pMemInputPin = nullptr;
    IMemAllocator* m_pAllocator = nullptr;

    AM_MEDIA_TYPE m_currentMediaType{};
    GUID m_currentSubtype = MEDIASUBTYPE_NV12;
    uint32_t m_width = 1920;
    uint32_t m_height = 1080;
    REFERENCE_TIME m_frameDuration = 166666; // 60 FPS

    std::atomic<bool> m_isStreaming{false};
    std::thread m_streamThread;
    ShmConsumer m_shmConsumer;
    std::mutex m_pinMutex;
    int m_camIndex = 1;
};

class BouleCamVideoFilter : public IBaseFilter,
                            public IAMFilterMiscFlags {
public:
    // camIndex 1..3
    BouleCamVideoFilter(int camIndex = 1);
    virtual ~BouleCamVideoFilter();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IPersist
    STDMETHODIMP GetClassID(CLSID* pClassID) override;

    // IMediaFilter
    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Run(REFERENCE_TIME tStart) override;
    STDMETHODIMP GetState(DWORD dwMilliSecsTimeout, FILTER_STATE* State) override;
    STDMETHODIMP SetSyncSource(IReferenceClock* pClock) override;
    STDMETHODIMP GetSyncSource(IReferenceClock** pClock) override;

    // IBaseFilter
    STDMETHODIMP EnumPins(IEnumPins** ppEnum) override;
    STDMETHODIMP FindPin(LPCWSTR Id, IPin** ppPin) override;
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* pInfo) override;
    STDMETHODIMP JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName) override;
    STDMETHODIMP QueryVendorInfo(LPWSTR* pVendorInfo) override;

    // IAMFilterMiscFlags
    STDMETHODIMP_(ULONG) GetMiscFlags() override;

    int GetCamIndex() const { return m_camIndex; }

private:
    std::atomic<ULONG> m_cRef{1};
    FILTER_STATE m_state = State_Stopped;
    IReferenceClock* m_pClock = nullptr;
    IFilterGraph* m_pGraph = nullptr;
    BouleCamVideoPin* m_pPin = nullptr;
    std::mutex m_filterMutex;
    int m_camIndex = 1;
};

// =========================================================================
// AUDIO FILTER & PIN
// =========================================================================

class BouleCamAudioPin : public IPin,
                         public IAMStreamConfig,
                         public IKsPropertySet {
public:
    BouleCamAudioPin(BouleCamAudioFilter* pFilter, int camIndex = 1);
    virtual ~BouleCamAudioPin();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IPin
    STDMETHODIMP Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP Disconnect() override;
    STDMETHODIMP ConnectedTo(IPin** pPin) override;
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP QueryPinInfo(PIN_INFO* pInfo) override;
    STDMETHODIMP QueryDirection(PIN_DIRECTION* pPinDir) override;
    STDMETHODIMP QueryId(LPWSTR* Id) override;
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** ppEnum) override;
    STDMETHODIMP QueryInternalConnections(IPin** apPin, ULONG* nPin) override;
    STDMETHODIMP EndOfStream() override;
    STDMETHODIMP BeginFlush() override;
    STDMETHODIMP EndFlush() override;
    STDMETHODIMP NewSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double dRate) override;

    // IAMStreamConfig
    STDMETHODIMP SetFormat(AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE** ppmt) override;
    STDMETHODIMP GetNumberOfCapabilities(int* piCount, int* piSize) override;
    STDMETHODIMP GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) override;

    // IKsPropertySet
    STDMETHODIMP Set(REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData, DWORD cbInstanceData, LPVOID pPropData, DWORD cbPropData) override;
    STDMETHODIMP Get(REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData, DWORD cbInstanceData, LPVOID pPropData, DWORD cbPropData, DWORD* pcbReturned) override;
    STDMETHODIMP QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD* pTypeSupport) override;

    void StartStreaming();
    void StopStreaming();

private:
    void StreamingWorker();

    std::atomic<ULONG> m_cRef{1};
    BouleCamAudioFilter* m_pFilter = nullptr;
    IPin* m_pConnectedPin = nullptr;
    IMemInputPin* m_pMemInputPin = nullptr;
    IMemAllocator* m_pAllocator = nullptr;

    AM_MEDIA_TYPE m_currentMediaType{};
    std::atomic<bool> m_isStreaming{false};
    std::thread m_streamThread;
    ShmConsumer m_shmConsumer;
    std::mutex m_pinMutex;
    int m_camIndex = 1;
};

class BouleCamAudioFilter : public IBaseFilter,
                            public IAMFilterMiscFlags {
public:
    BouleCamAudioFilter(int camIndex = 1);
    virtual ~BouleCamAudioFilter();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IPersist
    STDMETHODIMP GetClassID(CLSID* pClassID) override;

    // IMediaFilter
    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Run(REFERENCE_TIME tStart) override;
    STDMETHODIMP GetState(DWORD dwMilliSecsTimeout, FILTER_STATE* State) override;
    STDMETHODIMP SetSyncSource(IReferenceClock* pClock) override;
    STDMETHODIMP GetSyncSource(IReferenceClock** pClock) override;

    // IBaseFilter
    STDMETHODIMP EnumPins(IEnumPins** ppEnum) override;
    STDMETHODIMP FindPin(LPCWSTR Id, IPin** ppPin) override;
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* pInfo) override;
    STDMETHODIMP JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName) override;
    STDMETHODIMP QueryVendorInfo(LPWSTR* pVendorInfo) override;

    // IAMFilterMiscFlags
    STDMETHODIMP_(ULONG) GetMiscFlags() override;

    int GetCamIndex() const { return m_camIndex; }

private:
    std::atomic<ULONG> m_cRef{1};
    FILTER_STATE m_state = State_Stopped;
    IReferenceClock* m_pClock = nullptr;
    IFilterGraph* m_pGraph = nullptr;
    BouleCamAudioPin* m_pPin = nullptr;
    std::mutex m_filterMutex;
    int m_camIndex = 1;
};

} // namespace boulecam
