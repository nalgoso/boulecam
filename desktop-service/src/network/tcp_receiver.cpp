#include "tcp_receiver.h"
#include <iostream>

#pragma comment(lib, "ws2_32.lib")

namespace boulecam {

TcpReceiver::TcpReceiver()
    : m_port(8088)
    , m_listenSocket(INVALID_SOCKET)
    , m_isRunning(false)
    , m_activeClientCount(0) {
    
    // Initialize WinSock
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
}

TcpReceiver::~TcpReceiver() {
    Stop();
    WSACleanup();
}

bool TcpReceiver::Start(uint16_t port, FrameReceivedCallback frameCb, HandshakeReceivedCallback handshakeCb, CameraStateReceivedCallback stateCb) {
    if (m_isRunning.load()) {
        return true;
    }

    m_port = port;
    m_frameCallback = frameCb;
    m_handshakeCallback = handshakeCb;
    m_cameraStateCallback = stateCb;

    m_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listenSocket == INVALID_SOCKET) {
        std::cerr << "[TcpReceiver] Failed to create socket: " << WSAGetLastError() << std::endl;
        return false;
    }

    // Enable SO_REUSEADDR
    int opt = 1;
    setsockopt(m_listenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY; // Bind to 0.0.0.0 (Wi-Fi + USB ADB reverse)
    serverAddr.sin_port = htons(m_port);

    if (bind(m_listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "[TcpReceiver] Bind failed on port " << m_port << ": " << WSAGetLastError() << std::endl;
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
        return false;
    }

    // Backlog 16 allows multiple cameras to queue connections simultaneously
    if (listen(m_listenSocket, 16) == SOCKET_ERROR) {
        std::cerr << "[TcpReceiver] Listen failed: " << WSAGetLastError() << std::endl;
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
        return false;
    }

    std::cout << "[TcpReceiver] Listening on 0.0.0.0:" << m_port << " (Multi-Device Low-Latency ready)" << std::endl;

    m_isRunning.store(true);
    m_listenThread = std::thread(&TcpReceiver::ListenThreadWorker, this);

    return true;
}

void TcpReceiver::Stop() {
    m_isRunning.store(false);
    m_activeClientCount.store(0);

    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (auto& pair : m_clients) {
            closesocket(pair.second.socket);
        }
        m_clients.clear();
    }

    if (m_listenSocket != INVALID_SOCKET) {
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
    }

    if (m_listenThread.joinable()) {
        m_listenThread.join();
    }
}

bool TcpReceiver::SendCameraCommand(const BouleCamCameraCmd& cmd, int deviceId) {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    if (m_clients.empty()) return false;

    if (deviceId > 0) {
        auto it = m_clients.find(deviceId);
        if (it != m_clients.end()) {
            int sent = send(it->second.socket, (const char*)&cmd, sizeof(cmd), 0);
            return sent == sizeof(cmd);
        }
        return false;
    }

    // If deviceId == 0, send to all connected devices
    bool anySent = false;
    for (auto& pair : m_clients) {
        int sent = send(pair.second.socket, (const char*)&cmd, sizeof(cmd), 0);
        if (sent == sizeof(cmd)) anySent = true;
    }
    return anySent;
}

std::vector<ConnectedClient> TcpReceiver::GetConnectedClients() {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    std::vector<ConnectedClient> list;
    for (const auto& pair : m_clients) {
        list.push_back(pair.second);
    }
    return list;
}

void TcpReceiver::ListenThreadWorker() {
    while (m_isRunning.load()) {
        sockaddr_in clientAddr{};
        int clientLen = sizeof(clientAddr);
        SOCKET clientSocket = accept(m_listenSocket, (sockaddr*)&clientAddr, &clientLen);

        if (clientSocket == INVALID_SOCKET) {
            if (!m_isRunning.load()) break;
            continue;
        }

        char clientIp[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(clientAddr.sin_addr), clientIp, INET_ADDRSTRLEN);
        uint16_t clientPort = ntohs(clientAddr.sin_port);

        // Configure Low Latency socket settings
        int nodelay = 1;
        setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));

        // Enlarge receive buffer to 2MB to prevent TCP window throttling
        int rcvBufSize = 2 * 1024 * 1024;
        setsockopt(clientSocket, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvBufSize, sizeof(rcvBufSize));

        // Socket timeouts to prevent hanging threads or freeze
        DWORD rcvTimeout = 4000;
        setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rcvTimeout, sizeof(rcvTimeout));
        DWORD sndTimeout = 2000;
        setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sndTimeout, sizeof(sndTimeout));

        // Spawn independent worker thread per connection
        std::thread(&TcpReceiver::ClientThreadWorker, this, clientSocket, std::string(clientIp), clientPort).detach();
    }
}

bool TcpReceiver::ReceiveExact(SOCKET sock, uint8_t* destination, size_t bytesToRead) {
    size_t totalRead = 0;
    while (totalRead < bytesToRead && m_isRunning.load()) {
        int bytes = recv(sock, (char*)(destination + totalRead), static_cast<int>(bytesToRead - totalRead), 0);
        if (bytes <= 0) {
            return false; // Connection closed or error
        }
        totalRead += bytes;
    }
    return totalRead == bytesToRead;
}

