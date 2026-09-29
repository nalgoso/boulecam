#include <windows.h>
#include <dshow.h>
#include "dshow_filter.h"
#include <shlwapi.h>

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shlwapi.lib")

// Camera 1 (primary)
static const wchar_t* g_wszVideoCam1Clsid    = L"{6B47C010-85A4-4D6C-9A52-2A1E7F19D3B1}";
static const wchar_t* g_wszAudioCam1Clsid    = L"{6B47C020-85A4-4D6C-9A52-2A1E7F19D3B1}";
// Camera 2
static const wchar_t* g_wszVideoCam2Clsid    = L"{6B47C011-85A4-4D6C-9A52-2A1E7F19D3B1}";
static const wchar_t* g_wszAudioCam2Clsid    = L"{6B47C021-85A4-4D6C-9A52-2A1E7F19D3B1}";
// Camera 3
static const wchar_t* g_wszVideoCam3Clsid    = L"{6B47C012-85A4-4D6C-9A52-2A1E7F19D3B1}";
static const wchar_t* g_wszAudioCam3Clsid    = L"{6B47C022-85A4-4D6C-9A52-2A1E7F19D3B1}";

static const wchar_t* g_wszVideoCategory = L"{860BB310-5D01-11d0-BD3B-00A0C911CE86}"; // CLSID_VideoInputDeviceCategory
static const wchar_t* g_wszAudioCategory = L"{33D9A762-90C8-11d0-BD43-00A0C911CE86}"; // CLSID_AudioInputDeviceCategory

static const wchar_t* g_wszVideoCam1Name = L"BouleCam Virtual Camera";
static const wchar_t* g_wszAudioCam1Name = L"BouleCam Audio";
static const wchar_t* g_wszVideoCam2Name = L"BouleCam Virtual Camera 2";
static const wchar_t* g_wszAudioCam2Name = L"BouleCam Audio 2";
static const wchar_t* g_wszVideoCam3Name = L"BouleCam Virtual Camera 3";
static const wchar_t* g_wszAudioCam3Name = L"BouleCam Audio 3";

HMODULE g_hModule = NULL;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

// =========================================================================
// CLASS FACTORIES
// =========================================================================

class GenericClassFactory : public IClassFactory {
public:
    enum FactoryType { TYPE_VIDEO, TYPE_AUDIO };
    GenericClassFactory(FactoryType type, int camIndex) : m_cRef(1), m_type(type), m_camIndex(camIndex) {}
    virtual ~GenericClassFactory() {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
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

    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        *ppvObject = nullptr;
        if (pUnkOuter != nullptr) return CLASS_E_NOAGGREGATION;

        if (m_type == TYPE_VIDEO) {
            boulecam::BouleCamVideoFilter* pFilter = new boulecam::BouleCamVideoFilter(m_camIndex);
            HRESULT hr = pFilter->QueryInterface(riid, ppvObject);
            pFilter->Release();
            return hr;
        } else {
            boulecam::BouleCamAudioFilter* pFilter = new boulecam::BouleCamAudioFilter(m_camIndex);
            HRESULT hr = pFilter->QueryInterface(riid, ppvObject);
            pFilter->Release();
            return hr;
        }
    }

    STDMETHODIMP LockServer(BOOL) override { return S_OK; }

private:
    std::atomic<ULONG> m_cRef;
    FactoryType m_type;
    int m_camIndex;
};

extern "C" __declspec(dllexport) HRESULT STDAPICALLTYPE DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;

    // Cam 1
    if (IsEqualCLSID(rclsid, boulecam::CLSID_BouleCamVirtualCam)) {
        GenericClassFactory* pFactory = new GenericClassFactory(GenericClassFactory::TYPE_VIDEO, 1);
        HRESULT hr = pFactory->QueryInterface(riid, ppv);
        pFactory->Release();
        return hr;
    } else if (IsEqualCLSID(rclsid, boulecam::CLSID_BouleCamAudioSource)) {
        GenericClassFactory* pFactory = new GenericClassFactory(GenericClassFactory::TYPE_AUDIO, 1);
        HRESULT hr = pFactory->QueryInterface(riid, ppv);
        pFactory->Release();
        return hr;
    }
    // Cam 2
    else if (IsEqualCLSID(rclsid, boulecam::CLSID_BouleCamVirtualCam2)) {
        GenericClassFactory* pFactory = new GenericClassFactory(GenericClassFactory::TYPE_VIDEO, 2);
        HRESULT hr = pFactory->QueryInterface(riid, ppv);
        pFactory->Release();
        return hr;
    } else if (IsEqualCLSID(rclsid, boulecam::CLSID_BouleCamAudioSource2)) {
        GenericClassFactory* pFactory = new GenericClassFactory(GenericClassFactory::TYPE_AUDIO, 2);
        HRESULT hr = pFactory->QueryInterface(riid, ppv);
        pFactory->Release();
        return hr;
    }
    // Cam 3
    else if (IsEqualCLSID(rclsid, boulecam::CLSID_BouleCamVirtualCam3)) {
        GenericClassFactory* pFactory = new GenericClassFactory(GenericClassFactory::TYPE_VIDEO, 3);
        HRESULT hr = pFactory->QueryInterface(riid, ppv);
        pFactory->Release();
        return hr;
    } else if (IsEqualCLSID(rclsid, boulecam::CLSID_BouleCamAudioSource3)) {
        GenericClassFactory* pFactory = new GenericClassFactory(GenericClassFactory::TYPE_AUDIO, 3);
        HRESULT hr = pFactory->QueryInterface(riid, ppv);
        pFactory->Release();
        return hr;
    }

    *ppv = nullptr;
    return CLASS_E_CLASSNOTAVAILABLE;
}

