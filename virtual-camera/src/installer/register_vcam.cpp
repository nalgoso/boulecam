#include <windows.h>
#include <iostream>
#include <string>

typedef HRESULT (STDAPICALLTYPE *pfnDllRegisterServer)(void);
typedef HRESULT (STDAPICALLTYPE *pfnDllUnregisterServer)(void);

void PrintUsage() {
    std::cout << "BouleCam Virtual Camera Registration Utility" << std::endl;
    std::cout << "Usage:" << std::endl;
    std::cout << "  register_vcam.exe --install    Register DirectShow virtual camera & audio in Windows" << std::endl;
    std::cout << "  register_vcam.exe --uninstall  Remove DirectShow virtual camera & audio from Windows" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    std::string action = argv[1];

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';

    std::wstring dllPath = std::wstring(exePath) + L"boulecam-vcam.dll";
    HMODULE hDll = LoadLibraryW(dllPath.c_str());
    if (!hDll) {
        hDll = LoadLibraryW(L"boulecam-vcam.dll");
    }

    if (!hDll) {
        std::wcerr << L"[Error] Could not load boulecam-vcam.dll from " << dllPath << std::endl;
        return 2;
    }

    if (action == "--install" || action == "-i") {
        pfnDllRegisterServer pRegister = (pfnDllRegisterServer)GetProcAddress(hDll, "DllRegisterServer");
        if (!pRegister) {
            std::cerr << "[Error] DllRegisterServer entry point not found." << std::endl;
            FreeLibrary(hDll);
            return 3;
        }

        HRESULT hr = pRegister();
        if (SUCCEEDED(hr)) {
            std::cout << "[Success] BouleCam Virtual Camera & Audio successfully registered in DirectShow!" << std::endl;
            FreeLibrary(hDll);
            return 0;
        } else {
            std::cerr << "[Error] DllRegisterServer failed with code: 0x" << std::hex << hr << std::endl;
            FreeLibrary(hDll);
            return 4;
        }
    } else if (action == "--uninstall" || action == "-u") {
        pfnDllUnregisterServer pUnregister = (pfnDllUnregisterServer)GetProcAddress(hDll, "DllUnregisterServer");
        if (pUnregister) {
            pUnregister();
            std::cout << "[Success] BouleCam Virtual Camera & Audio unregistered." << std::endl;
        }
        FreeLibrary(hDll);
        return 0;
    }

    PrintUsage();
    FreeLibrary(hDll);
    return 1;
}