void TcpReceiver::ClientThreadWorker(SOCKET clientSocket, std::string clientIp, uint16_t clientPort) {
    std::vector<uint8_t> payloadBuffer;
    payloadBuffer.reserve(1024 * 1024); // 1MB initial allocation

    int assignedId = -1;

    while (m_isRunning.load()) {
        // Step 1: Read the 4-byte Magic Number to synchronize packet boundaries
        uint32_t magic = 0;
        if (!ReceiveExact(clientSocket, (uint8_t*)&magic, sizeof(magic))) {
            break;
        }

        if (magic != BOULECAM_MAGIC) {
            std::cerr << "[TcpReceiver] Invalid magic byte sequence: 0x" 
                      << std::hex << magic << std::dec << std::endl;
            break;
        }

        // Step 2: Read the packet type
        uint8_t packetType = 0;
        if (!ReceiveExact(clientSocket, &packetType, sizeof(packetType))) {
            break;
        }

        if (packetType == BOULECAM_PKT_HANDSHAKE_REQ) {
            BouleCamHandshakeReq req{};
            if (!ReceiveExact(clientSocket, (uint8_t*)&req + 5, sizeof(req) - 5)) {
                break;
            }

            std::string devName(req.device_name);

            // ANTI-DUPLICATES: Search if same client is reconnecting
            {
                std::lock_guard<std::mutex> lock(m_clientsMutex);
                for (auto& pair : m_clients) {
                    bool isUsb = (clientIp == "127.0.0.1" && pair.second.ip == "127.0.0.1");
                    bool sameDev = (!devName.empty() && pair.second.deviceName == devName);
                    bool sameWifiIp = (!isUsb && pair.second.ip == clientIp);

                    // Reconnect only if it's genuinely the same device
                    if (sameDev && (isUsb || sameWifiIp)) {
                        assignedId = pair.first;
                        // Close stale socket if different
                        if (pair.second.socket != clientSocket && pair.second.socket != INVALID_SOCKET) {
                            closesocket(pair.second.socket);
                        }
                        pair.second.socket = clientSocket;
                        pair.second.port = clientPort;
                        pair.second.deviceName = devName;
                        std::cout << "[TcpReceiver] Reconnected [Device " << assignedId << "] (" << devName 
                                  << " from " << clientIp << ":" << clientPort << ")" << std::endl;
                        break;
                    }
                }

                // If new device, assign lowest available ID (1, 2, 3...)
                if (assignedId == -1) {
                    assignedId = 1;
                    while (m_clients.find(assignedId) != m_clients.end()) {
                        assignedId++;
                    }
                    ConnectedClient client;
                    client.id = assignedId;
                    client.deviceName = devName;
                    client.socket = clientSocket;
                    client.ip = clientIp;
                    client.port = clientPort;
                    m_clients[assignedId] = client;
                    m_activeClientCount++;
                    std::cout << "[TcpReceiver] New device accepted [Device " << assignedId << "] (" 
                              << devName << " from " << clientIp << ":" << clientPort << ")" << std::endl;
                }
            }

            if (m_handshakeCallback) {
                m_handshakeCallback(assignedId, req);
            }

            // Step 3: Send back Handshake Response
            BouleCamHandshakeResp resp{};
            resp.magic = BOULECAM_MAGIC;
            resp.packet_type = BOULECAM_PKT_HANDSHAKE_RESP;
            resp.status_code = 0; // OK
            resp.negotiated_width = req.width;
            resp.negotiated_height = req.height;
            resp.negotiated_fps = req.target_fps;
            send(clientSocket, (const char*)&resp, sizeof(resp), 0);
        } else if (packetType == BOULECAM_PKT_FRAME_DATA) {
            BouleCamFrameHeader header{};
            if (!ReceiveExact(clientSocket, (uint8_t*)&header + 5, sizeof(header) - 5)) {
                break;
            }

            if (header.payload_size > 5 * 1024 * 1024) {
                std::cerr << "[TcpReceiver][Device " << assignedId << "] Frame payload too large (" 
                          << header.payload_size << " bytes). Terminating connection." << std::endl;
                break;
            }

            if (payloadBuffer.size() < header.payload_size) {
                payloadBuffer.resize(header.payload_size);
            }

            if (!ReceiveExact(clientSocket, payloadBuffer.data(), header.payload_size)) {
                break;
            }

            if (m_frameCallback && assignedId > 0) {
                m_frameCallback(assignedId, header, payloadBuffer.data(), header.payload_size);
            }
        } else if (packetType == BOULECAM_PKT_CAMERA_STATE) {
            BouleCamCameraState state{};
            if (!ReceiveExact(clientSocket, (uint8_t*)&state + 5, sizeof(state) - 5)) {
                break;
            }

            if (m_cameraStateCallback && assignedId > 0) {
                m_cameraStateCallback(assignedId, state);
            }
        } else if (packetType == BOULECAM_PKT_AUDIO_DATA) {
            BouleCamAudioHeader audioHeader{};
            if (!ReceiveExact(clientSocket, (uint8_t*)&audioHeader + 5, sizeof(audioHeader) - 5)) {
                break;
            }

            if (audioHeader.payload_size > 65536) {
                break;
            }

            if (payloadBuffer.size() < audioHeader.payload_size) {
                payloadBuffer.resize(audioHeader.payload_size);
            }

            if (!ReceiveExact(clientSocket, payloadBuffer.data(), audioHeader.payload_size)) {
                break;
            }

            if (m_audioCallback && assignedId > 0) {
                m_audioCallback(assignedId, audioHeader, payloadBuffer.data(), audioHeader.payload_size);
            }
        }
    }

    // Client disconnected or timed out: only erase if this socket is still the active one
    if (assignedId > 0) {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        if (m_clients.find(assignedId) != m_clients.end() && m_clients[assignedId].socket == clientSocket) {
            m_clients.erase(assignedId);
            m_activeClientCount--;
            std::cout << "[TcpReceiver] Device " << assignedId << " disconnected." << std::endl;
        }
    }
    closesocket(clientSocket);
}

} // namespace boulecam