extern "C" __declspec(dllexport) HRESULT STDAPICALLTYPE DllCanUnloadNow(void) {
    return S_OK;
}

// Standard DirectShow FilterData binary payload for 1 output capture pin
static const unsigned char g_videoFilterData[] = {
    0x02, 0x00, 0x00, 0x00, // Version 2
    0x00, 0x00, 0x20, 0x00, // Merit MERIT_DO_NOT_USE (0x00200000)
    0x01, 0x00, 0x00, 0x00, // 1 Pin
    0x00, 0x00, 0x00, 0x00, // Zero
    0x30, 0x31, 0x70, 0x69, // Pin Signature "01pi"
    0x04, 0x00, 0x00, 0x00, // REG_PINFLAG_B_OUTPUT (4)
    0x01, 0x00, 0x00, 0x00, // 1 Instance
    0x01, 0x00, 0x00, 0x00, // 1 Media Type
    0x81, 0x42, 0x6c, 0xfb, 0x53, 0x03, 0xd1, 0x11, 0x90, 0x5f, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba, // PIN_CATEGORY_CAPTURE
    0x76, 0x69, 0x64, 0x73, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71, // MEDIATYPE_Video
    0x4e, 0x56, 0x31, 0x32, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71  // MEDIASUBTYPE_NV12
};

static const unsigned char g_audioFilterData[] = {
    0x02, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x20, 0x00,
    0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x30, 0x31, 0x70, 0x69,
    0x04, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00,
    0x81, 0x42, 0x6c, 0xfb, 0x53, 0x03, 0xd1, 0x11, 0x90, 0x5f, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba, // PIN_CATEGORY_CAPTURE
    0x61, 0x75, 0x64, 0x73, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71, // MEDIATYPE_Audio
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71  // MEDIASUBTYPE_PCM
};

