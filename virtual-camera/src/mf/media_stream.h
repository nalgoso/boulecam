#pragma once

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <atomic>
#include <thread>
#include <vector>
#include "../ipc/shm_consumer.h"

namespace boulecam {

class VirtualCameraSource;

class VirtualCameraStream : public IMFMediaStream {
public:
    VirtualCameraStream(VirtualCameraSource* pSource, IMFStreamDescriptor* pSD);
    virtual ~VirtualCameraStream();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue) override;

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** ppMediaSource) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor) override;
    STDMETHODIMP RequestSample(IUnknown* pToken) override;

    // Internal controls called by Source
    HRESULT Start();
    HRESULT Stop();
    HRESULT Shutdown();

private:
    void DeliverFrameSample();

    std::atomic<ULONG> m_cRef;
    VirtualCameraSource* m_pSource;
    IMFStreamDescriptor* m_pStreamDescriptor;
    IMFMediaEventQueue* m_pEventQueue;
    ShmConsumer m_shmConsumer;

    std::vector<uint8_t> m_frameBuffer;
    uint64_t m_sampleTimestamp100ns;
    bool m_isStreaming;
    bool m_isShutdown;
};

} // namespace boulecam
