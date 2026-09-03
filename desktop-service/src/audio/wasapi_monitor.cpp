#include "wasapi_monitor.h"
#include <functiondiscoverykeys_devpkey.h>
#include <iostream>
#include <algorithm>

namespace boulecam {

WasapiMonitor::WasapiMonitor() {
    m_ringBuffer.resize(RING_BUFFER_SIZE, 0);
}

WasapiMonitor::~WasapiMonitor() {
    Stop();
}

bool WasapiMonitor::Start(const std::wstring& deviceId) {
    if (m_isRunning.load()) return true;

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&m_pEnumerator
    );

    if (FAILED(hr) || !m_pEnumerator) {
        std::cerr << "[WASAPI Monitor] Failed to create MMDeviceEnumerator. hr=0x" << std::hex << hr << std::endl;
        return false;
    }

    if (deviceId.empty()) {
        hr = m_pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_pDevice);
    } else {
        hr = m_pEnumerator->GetDevice(deviceId.c_str(), &m_pDevice);
    }

    if (FAILED(hr) || !m_pDevice) {
        std::cerr << "[WASAPI Monitor] Failed to get audio endpoint device. hr=0x" << std::hex << hr << std::endl;
        return false;
    }

    hr = m_pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&m_pAudioClient);
    if (FAILED(hr) || !m_pAudioClient) {
        std::cerr << "[WASAPI Monitor] Failed to activate IAudioClient. hr=0x" << std::hex << hr << std::endl;
        return false;
    }

    hr = m_pAudioClient->GetMixFormat(&m_pMixFormat);
    if (FAILED(hr) || !m_pMixFormat) {
        std::cerr << "[WASAPI Monitor] Failed to get mix format. hr=0x" << std::hex << hr << std::endl;
        return false;
    }

    // Initialize in shared mode with 30ms buffer (~300000 reference-time units)
    REFERENCE_TIME hnsBufferDuration = 300000;
    hr = m_pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        0,
        hnsBufferDuration,
        0,
        m_pMixFormat,
        NULL
    );

    if (FAILED(hr)) {
        std::cerr << "[WASAPI Monitor] AudioClient Initialize failed. hr=0x" << std::hex << hr << std::endl;
        return false;
    }

    hr = m_pAudioClient->GetService(__uuidof(IAudioRenderClient), (void**)&m_pRenderClient);
    if (FAILED(hr) || !m_pRenderClient) {
        std::cerr << "[WASAPI Monitor] Failed to get IAudioRenderClient. hr=0x" << std::hex << hr << std::endl;
        return false;
    }

    hr = m_pAudioClient->Start();
    if (FAILED(hr)) {
        std::cerr << "[WASAPI Monitor] AudioClient Start failed. hr=0x" << std::hex << hr << std::endl;
        return false;
    }

    m_isRunning.store(true);
    m_playoutThread = std::thread(&WasapiMonitor::PlayoutWorker, this);

    std::cout << "[WASAPI Monitor] Audio monitoring started on default render endpoint ("
              << m_pMixFormat->nSamplesPerSec << " Hz, " << m_pMixFormat->nChannels << " channels)" << std::endl;
    return true;
}

void WasapiMonitor::Stop() {
    if (!m_isRunning.load()) return;

    m_isRunning.store(false);
    if (m_playoutThread.joinable()) {
        m_playoutThread.join();
    }

    if (m_pAudioClient) {
        m_pAudioClient->Stop();
        m_pAudioClient->Release();
        m_pAudioClient = nullptr;
    }

    if (m_pRenderClient) {
        m_pRenderClient->Release();
        m_pRenderClient = nullptr;
    }

    if (m_pMixFormat) {
        CoTaskMemFree(m_pMixFormat);
        m_pMixFormat = nullptr;
    }

    if (m_pDevice) {
        m_pDevice->Release();
        m_pDevice = nullptr;
    }

    if (m_pEnumerator) {
        m_pEnumerator->Release();
        m_pEnumerator = nullptr;
    }

    m_availableSamples.store(0);
    m_writeIndex.store(0);
    m_readIndex.store(0);
}

void WasapiMonitor::SetVolume(float vol) {
    m_volume.store(std::max(0.0f, std::min(1.0f, vol)));
}

void WasapiMonitor::PushPcm(const uint8_t* pcm16, uint32_t bytes, int channels) {
    if (!m_isRunning.load() || !pcm16 || bytes < 2) return;

    std::lock_guard<std::mutex> lock(m_pushMutex);
    int numSamples = bytes / 2;
    const int16_t* samples = reinterpret_cast<const int16_t*>(pcm16);

    size_t wIdx = m_writeIndex.load();
    for (int i = 0; i < numSamples; ++i) {
        int16_t s = samples[i];
        m_ringBuffer[wIdx] = s;
        wIdx = (wIdx + 1) % RING_BUFFER_SIZE;
        // If mono input, duplicate to stereo slot so headphones get stereo sound
        if (channels == 1) {
            m_ringBuffer[wIdx] = s;
            wIdx = (wIdx + 1) % RING_BUFFER_SIZE;
        }
    }
    m_writeIndex.store(wIdx);

    size_t added = (channels == 1) ? (numSamples * 2) : numSamples;
    size_t current = m_availableSamples.load();
    m_availableSamples.store(std::min(RING_BUFFER_SIZE, current + added));
}

