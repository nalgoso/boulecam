#include "audio_output_manager.h"
#include <iostream>
#include <cstring>
#include <setupapi.h>
#include <devguid.h>
#include <initguid.h>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "advapi32.lib")

// KSCATEGORY_CAPTURE: {65E8773D-8F56-11D0-A3B9-00A0C9223196}
static const GUID GUID_KSCAT_CAPTURE = 
    { 0x65E8773D, 0x8F56, 0x11D0, { 0xA3, 0xB9, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96 } };

#define IOCTL_BOULECAM_AUDIO_INIT  0x00222008
#define IOCTL_BOULECAM_AUDIO_WRITE 0x00222010

namespace boulecam {

AudioOutputManager::AudioOutputManager() {
    m_buffers.resize(NUM_BUFFERS);
    for (auto& b : m_buffers) {
        b.data.resize(4096);
        std::memset(&b.header, 0, sizeof(WAVEHDR));
        b.inUse = false;
    }
}

AudioOutputManager::~AudioOutputManager() {
    Shutdown();
}

void AudioOutputManager::EnsureAudioEndpointActive() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD subKeyCount = 0;
        if (RegQueryInfoKeyW(hKey, nullptr, nullptr, nullptr, &subKeyCount, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
            for (DWORD i = 0; i < subKeyCount; ++i) {
                wchar_t subKeyName[256];
                DWORD nameLen = 256;
                if (RegEnumKeyExW(hKey, i, subKeyName, &nameLen, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
                    std::wstring propPath = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture\\") + subKeyName + L"\\Properties";
                    HKEY hPropKey = nullptr;
                    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, propPath.c_str(), 0, KEY_READ | KEY_SET_VALUE, &hPropKey) == ERROR_SUCCESS) {
                        wchar_t devDesc[256] = {0};
                        DWORD descSize = sizeof(devDesc);
                        DWORD type = 0;
                        bool isTarget = false;
                        if (RegQueryValueExW(hPropKey, L"{b3f8fa53-0004-438e-9003-51a46e139bfc},6", nullptr, &type, reinterpret_cast<LPBYTE>(devDesc), &descSize) == ERROR_SUCCESS) {
                            if (wcsstr(devDesc, L"Iriun") || wcsstr(devDesc, L"BouleCam")) {
                                isTarget = true;
                            }
                        }
                        if (!isTarget) {
                            wchar_t hwId[256] = {0};
                            DWORD hwIdSize = sizeof(hwId);
                            if (RegQueryValueExW(hPropKey, L"{a8b865dd-2e3d-4094-ad97-e593a70c75d6},8", nullptr, &type, reinterpret_cast<LPBYTE>(hwId), &hwIdSize) == ERROR_SUCCESS) {
                                if (wcsstr(hwId, L"IriunA0") || wcsstr(hwId, L"BouleCam")) {
                                    isTarget = true;
                                }
                            }
                        }
                        if (isTarget) {
                            wchar_t endpointName[256] = {0};
                            DWORD epNameSize = sizeof(endpointName);
                            RegQueryValueExW(hPropKey, L"{a45c254e-df1c-4efd-8020-67d146a850e0},2", nullptr, &type, reinterpret_cast<LPBYTE>(endpointName), &epNameSize);
                            
                            DWORD formFactor = 0;
                            DWORD formFactorSize = sizeof(formFactor);
                            RegQueryValueExW(hPropKey, L"{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},0", nullptr, &type, reinterpret_cast<LPBYTE>(&formFactor), &formFactorSize);

                            bool isMidi = (wcsstr(endpointName, L"MIDI") != nullptr) || (formFactor == 2);

                            std::wstring devPath = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Capture\\") + subKeyName;
                            HKEY hDevKey = nullptr;
                            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, devPath.c_str(), 0, KEY_READ | KEY_SET_VALUE, &hDevKey) == ERROR_SUCCESS) {
                                if (isMidi) {
                                    // FORCE DISABLE MIDI endpoints to avoid clutter in OBS / Windows
                                    DWORD disabledState = 0x10000002;
                                    RegSetValueExW(hDevKey, L"DeviceState", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&disabledState), sizeof(disabledState));
                                } else {
                                    // MICROPHONE: Set Friendly Name and ACTIVATE
                                    const wchar_t* friendlyDesc = L"BouleCam Audio";
                                    RegSetValueExW(hPropKey, L"{b3f8fa53-0004-438e-9003-51a46e139bfc},6", 0, REG_SZ, 
                                        reinterpret_cast<const BYTE*>(friendlyDesc), static_cast<DWORD>((wcslen(friendlyDesc) + 1) * sizeof(wchar_t)));
                                    const wchar_t* fullFriendly = L"Micrófono (BouleCam Audio)";
                                    RegSetValueExW(hPropKey, L"{a45c254e-df1c-4efd-8020-67d146a850e0},14", 0, REG_SZ, 
                                        reinterpret_cast<const BYTE*>(fullFriendly), static_cast<DWORD>((wcslen(fullFriendly) + 1) * sizeof(wchar_t)));

                                    DWORD activeState = 1;
                                    RegSetValueExW(hDevKey, L"DeviceState", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&activeState), sizeof(activeState));
                                }
                                RegCloseKey(hDevKey);
                            }
                        }
                        RegCloseKey(hPropKey);
                    }
                }
            }
        }
        RegCloseKey(hKey);
    }
}

