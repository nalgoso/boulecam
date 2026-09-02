#pragma once

#include <windows.h>
#include <unknwn.h>
#include <atomic>

namespace boulecam {

class VirtualCameraClassFactory : public IClassFactory {
public:
    VirtualCameraClassFactory();
    virtual ~VirtualCameraClassFactory();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IClassFactory
    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject) override;
    STDMETHODIMP LockServer(BOOL fLock) override;

private:
    std::atomic<ULONG> m_cRef;
};

} // namespace boulecam