void WasapiMonitor::PlayoutWorker() {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    UINT32 bufferFrameCount = 0;
    if (FAILED(m_pAudioClient->GetBufferSize(&bufferFrameCount))) {
        return;
    }

    const int targetChannels = m_pMixFormat->nChannels;
    const bool isFloat = (m_pMixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (m_pMixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         reinterpret_cast<WAVEFORMATEXTENSIBLE*>(m_pMixFormat)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));

    while (m_isRunning.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        UINT32 numPadding = 0;
        if (FAILED(m_pAudioClient->GetCurrentPadding(&numPadding))) {
            continue;
        }

        UINT32 numFramesAvailable = bufferFrameCount - numPadding;
        if (numFramesAvailable == 0) continue;

        BYTE* pData = nullptr;
        if (FAILED(m_pRenderClient->GetBuffer(numFramesAvailable, &pData)) || !pData) {
            continue;
        }

        float vol = m_isMuted.load() ? 0.0f : m_volume.load();
        size_t available = m_availableSamples.load();
        size_t rIdx = m_readIndex.load();

        if (isFloat) {
            float* fOut = reinterpret_cast<float*>(pData);
            for (UINT32 f = 0; f < numFramesAvailable; ++f) {
                float sL = 0.0f, sR = 0.0f;
                if (available >= 2) {
                    sL = (static_cast<float>(m_ringBuffer[rIdx]) / 32768.0f) * vol;
                    rIdx = (rIdx + 1) % RING_BUFFER_SIZE;
                    sR = (static_cast<float>(m_ringBuffer[rIdx]) / 32768.0f) * vol;
                    rIdx = (rIdx + 1) % RING_BUFFER_SIZE;
                    available -= 2;
                }
                fOut[f * targetChannels] = sL;
                if (targetChannels > 1) fOut[f * targetChannels + 1] = sR;
            }
        } else {
            int16_t* sOut = reinterpret_cast<int16_t*>(pData);
            for (UINT32 f = 0; f < numFramesAvailable; ++f) {
                int16_t sL = 0, sR = 0;
                if (available >= 2) {
                    sL = static_cast<int16_t>(m_ringBuffer[rIdx] * vol);
                    rIdx = (rIdx + 1) % RING_BUFFER_SIZE;
                    sR = static_cast<int16_t>(m_ringBuffer[rIdx] * vol);
                    rIdx = (rIdx + 1) % RING_BUFFER_SIZE;
                    available -= 2;
                }
                sOut[f * targetChannels] = sL;
                if (targetChannels > 1) sOut[f * targetChannels + 1] = sR;
            }
        }

        m_readIndex.store(rIdx);
        m_availableSamples.store(available);

        m_pRenderClient->ReleaseBuffer(numFramesAvailable, 0);
    }

    CoUninitialize();
}

std::vector<WasapiMonitor::AudioOutputDevice> WasapiMonitor::EnumerateOutputDevices() {
    std::vector<AudioOutputDevice> list;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* pEnum = nullptr;
    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&pEnum
    );

    if (SUCCEEDED(hr) && pEnum) {
        IMMDeviceCollection* pCol = nullptr;
        hr = pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCol);
        if (SUCCEEDED(hr) && pCol) {
            UINT count = 0;
            pCol->GetCount(&count);

            IMMDevice* pDef = nullptr;
            pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDef);
            LPWSTR defId = nullptr;
            if (pDef) pDef->GetId(&defId);

            for (UINT i = 0; i < count; ++i) {
                IMMDevice* pDev = nullptr;
                if (SUCCEEDED(pCol->Item(i, &pDev)) && pDev) {
                    LPWSTR idStr = nullptr;
                    pDev->GetId(&idStr);

                    IPropertyStore* pProps = nullptr;
                    pDev->OpenPropertyStore(STGM_READ, &pProps);
                    std::string friendlyName = "Altavoces / Auriculares";

                    if (pProps) {
                        PROPVARIANT var;
                        PropVariantInit(&var);
                        if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &var)) && var.pwszVal) {
                            int len = WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, NULL, 0, NULL, NULL);
                            if (len > 0) {
                                std::string s(len - 1, '\0');
                                WideCharToMultiByte(CP_UTF8, 0, var.pwszVal, -1, &s[0], len, NULL, NULL);
                                friendlyName = s;
                            }
                        }
                        PropVariantClear(&var);
                        pProps->Release();
                    }

                    std::string idUtf8 = "";
                    if (idStr) {
                        int len = WideCharToMultiByte(CP_UTF8, 0, idStr, -1, NULL, 0, NULL, NULL);
                        if (len > 0) {
                            std::string s(len - 1, '\0');
                            WideCharToMultiByte(CP_UTF8, 0, idStr, -1, &s[0], len, NULL, NULL);
                            idUtf8 = s;
                        }
                    }

                    bool isDefault = (defId && idStr && wcscmp(defId, idStr) == 0);
                    list.push_back({ idUtf8, friendlyName, isDefault });

                    if (idStr) CoTaskMemFree(idStr);
                    pDev->Release();
                }
            }

            if (defId) CoTaskMemFree(defId);
            if (pDef) pDef->Release();
            pCol->Release();
        }
        pEnum->Release();
    }

    return list;
}

} // namespace boulecam