static void RegisterDevice(HKEY rootKey, const wchar_t* clsidStr, const wchar_t* categoryStr,
                           const wchar_t* friendlyName, const wchar_t* dllPath,
                           const unsigned char* filterData, DWORD filterDataSize) {
    wchar_t szKey[512];
    HKEY hKey = NULL;

    // 1. Register COM CLSID InprocServer32
    wsprintfW(szKey, L"Software\\Classes\\CLSID\\%s", clsidStr);
    if (RegCreateKeyExW(rootKey, szKey, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, NULL, 0, REG_SZ, (const BYTE*)friendlyName, (DWORD)((wcslen(friendlyName) + 1) * sizeof(wchar_t)));

        HKEY hInprocKey = NULL;
        if (RegCreateKeyExW(hKey, L"InprocServer32", 0, NULL, 0, KEY_WRITE, NULL, &hInprocKey, NULL) == ERROR_SUCCESS) {
            RegSetValueExW(hInprocKey, NULL, 0, REG_SZ, (const BYTE*)dllPath, (DWORD)((wcslen(dllPath) + 1) * sizeof(wchar_t)));
            const wchar_t* szThreading = L"Both";
            RegSetValueExW(hInprocKey, L"ThreadingModel", 0, REG_SZ, (const BYTE*)szThreading, (DWORD)((wcslen(szThreading) + 1) * sizeof(wchar_t)));
            RegCloseKey(hInprocKey);
        }
        RegCloseKey(hKey);
    }

    // 2. Register DirectShow Device Category Instance strictly under clsidStr
    wsprintfW(szKey, L"Software\\Classes\\CLSID\\%s\\Instance\\%s", categoryStr, clsidStr);
    if (RegCreateKeyExW(rootKey, szKey, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, L"FriendlyName", 0, REG_SZ, (const BYTE*)friendlyName, (DWORD)((wcslen(friendlyName) + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"CLSID", 0, REG_SZ, (const BYTE*)clsidStr, (DWORD)((wcslen(clsidStr) + 1) * sizeof(wchar_t)));
        if (filterData && filterDataSize > 0) {
            RegSetValueExW(hKey, L"FilterData", 0, REG_BINARY, filterData, filterDataSize);
        }
        RegCloseKey(hKey);
    }

    // Explicitly delete any legacy key named after friendlyName to prevent duplicate entries in OBS
    wsprintfW(szKey, L"Software\\Classes\\CLSID\\%s\\Instance\\%s", categoryStr, friendlyName);
    RegDeleteKeyW(rootKey, szKey);
}

static void UnregisterDevice(HKEY rootKey, const wchar_t* clsidStr, const wchar_t* categoryStr, const wchar_t* friendlyName) {
    wchar_t szKey[512];
    wsprintfW(szKey, L"Software\\Classes\\CLSID\\%s\\Instance\\%s", categoryStr, clsidStr);
    RegDeleteKeyW(rootKey, szKey);

    wsprintfW(szKey, L"Software\\Classes\\CLSID\\%s\\Instance\\%s", categoryStr, friendlyName);
    RegDeleteKeyW(rootKey, szKey);

    wsprintfW(szKey, L"Software\\Classes\\CLSID\\%s\\InprocServer32", clsidStr);
    RegDeleteKeyW(rootKey, szKey);

    wsprintfW(szKey, L"Software\\Classes\\CLSID\\%s", clsidStr);
    RegDeleteKeyW(rootKey, szKey);
}

// Helper to register all 3 cameras on a given root key
static void RegisterAllCams(HKEY rootKey, const wchar_t* dllPath) {
    RegisterDevice(rootKey, g_wszVideoCam1Clsid, g_wszVideoCategory, g_wszVideoCam1Name, dllPath, g_videoFilterData, sizeof(g_videoFilterData));
    RegisterDevice(rootKey, g_wszAudioCam1Clsid, g_wszAudioCategory, g_wszAudioCam1Name, dllPath, g_audioFilterData, sizeof(g_audioFilterData));
    RegisterDevice(rootKey, g_wszVideoCam2Clsid, g_wszVideoCategory, g_wszVideoCam2Name, dllPath, g_videoFilterData, sizeof(g_videoFilterData));
    RegisterDevice(rootKey, g_wszAudioCam2Clsid, g_wszAudioCategory, g_wszAudioCam2Name, dllPath, g_audioFilterData, sizeof(g_audioFilterData));
    RegisterDevice(rootKey, g_wszVideoCam3Clsid, g_wszVideoCategory, g_wszVideoCam3Name, dllPath, g_videoFilterData, sizeof(g_videoFilterData));
    RegisterDevice(rootKey, g_wszAudioCam3Clsid, g_wszAudioCategory, g_wszAudioCam3Name, dllPath, g_audioFilterData, sizeof(g_audioFilterData));
}

static void UnregisterAllCams(HKEY rootKey) {
    UnregisterDevice(rootKey, g_wszVideoCam1Clsid, g_wszVideoCategory, g_wszVideoCam1Name);
    UnregisterDevice(rootKey, g_wszAudioCam1Clsid, g_wszAudioCategory, g_wszAudioCam1Name);
    UnregisterDevice(rootKey, g_wszVideoCam2Clsid, g_wszVideoCategory, g_wszVideoCam2Name);
    UnregisterDevice(rootKey, g_wszAudioCam2Clsid, g_wszAudioCategory, g_wszAudioCam2Name);
    UnregisterDevice(rootKey, g_wszVideoCam3Clsid, g_wszVideoCategory, g_wszVideoCam3Name);
    UnregisterDevice(rootKey, g_wszAudioCam3Clsid, g_wszAudioCategory, g_wszAudioCam3Name);
}

extern "C" __declspec(dllexport) HRESULT STDAPICALLTYPE DllRegisterServer(void) {
    wchar_t szModulePath[MAX_PATH];
    if (GetModuleFileNameW(g_hModule, szModulePath, MAX_PATH) == 0) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    RegisterAllCams(HKEY_CURRENT_USER, szModulePath);
    RegisterAllCams(HKEY_LOCAL_MACHINE, szModulePath);

    // Clean up stale entries from older versions
    const wchar_t* staleClsids[] = {
        L"{6B47C010-85A4-4D6C-9A52-2A1E7F19D3B1}", // old cam1 video registered under friendlyName
        L"{6B47C020-85A4-4D6C-9A52-2A1E7F19D3B1}"  // old cam1 audio registered under friendlyName
    };
    const wchar_t* staleNames[] = { g_wszVideoCam1Name, g_wszAudioCam1Name };
    const wchar_t* staleCategories[] = { g_wszVideoCategory, g_wszAudioCategory };
    for (int i = 0; i < 2; ++i) {
        wchar_t staleKey[512];
        wsprintfW(staleKey, L"Software\\Classes\\CLSID\\%s\\Instance\\%s", staleCategories[i], staleNames[i]);
        RegDeleteKeyW(HKEY_CURRENT_USER,  staleKey);
        RegDeleteKeyW(HKEY_LOCAL_MACHINE, staleKey);
        wsprintfW(staleKey, L"Software\\Classes\\CLSID\\%s\\Instance\\DirectShow Softcam", staleCategories[i]);
        RegDeleteKeyW(HKEY_CURRENT_USER,  staleKey);
        RegDeleteKeyW(HKEY_LOCAL_MACHINE, staleKey);
    }

    return S_OK;
}

extern "C" __declspec(dllexport) HRESULT STDAPICALLTYPE DllUnregisterServer(void) {
    UnregisterAllCams(HKEY_CURRENT_USER);
    UnregisterAllCams(HKEY_LOCAL_MACHINE);
    return S_OK;
}
