#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <functional>
#include <thread>
#include <atomic>
#include <vector>
#include <map>
#include <mutex>
#include <string>
#include <cstdint>
#include "boulecam_protocol.h"

#include <set>
#include <memory>

namespace boulecam {

// Callback function type: invoked when a complete H.264/H.265 frame packet is assembled
using FrameReceivedCallback = std::function<void(
    int deviceId,
    const BouleCamFrameHeader& header,
    const uint8_t* payloadData,
    uint32_t payloadSize
)>;

using HandshakeReceivedCallback = std::function<void(
    int deviceId,
    const BouleCamHandshakeReq& handshake
)>;

using CameraStateReceivedCallback = std::function<void(
    int deviceId,
    const BouleCamCameraState& state
)>;

using AudioReceivedCallback = std::function<void(
    int deviceId,
    const BouleCamAudioHeader& header,
    const uint8_t* payloadData,
    uint32_t payloadSize
)>;

struct ConnectedClient {
    int id;
    std::string deviceName;
    SOCKET socket;
    std::string ip;
    uint16_t port;
    std::shared_ptr<std::atomic<int>> deviceIdRef;
};

class TcpReceiver {
public:
    TcpReceiver();
    ~TcpReceiver();

    bool Start(uint16_t port, 
               FrameReceivedCallback frameCb,
               HandshakeReceivedCallback handshakeCb = nullptr,
               CameraStateReceivedCallback stateCb = nullptr);
    void Stop();

    bool IsConnected() const { return m_activeClientCount.load() > 0; }
    int GetClientCount() const { return m_activeClientCount.load(); }
    uint16_t GetPort() const { return m_port; }

    // Send remote camera control command to connected mobile device (deviceId 0 = all/active)
    bool SendCameraCommand(const BouleCamCameraCmd& cmd, int deviceId = 0);

    // Disconnect and unlink a specific device
    bool DisconnectClient(int deviceId, bool blockFutureReconnect = true);
    void ClearIgnoredClients();

    // Reorder / swap camera channels (e.g. Cam 1 <-> Cam 2)
    bool SwapClientIds(int camA, int camB);
    bool ReassignClientId(int fromId, int toId);

    void SetAudioCallback(AudioReceivedCallback audioCb) { m_audioCallback = audioCb; }

    std::vector<ConnectedClient> GetConnectedClients();
    double GetClientRttMs(int deviceId);
    std::string GetClientIp(int deviceId);

private:
    void ListenThreadWorker();
    void ClientThreadWorker(SOCKET clientSocket, std::string clientIp, uint16_t clientPort);

    bool ReceiveExact(SOCKET sock, uint8_t* destination, size_t bytesToRead);

    uint16_t m_port;
    SOCKET m_listenSocket;
    std::atomic<bool> m_isRunning;
    std::atomic<int> m_activeClientCount;

    std::mutex m_clientsMutex;
    std::map<int, ConnectedClient> m_clients;
    std::set<std::string> m_ignoredClients;

    std::thread m_listenThread;
    FrameReceivedCallback m_frameCallback;
    HandshakeReceivedCallback m_handshakeCallback;
    CameraStateReceivedCallback m_cameraStateCallback;
    AudioReceivedCallback m_audioCallback;
};

} // namespace boulecam

