#pragma once

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <atomic>
#include "media_stream.h"

namespace boulecam {

class VirtualCameraSource : public IMFMediaSource, public IMFGetService {
public:
    VirtualCameraSource();
    virtual ~VirtualCameraSource();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue) override;

    // IMFMediaSource
    STDMETHODIMP GetCharacteristics(DWORD* pdwCharacteristics) override;
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** ppPresentationDescriptor) override;
    STDMETHODIMP Start(IMFPresentationDescriptor* pPresentationDescriptor, const GUID* pguidTimeFormat, const PROPVARIANT* pvarStartPosition) override;
    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Shutdown() override;

    // IMFGetService
    STDMETHODIMP GetService(REFGUID guidService, REFIID riid, LPVOID* ppvObject) override;

private:
    HRESULT CreateDescriptors();

    std::atomic<ULONG> m_cRef;
    IMFMediaEventQueue* m_pEventQueue;
    IMFPresentationDescriptor* m_pPresentationDescriptor;
    VirtualCameraStream* m_pStream;
    bool m_isShutdown;
};

} // namespace boulecam