bool AudioOutputManager::InitializeWdmAudio() {
    EnsureAudioEndpointActive();

    // Check direct known KS wave path
    std::wstring directPath = L"\\\\?\\ROOT#MEDIA#0000#{65e8773d-8f56-11d0-a3b9-00a0c9223196}\\Wave";
    m_hWdmDevice = CreateFileW(
        directPath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr
    );

    // If direct open failed, try enumerating via SetupDi
    if (m_hWdmDevice == INVALID_HANDLE_VALUE) {
        HDEVINFO hDevInfo = SetupDiGetClassDevsW(
            &GUID_KSCAT_CAPTURE,
            nullptr,
            nullptr,
            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
        );
        if (hDevInfo != INVALID_HANDLE_VALUE) {
            SP_DEVICE_INTERFACE_DATA ifData{};
            ifData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
            for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(hDevInfo, nullptr, &GUID_KSCAT_CAPTURE, idx, &ifData); ++idx) {
                DWORD reqSize = 0;
                SP_DEVINFO_DATA devInfoData{};
                devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
                SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, nullptr, 0, &reqSize, &devInfoData);
                if (reqSize > 0) {
                    std::vector<uint8_t> buf(reqSize);
                    PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
                    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
                    if (SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, detail, reqSize, nullptr, &devInfoData)) {
                        std::wstring p = detail->DevicePath;
                        std::wstring lowerP = p;
                        std::transform(lowerP.begin(), lowerP.end(), lowerP.begin(), ::tolower);
                        if (lowerP.find(L"root#media") != std::wstring::npos || lowerP.find(L"iriuna0") != std::wstring::npos || lowerP.find(L"boulecam") != std::wstring::npos) {
                            if (lowerP.find(L"\\wave") == std::wstring::npos) {
                                p += L"\\Wave";
                            }
                            m_hWdmDevice = CreateFileW(
                                p.c_str(),
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr,
                                OPEN_EXISTING,
                                0,
                                nullptr
                            );
                            if (m_hWdmDevice != INVALID_HANDLE_VALUE) {
                                break;
                            }
                        }
                    }
                }
            }
            SetupDiDestroyDeviceInfoList(hDevInfo);
        }
    }

    if (m_hWdmDevice != INVALID_HANDLE_VALUE) {
        DWORD inBuf[2] = {100, 0};
        uint8_t outBuf[32] = {0};
        DWORD bytesReturned = 0;
        BOOL ioRes = DeviceIoControl(m_hWdmDevice, IOCTL_BOULECAM_AUDIO_INIT, inBuf, sizeof(inBuf), outBuf, sizeof(outBuf), &bytesReturned, nullptr);
        m_isWdmActive = true;
        m_activeDeviceName = "Micrófono (BouleCam Audio)";
        std::cout << "[AudioOutput] Driver nativo WDM PortCls conectado exitosamente: " << m_activeDeviceName
                  << " (IOCTL INIT: " << (ioRes ? "OK" : "Ignorado") << ")" << std::endl;
        return true;
    }

    return false;
}

