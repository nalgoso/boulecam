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

using ClientDisconnectedCallback = std::function<void(
    int deviceId
)>;

struct LockedCameraSlot {
    int camId = 0;
    std::string uniqueId = "";
    std::string deviceName = "";
    bool isLocked = false;
};

struct ConnectedClient {
    int id;
    std::string deviceName;
    std::string uniqueId;
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

    // Lock camera slot to a specific device unique ID (URL reservation)
    bool LockCameraSlot(int camId, bool lock, const std::string& uniqueId = "", const std::string& devName = "");
    bool IsCameraSlotLocked(int camId);
    std::vector<LockedCameraSlot> GetLockedSlots();
    LockedCameraSlot GetSlotLock(int camId);

    void SetAudioCallback(AudioReceivedCallback audioCb) { m_audioCallback = audioCb; }
    void SetDisconnectedCallback(ClientDisconnectedCallback discCb) { m_disconnectCallback = discCb; }

    // Rename a camera slot (persists to locked slots if locked)
    bool RenameCameraSlot(int camId, const std::string& newName);

    std::vector<ConnectedClient> GetConnectedClients();
    double GetClientRttMs(int deviceId);
    std::string GetClientIp(int deviceId);
    std::string GetClientUniqueId(int deviceId);
    std::string GetClientDeviceName(int deviceId);

private:
    void ListenThreadWorker();
    void ClientThreadWorker(SOCKET clientSocket, std::string clientIp, uint16_t clientPort);

    bool ReceiveExact(SOCKET sock, uint8_t* destination, size_t bytesToRead);

    void LoadLockedSlots();
    void SaveLockedSlots();
    std::string GetLocksFilePath();

    uint16_t m_port;
    SOCKET m_listenSocket;
    std::atomic<bool> m_isRunning;
    std::atomic<int> m_activeClientCount;

    std::mutex m_clientsMutex;
    std::map<int, ConnectedClient> m_clients;
    std::set<std::string> m_ignoredClients;
    std::map<int, LockedCameraSlot> m_lockedSlots;

    std::thread m_listenThread;
    FrameReceivedCallback m_frameCallback;
    HandshakeReceivedCallback m_handshakeCallback;
    CameraStateReceivedCallback m_cameraStateCallback;
    AudioReceivedCallback m_audioCallback;
    ClientDisconnectedCallback m_disconnectCallback;
};

} // namespace boulecam

