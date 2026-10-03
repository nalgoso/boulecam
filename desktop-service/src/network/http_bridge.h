#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <vector>
#include <map>
#include <mutex>
#include "boulecam_protocol.h"
#include "boulecam_ipc.h"

namespace boulecam {

class TcpReceiver;

struct DeviceInfo {
    int id = 0;
    std::string name = "Móvil";
    std::string ip = "";
    std::string uniqueId = "";
    bool isUsb = false;
    bool isLocked = false;
    bool connected = true;
    uint32_t width = 0;
    uint32_t height = 0;
    bool isVertical = false;
    bool isDimmed = false;
    int batteryLevel = -1; // 0..100%, or -1 if unknown
    bool isCharging = false;
    float fps = 0.0f;
    float latencyMs = 0.0f;
    uint32_t bitrateKbps = 0;
    int audioLevel = 0;
    bool isMicMuted = false;
    float micGainDb = 0.0f; // Microphone gain in dB (-40..+20)
};

struct SystemStatus {
    bool connected = false;
    bool usbConnected = false;
    int activeDeviceId = 1;
    std::string deviceName = "Desconectado";
    std::string deviceIp = "";
    bool isUsb = false;
    uint32_t width = 0;
    uint32_t height = 0;
    bool isVertical = false;
    bool isDimmed = false;
    int batteryLevel = -1;
    bool isCharging = false;
    float fps = 0.0f;
    float latencyMs = 0.0f;
    uint32_t bitrateKbps = 0;
    int audioLevel = 0;
    std::vector<std::string> localIps;
};

class HttpControlBridge {
public:
    HttpControlBridge(TcpReceiver& receiver);
    ~HttpControlBridge();

    bool Start(uint16_t port = 8090);
    void Stop();

    void UpdateStats(int deviceId, float fps, float latencyMs, uint32_t bitrateKbps);
    void SetUsbStatus(bool connected);
    void SetDeviceMetadata(int deviceId, const std::string& name, uint32_t width, uint32_t height, const std::string& ip = "", bool isUsb = false, const std::string& uniqueId = "");
    void SetDeviceDimState(int deviceId, bool isDimmed);
    void SetDeviceBatteryState(int deviceId, float batteryLevel, bool isCharging);
    void RemoveDevice(int deviceId);
    bool DisconnectDevice(int deviceId);
    bool SwapDevices(int camA, int camB);
    bool ReassignDevice(int fromId, int toId);
    bool RenameDevice(int camId, const std::string& newName);
    bool LockDevice(int camId, bool lock, const std::string& uniqueId = "", const std::string& devName = "");

    using RescanCallback = std::function<void()>;
    void SetRescanCallback(RescanCallback cb) { m_rescanCallback = cb; }
    void TriggerRescan();

    // Callbacks invoked when the UI sends mute/gain commands so main.cpp can
    // propagate them to the correct ShmProducer instance.
    using MicMuteCallback = std::function<void(int deviceId, bool muted)>;
    using MicGainCallback = std::function<void(int deviceId, float gainDb)>;
    void SetMicMuteCallback(MicMuteCallback cb) { m_micMuteCallback = cb; }
    void SetMicGainCallback(MicGainCallback cb) { m_micGainCallback = cb; }

    void UpdateDecodedFrame(int deviceId, const uint8_t* pDecodedData, uint32_t dataSize, uint32_t width, uint32_t height, BouleCamPixelFormat pixelFormat, uint16_t rotation = 0);

    struct DeviceTransform {
        bool isMirrored = false;
        int rotation = 0;
    };

    void SetDeviceTransform(int deviceId, bool isMirrored, int rotation);
    DeviceTransform GetDeviceTransform(int deviceId);

    void PushAudioData(int deviceId, const uint8_t* pcmData, uint32_t size);
    void SetMicMute(int deviceId, bool muted);
    void SetMicGain(int deviceId, float gainDb); // -40..+20 dB

    void SetAudioOutputManager(class AudioOutputManager* pMgr) { m_pAudioOutput = pMgr; }
    class AudioOutputManager* GetAudioOutputManager() const { return m_pAudioOutput; }

    int GetActiveDeviceId() const { return m_activeDeviceId.load(); }
    void SetActiveDeviceId(int id) { m_activeDeviceId.store(id); }

    SystemStatus GetStatus() {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        return m_status;
    }

private:
    void ServerWorker();
    void HandleClient(SOCKET clientSock);

    RescanCallback m_rescanCallback;
    TcpReceiver& m_receiver;
    uint16_t m_port;
    SOCKET m_listenSocket;
    std::atomic<bool> m_isRunning;
    std::thread m_serverThread;

    std::atomic<int> m_activeDeviceId{1};

    std::mutex m_statusMutex;
    SystemStatus m_status;
    std::map<int, DeviceInfo> m_devices;

    std::mutex m_frameMutex;
    std::map<int, std::vector<uint8_t>> m_deviceBmpFrames;
    std::vector<uint8_t> m_latestBmpFrame;

    std::mutex m_transformMutex;
    std::map<int, DeviceTransform> m_deviceTransforms;

    std::mutex m_audioMutex;
    std::map<int, std::vector<uint8_t>> m_audioBuffers;

    class AudioOutputManager* m_pAudioOutput = nullptr;
    MicMuteCallback m_micMuteCallback;
    MicGainCallback m_micGainCallback;
};

} // namespace boulecam