void AudioOutputManager::ShutdownWdmAudio() {
    if (m_hWdmDevice != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hWdmDevice);
        m_hWdmDevice = INVALID_HANDLE_VALUE;
    }
    m_isWdmActive = false;
}

std::vector<std::pair<int, std::string>> AudioOutputManager::EnumerateOutputDevices() {
    std::vector<std::pair<int, std::string>> devices;
    UINT numDevs = waveOutGetNumDevs();
    for (UINT i = 0; i < numDevs; ++i) {
        WAVEOUTCAPSW caps{};
        if (waveOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            char nameBuf[256];
            WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, nameBuf, sizeof(nameBuf), NULL, NULL);
            devices.push_back({static_cast<int>(i), std::string(nameBuf)});
        }
    }
    return devices;
}

bool AudioOutputManager::Initialize(uint32_t sampleRate, uint16_t channels, uint16_t bitsPerSample) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_isInitialized.load()) {
        Shutdown();
    }

    std::memset(&m_wfx, 0, sizeof(WAVEFORMATEX));
    m_wfx.wFormatTag = WAVE_FORMAT_PCM;
    m_wfx.nChannels = channels;
    m_wfx.nSamplesPerSec = sampleRate;
    m_wfx.wBitsPerSample = bitsPerSample;
    m_wfx.nBlockAlign = (m_wfx.nChannels * m_wfx.wBitsPerSample) / 8;
    m_wfx.nAvgBytesPerSec = m_wfx.nSamplesPerSec * m_wfx.nBlockAlign;
    m_wfx.cbSize = 0;

    // 1. Try to connect directly to the native WDM Kernel Audio driver (Micrófono BouleCam Audio)
    if (InitializeWdmAudio()) {
        m_isCableDevice = false;
        m_isInitialized.store(true);
        std::cout << "[AudioOutput] Motor de audio nativo WDM activo: " << m_activeDeviceName
                  << " [" << sampleRate << " Hz, " << channels << "ch, " << bitsPerSample << "b] -> Directamente disponible en OBS Studio y aplicaciones de Windows." << std::endl;
        return true;
    }

    // Search for a Virtual Audio Cable device (VB-Audio Virtual Cable or similar)
    int targetDevice = -1;
    std::string foundName = "Aislado (Sin salida a altavoces)";
    bool isCable = false;

    auto devList = EnumerateOutputDevices();
    for (const auto& dev : devList) {
        std::string lowerName = dev.second;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
        if (lowerName.find("cable input") != std::string::npos || 
            lowerName.find("vb-audio") != std::string::npos ||
            lowerName.find("virtual audio") != std::string::npos) {
            targetDevice = dev.first;
            foundName = dev.second;
            isCable = true;
            break;
        }
    }

    if (isCable && targetDevice >= 0) {
        MMRESULT res = waveOutOpen(&m_hWaveOut, targetDevice, &m_wfx, 0, 0, CALLBACK_NULL);
        if (res == MMSYSERR_NOERROR) {
            m_deviceIndex = targetDevice;
            m_activeDeviceName = foundName;
            m_isCableDevice = true;
            m_isInitialized.store(true);

            std::cout << "[AudioOutput] Audio playback engine active: " << m_activeDeviceName
                      << " (Virtual Audio Cable - Native OBS Input ready!)"
                      << " [" << sampleRate << " Hz, " << channels << "ch, " << bitsPerSample << "b]" << std::endl;
            return true;
        } else {
            std::cerr << "[AudioOutput] Failed to open Virtual Cable device. Error code: " << res << std::endl;
        }
    }

    // DO NOT fallback to WAVE_MAPPER! Fallback to isolated mode (no physical speaker output)
    // to prevent echo and prevent bleeding into Windows Desktop Audio capture in OBS!
    m_hWaveOut = nullptr;
    m_deviceIndex = -1;
    m_activeDeviceName = "Aislado (DirectShow / VCam SHM activo, sin sangrado a altavoces)";
    m_isCableDevice = false;
    m_isInitialized.store(true);

    std::cout << "[AudioOutput] Modo Aislado activo (" << m_activeDeviceName << ").\n"
              << "[AudioOutput] El audio del microfono NO se reproducira en los altavoces de Windows para evitar eco y mezcla con audio de escritorio.\n"
              << "[AudioOutput] En OBS, captura el audio como 'BouleCam Audio' (DirectShow) o instala un Cable de Audio Virtual." << std::endl;

    return true;
}

