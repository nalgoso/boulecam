#include "class_factory.h"
#include "virtual_camera_source.h"

namespace boulecam {

VirtualCameraClassFactory::VirtualCameraClassFactory() : m_cRef(1) {}
VirtualCameraClassFactory::~VirtualCameraClassFactory() {}

STDMETHODIMP VirtualCameraClassFactory::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IClassFactory) {
        *ppv = static_cast<IClassFactory*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) VirtualCameraClassFactory::AddRef() {
    return ++m_cRef;
}

STDMETHODIMP_(ULONG) VirtualCameraClassFactory::Release() {
    ULONG ref = --m_cRef;
    if (ref == 0) delete this;
    return ref;
}

STDMETHODIMP VirtualCameraClassFactory::CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;
    if (pUnkOuter != nullptr) return CLASS_E_NOAGGREGATION;

    VirtualCameraSource* pSource = new VirtualCameraSource();
    HRESULT hr = pSource->QueryInterface(riid, ppvObject);
    pSource->Release();
    return hr;
}

STDMETHODIMP VirtualCameraClassFactory::LockServer(BOOL fLock) {
    return S_OK;
}

} // namespace boulecam
