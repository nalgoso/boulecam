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
    uint32_t width = 1920;
    uint32_t height = 1080;
    bool isVertical = false;
    bool isDimmed = false;
    float fps = 0.0f;
    float latencyMs = 0.0f;
    uint32_t bitrateKbps = 0;
};

struct SystemStatus {
    bool connected = false;
    bool usbConnected = false;
    int activeDeviceId = 1;
    std::string deviceName = "Desconectado";
    uint32_t width = 1920;
    uint32_t height = 1080;
    bool isVertical = false;
    bool isDimmed = false;
    float fps = 0.0f;
    float latencyMs = 0.0f;
    uint32_t bitrateKbps = 0;
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
    void SetDeviceMetadata(int deviceId, const std::string& name, uint32_t width, uint32_t height);
    void SetDeviceDimState(int deviceId, bool isDimmed);
    void RemoveDevice(int deviceId);
    void UpdateDecodedFrame(int deviceId, const uint8_t* pDecodedData, uint32_t dataSize, uint32_t width, uint32_t height, BouleCamPixelFormat pixelFormat, uint16_t rotation = 0);

    struct DeviceTransform {
        bool isMirrored = false;
        int rotation = 0;
    };

    void SetDeviceTransform(int deviceId, bool isMirrored, int rotation);
    DeviceTransform GetDeviceTransform(int deviceId);

    void PushAudioData(int deviceId, const uint8_t* pcmData, uint32_t size);

    int GetActiveDeviceId() const { return m_activeDeviceId.load(); }
    void SetActiveDeviceId(int id) { m_activeDeviceId.store(id); }

private:
    void ServerWorker();
    void HandleClient(SOCKET clientSock);

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