bool AudioOutputManager::SelectDevice(int deviceIndex) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_isInitialized.load()) {
        if (m_hWaveOut) {
            waveOutReset(m_hWaveOut);
            CleanCompletedHeaders();
            waveOutClose(m_hWaveOut);
            m_hWaveOut = nullptr;
        }
    }

    std::string foundName = "Default Windows Playback";
    bool isCable = false;

    if (deviceIndex >= 0) {
        WAVEOUTCAPSW caps{};
        if (waveOutGetDevCapsW(deviceIndex, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            char nameBuf[256];
            WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, nameBuf, sizeof(nameBuf), NULL, NULL);
            foundName = std::string(nameBuf);
            std::string lowerName = foundName;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
            if (lowerName.find("cable") != std::string::npos || lowerName.find("vb-audio") != std::string::npos) {
                isCable = true;
            }
        }
    }

    MMRESULT res = waveOutOpen(&m_hWaveOut, deviceIndex, &m_wfx, 0, 0, CALLBACK_NULL);
    if (res != MMSYSERR_NOERROR) {
        std::cerr << "[AudioOutput] Failed to switch to device " << deviceIndex << std::endl;
        return false;
    }

    m_deviceIndex = deviceIndex;
    m_activeDeviceName = foundName;
    m_isCableDevice = isCable;
    m_isInitialized.store(true);
    return true;
}

void AudioOutputManager::CleanCompletedHeaders() {
    if (!m_hWaveOut) return;
    for (auto& b : m_buffers) {
        if (b.inUse && (b.header.dwFlags & WHDR_DONE)) {
            waveOutUnprepareHeader(m_hWaveOut, &b.header, sizeof(WAVEHDR));
            b.inUse = false;
        }
    }
}

