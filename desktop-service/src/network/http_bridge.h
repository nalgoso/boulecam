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
    bool isUsb = false;
    uint32_t width = 0;
    uint32_t height = 0;
    bool isVertical = false;
    bool isDimmed = false;
    float fps = 0.0f;
    float latencyMs = 0.0f;
    uint32_t bitrateKbps = 0;
    float batteryLevel = -1.0f;
    float temperatureC = 0.0f;
    uint8_t lensesMask = 3;
    float zoomRatio = 1.0f;
    uint8_t currentLens = 0;
    uint8_t micChannels = 1;
    uint8_t micCapsule = 0;
    bool micBeamforming = false;
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
    float fps = 0.0f;
    float latencyMs = 0.0f;
    uint32_t bitrateKbps = 0;
    float batteryLevel = -1.0f;
    float temperatureC = 0.0f;
    uint8_t lensesMask = 3;
    float zoomRatio = 1.0f;
    uint8_t currentLens = 0;
    uint8_t micChannels = 1;
    uint8_t micCapsule = 0;
    bool micBeamforming = false;
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
    void SetDeviceMetadata(int deviceId, const std::string& name, uint32_t width, uint32_t height, const std::string& ip = "", bool isUsb = false);
    void SetDeviceDimState(int deviceId, bool isDimmed);
    void SetDeviceTelemetry(int deviceId, float batteryLevel, float temperatureC, uint8_t lensesMask, float currentZoom, uint8_t currentLens, uint8_t micChannels = 1, uint8_t micCapsule = 0, bool micBeamforming = false);
    void RemoveDevice(int deviceId);
    bool DisconnectDevice(int deviceId);
    bool SwapDevices(int camA, int camB);
    bool ReassignDevice(int fromId, int toId);
    bool RenameDevice(int camId, const std::string& newName);

    using RescanCallback = std::function<void()>;
    void SetRescanCallback(RescanCallback cb) { m_rescanCallback = cb; }
    void TriggerRescan();

    void UpdateDecodedFrame(int deviceId, const uint8_t* pDecodedData, uint32_t dataSize, uint32_t width, uint32_t height, BouleCamPixelFormat pixelFormat, uint16_t rotation = 0);

    struct DeviceTransform {
        bool isMirrored = false;
        int rotation = 0;
    };

    void SetDeviceTransform(int deviceId, bool isMirrored, int rotation);
    DeviceTransform GetDeviceTransform(int deviceId);

    void PushAudioData(int deviceId, const uint8_t* pcmData, uint32_t size);
    void UpdateAudioMeters(float prePeak, float preRms, float postPeak, float postRms);
    void SetAudioDspPipeline(class AudioDspPipeline* pDsp) { m_pDsp = pDsp; }
    void SetWasapiMonitor(class WasapiMonitor* pMonitor) { m_pMonitor = pMonitor; }

    int GetActiveDeviceId() const { return m_activeDeviceId.load(); }
    void SetActiveDeviceId(int id) { m_activeDeviceId.store(id); }

private:
    void ServerWorker();
    void HandleClient(SOCKET clientSock);

    class AudioDspPipeline* m_pDsp = nullptr;
    class WasapiMonitor* m_pMonitor = nullptr;
    float m_prePeakDb = -100.0f;
    float m_preRmsDb = -100.0f;
    float m_postPeakDb = -100.0f;
    float m_postRmsDb = -100.0f;
    std::mutex m_audioMeterMutex;

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
    std::map<int, std::vector<SOCKET>> m_audioClients;
};

} // namespace boulecam
