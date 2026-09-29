#include "audio_output_manager.h"
#include <iostream>
#include <cstring>

#pragma comment(lib, "winmm.lib")

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

    // Search for a Virtual Audio Cable device (VB-Audio Virtual Cable or similar)
    int targetDevice = WAVE_MAPPER;
    std::string foundName = "Default Windows Playback";
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

    MMRESULT res = waveOutOpen(&m_hWaveOut, targetDevice, &m_wfx, 0, 0, CALLBACK_NULL);
    if (res != MMSYSERR_NOERROR && targetDevice != WAVE_MAPPER) {
        // Fallback to default mapper if preferred device failed
        targetDevice = WAVE_MAPPER;
        foundName = "Default Windows Playback";
        isCable = false;
        res = waveOutOpen(&m_hWaveOut, targetDevice, &m_wfx, 0, 0, CALLBACK_NULL);
    }

    if (res != MMSYSERR_NOERROR) {
        std::cerr << "[AudioOutput] Failed to open waveOut device. Error code: " << res << std::endl;
        return false;
    }

    m_deviceIndex = targetDevice;
    m_activeDeviceName = foundName;
    m_isCableDevice = isCable;
    m_isInitialized.store(true);

    std::cout << "[AudioOutput] Audio playback engine active: " << m_activeDeviceName
              << (isCable ? " (Virtual Audio Cable - Native OBS Input ready!)" : " (Standard Output)")
              << " [" << sampleRate << " Hz, " << channels << "ch, " << bitsPerSample << "b]" << std::endl;

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
    if (!m_isInitialized.load() || !m_hWaveOut || !pcmData || size == 0) return;

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

    float vol = m_volume.load();
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