void AudioOutputManager::WriteAudio(const uint8_t* pcmData, uint32_t size) {
    if (!m_isInitialized.load() || !pcmData || size == 0) return;

    // Calculate real-time level (Peak RMS)
    const int16_t* samples = reinterpret_cast<const int16_t*>(pcmData);
    size_t sampleCount = size / sizeof(int16_t);
    int16_t maxSample = 0;
    for (size_t i = 0; i < sampleCount; ++i) {
        int16_t val = std::abs(samples[i]);
        if (val > maxSample) maxSample = val;
    }

    float targetLevel = static_cast<float>(maxSample) / 32768.0f;
    float current = m_currentLevel.load();
    if (targetLevel > current) {
        m_currentLevel.store(targetLevel); // Fast attack
    } else {
        m_currentLevel.store(current * 0.85f + targetLevel * 0.15f); // Smooth decay
    }

    if (m_isMuted.load()) return;

    float vol = m_volume.load();

    // 1. Send to Native WDM Kernel Audio Driver if active
    if (m_isWdmActive && m_hWdmDevice != INVALID_HANDLE_VALUE) {
        DWORD bytesRet = 0;
        if (vol >= 0.99f) {
            DeviceIoControl(
                m_hWdmDevice,
                IOCTL_BOULECAM_AUDIO_WRITE,
                const_cast<uint8_t*>(pcmData),
                size,
                nullptr,
                0,
                &bytesRet,
                nullptr
            );
        } else {
            std::vector<int16_t> adjusted(sampleCount);
            for (size_t i = 0; i < sampleCount; ++i) {
                adjusted[i] = static_cast<int16_t>(samples[i] * vol);
            }
            DeviceIoControl(
                m_hWdmDevice,
                IOCTL_BOULECAM_AUDIO_WRITE,
                adjusted.data(),
                size,
                nullptr,
                0,
                &bytesRet,
                nullptr
            );
        }
    }

    // 2. Also send to waveOut if active
    if (!m_hWaveOut) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    CleanCompletedHeaders();

    // Find next available buffer
    AudioBuffer* targetBuf = nullptr;
    for (size_t i = 0; i < NUM_BUFFERS; ++i) {
        size_t idx = (m_currentBufferIndex + i) % NUM_BUFFERS;
        if (!m_buffers[idx].inUse) {
            targetBuf = &m_buffers[idx];
            m_currentBufferIndex = (idx + 1) % NUM_BUFFERS;
            break;
        }
    }

    if (!targetBuf) {
        // All buffers busy, force recycle oldest to prevent audio drift
        targetBuf = &m_buffers[m_currentBufferIndex];
        if (targetBuf->inUse) {
            waveOutUnprepareHeader(m_hWaveOut, &targetBuf->header, sizeof(WAVEHDR));
            targetBuf->inUse = false;
        }
        m_currentBufferIndex = (m_currentBufferIndex + 1) % NUM_BUFFERS;
    }

    if (targetBuf->data.size() < size) {
        targetBuf->data.resize(size);
    }

    if (vol >= 0.99f) {
        std::memcpy(targetBuf->data.data(), pcmData, size);
    } else {
        int16_t* dstSamples = reinterpret_cast<int16_t*>(targetBuf->data.data());
        for (size_t i = 0; i < sampleCount; ++i) {
            dstSamples[i] = static_cast<int16_t>(samples[i] * vol);
        }
    }

    std::memset(&targetBuf->header, 0, sizeof(WAVEHDR));
    targetBuf->header.lpData = reinterpret_cast<LPSTR>(targetBuf->data.data());
    targetBuf->header.dwBufferLength = size;

    MMRESULT prepRes = waveOutPrepareHeader(m_hWaveOut, &targetBuf->header, sizeof(WAVEHDR));
    if (prepRes == MMSYSERR_NOERROR) {
        MMRESULT writeRes = waveOutWrite(m_hWaveOut, &targetBuf->header, sizeof(WAVEHDR));
        if (writeRes == MMSYSERR_NOERROR) {
            targetBuf->inUse = true;
        } else {
            waveOutUnprepareHeader(m_hWaveOut, &targetBuf->header, sizeof(WAVEHDR));
        }
    }
}

void AudioOutputManager::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_isInitialized.load()) return;
    m_isInitialized.store(false);

    ShutdownWdmAudio();

    if (m_hWaveOut) {
        waveOutReset(m_hWaveOut);
        for (auto& b : m_buffers) {
            if (b.inUse) {
                waveOutUnprepareHeader(m_hWaveOut, &b.header, sizeof(WAVEHDR));
                b.inUse = false;
            }
        }
        waveOutClose(m_hWaveOut);
        m_hWaveOut = nullptr;
    }
    m_currentLevel.store(0.0f);
}

} // namespace boulecam
