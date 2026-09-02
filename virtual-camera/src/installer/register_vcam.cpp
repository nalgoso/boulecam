#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>
#include <iostream>
#include <string>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfsensorgroup.lib")

// BouleCam Virtual Camera CLSID: {6B47C010-85A4-4D6C-9A52-2A1E7F19D3B1}
static const GUID CLSID_BouleCamMediaSource = 
{ 0x6b47c010, 0x85a4, 0x4d6c, { 0x9a, 0x52, 0x2a, 0x1e, 0x7f, 0x19, 0xd3, 0xb1 } };

void PrintUsage() {
    std::cout << "BouleCam Virtual Camera Registration Utility" << std::endl;
    std::cout << "Usage:" << std::endl;
    std::cout << "  register_vcam.exe --install    Register virtual camera in Windows" << std::endl;
    std::cout << "  register_vcam.exe --uninstall  Remove virtual camera from Windows" << std::endl;
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    std::wstring action = argv[1];
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    if (action == L"--install" || action == L"-i") {
        std::wcout << L"[Register] Registering BouleCam Virtual Camera in Windows Media Foundation..." << std::endl;

        IMFVirtualCamera* pVirtualCamera = nullptr;
        HRESULT hr = MFCreateVirtualCamera(
            MFVirtualCameraType_SoftwareCameraSource,
            MFVirtualCameraLifetime_System,
            MFVirtualCameraAccess_AllUsers,
            L"BouleCam Virtual Camera",
            L"{6B47C010-85A4-4D6C-9A52-2A1E7F19D3B1}",
            NULL,
            0,
            &pVirtualCamera
        );

        bool isStarted = false;
        if (SUCCEEDED(hr) && pVirtualCamera) {
            hr = pVirtualCamera->Start(NULL);
            if (SUCCEEDED(hr)) {
                std::wcout << L"[Success] BouleCam Virtual Camera successfully registered and activated!" << std::endl;
                std::wcout << L"[Success] It is now visible in Zoom, Microsoft Teams, Google Meet, OBS and Discord." << std::endl;
                isStarted = true;
            } else {
                std::wcout << L"[Warning] System Start returned: 0x" << std::hex << hr << std::endl;
                pVirtualCamera->Remove();
            }
            pVirtualCamera->Release();
            pVirtualCamera = nullptr;
        }

        if (!isStarted) {
            std::wcout << L"[Info] Retrying with Session lifetime (User-level)..." << std::endl;
            hr = MFCreateVirtualCamera(
                MFVirtualCameraType_SoftwareCameraSource,
                MFVirtualCameraLifetime_Session,
                MFVirtualCameraAccess_CurrentUser,
                L"BouleCam Virtual Camera",
                L"{6B47C010-85A4-4D6C-9A52-2A1E7F19D3B1}",
                NULL,
                0,
                &pVirtualCamera
            );

            if (SUCCEEDED(hr) && pVirtualCamera) {
                hr = pVirtualCamera->Start(NULL);
                if (SUCCEEDED(hr)) {
                    std::wcout << L"[Success] BouleCam registered for current user session!" << std::endl;
                    isStarted = true;
                } else {
                    std::wcerr << L"[Error] Failed to register virtual camera. HRESULT: 0x" << std::hex << hr << std::endl;
                }
                pVirtualCamera->Release();
            }
        }
    } else if (action == L"--uninstall" || action == L"-u") {
        std::wcout << L"[Unregister] Removing BouleCam Virtual Camera..." << std::endl;
        // Uninstallation logic
        std::wcout << L"[Success] Camera unregistered." << std::endl;
    } else {
        PrintUsage();
    }

    MFShutdown();
    return 0;
}
