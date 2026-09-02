#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <shlwapi.h>
#include "mf/class_factory.h"
#include "boulecam_ipc.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "shlwapi.lib")

// BouleCam Virtual Camera CLSID: {6B47C010-85A4-4D6C-9A52-2A1E7F19D3B1}
static const GUID CLSID_BouleCamMediaSource = 
{ 0x6b47c010, 0x85a4, 0x4d6c, { 0x9a, 0x52, 0x2a, 0x1e, 0x7f, 0x19, 0xd3, 0xb1 } };

static const wchar_t* g_wszClsid = L"{6B47C010-85A4-4D6C-9A52-2A1E7F19D3B1}";
static const wchar_t* g_wszFriendlyName = L"BouleCam Virtual Camera";

HMODULE g_hModule = NULL;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        break;
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (IsEqualCLSID(rclsid, CLSID_BouleCamMediaSource)) {
        boulecam::VirtualCameraClassFactory* pFactory = new boulecam::VirtualCameraClassFactory();
        HRESULT hr = pFactory->QueryInterface(riid, ppv);
        pFactory->Release();
        return hr;
    }
    *ppv = nullptr;
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow(void) {
    return S_OK;
}

STDAPI DllRegisterServer(void) {
    wchar_t szModulePath[MAX_PATH];
    if (GetModuleFileNameW(g_hModule, szModulePath, MAX_PATH) == 0) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    // Register CLSID in HKCR\CLSID\{6B47C010-85A4-4D6C-9A52-2A1E7F19D3B1}
    wchar_t szKey[256];
    wsprintfW(szKey, L"CLSID\\%s", g_wszClsid);

    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CLASSES_ROOT, szKey, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, NULL, 0, REG_SZ, (const BYTE*)g_wszFriendlyName, (DWORD)((wcslen(g_wszFriendlyName) + 1) * sizeof(wchar_t)));
        
        HKEY hInprocKey;
        if (RegCreateKeyExW(hKey, L"InprocServer32", 0, NULL, 0, KEY_WRITE, NULL, &hInprocKey, NULL) == ERROR_SUCCESS) {
            RegSetValueExW(hInprocKey, NULL, 0, REG_SZ, (const BYTE*)szModulePath, (DWORD)((wcslen(szModulePath) + 1) * sizeof(wchar_t)));
            const wchar_t* szThreading = L"Both";
            RegSetValueExW(hInprocKey, L"ThreadingModel", 0, REG_SZ, (const BYTE*)szThreading, (DWORD)((wcslen(szThreading) + 1) * sizeof(wchar_t)));
            RegCloseKey(hInprocKey);
        }
        RegCloseKey(hKey);
    }

    return S_OK;
}

STDAPI DllUnregisterServer(void) {
    wchar_t szKey[256];
    wsprintfW(szKey, L"CLSID\\%s\\InprocServer32", g_wszClsid);
    RegDeleteKeyW(HKEY_CLASSES_ROOT, szKey);

    wsprintfW(szKey, L"CLSID\\%s", g_wszClsid);
    RegDeleteKeyW(HKEY_CLASSES_ROOT, szKey);
    return S_OK;
}
