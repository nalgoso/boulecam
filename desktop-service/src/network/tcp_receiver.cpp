#include "tcp_receiver.h"
#include <iostream>
#include <mstcpip.h>

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

bool TcpReceiver::DisconnectClient(int deviceId, bool blockFutureReconnect) {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    auto it = m_clients.find(deviceId);
    if (it != m_clients.end()) {
        if (blockFutureReconnect) {
            if (!it->second.ip.empty() && it->second.ip != "127.0.0.1") {
                m_ignoredClients.insert(it->second.ip);
            }
            if (!it->second.deviceName.empty()) {
                m_ignoredClients.insert(it->second.deviceName);
            }
        }
        if (it->second.deviceIdRef) {
            it->second.deviceIdRef->store(-1);
        }
        if (it->second.socket != INVALID_SOCKET) {
            closesocket(it->second.socket);
            it->second.socket = INVALID_SOCKET;
        }
        m_clients.erase(it);
        m_activeClientCount--;
        std::cout << "[TcpReceiver] Client [Device " << deviceId << "] disconnected and unlinked." << std::endl;
        return true;
    }
    return false;
}

void TcpReceiver::ClearIgnoredClients() {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    m_ignoredClients.clear();
    std::cout << "[TcpReceiver] Cleared ignored clients list (ready for discovery/rescan)." << std::endl;
}

bool TcpReceiver::SwapClientIds(int camA, int camB) {
    if (camA == camB) return true;
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    bool hasA = (m_clients.find(camA) != m_clients.end());
    bool hasB = (m_clients.find(camB) != m_clients.end());
    if (!hasA && !hasB) return false;

    if (hasA && hasB) {
        ConnectedClient clientA = m_clients[camA];
        ConnectedClient clientB = m_clients[camB];

        clientA.id = camB;
        if (clientA.deviceIdRef) clientA.deviceIdRef->store(camB);

        clientB.id = camA;
        if (clientB.deviceIdRef) clientB.deviceIdRef->store(camA);

        m_clients[camA] = clientB;
        m_clients[camB] = clientA;
        std::cout << "[TcpReceiver] Swapped client IDs: " << camA << " <-> " << camB << std::endl;
        return true;
    } else if (hasA) {
        ConnectedClient clientA = m_clients[camA];
        clientA.id = camB;
        if (clientA.deviceIdRef) clientA.deviceIdRef->store(camB);
        m_clients[camB] = clientA;
        m_clients.erase(camA);
        std::cout << "[TcpReceiver] Reassigned client ID: " << camA << " -> " << camB << std::endl;
        return true;
    } else {
        ConnectedClient clientB = m_clients[camB];
        clientB.id = camA;
        if (clientB.deviceIdRef) clientB.deviceIdRef->store(camA);
        m_clients[camA] = clientB;
        m_clients.erase(camB);
        std::cout << "[TcpReceiver] Reassigned client ID: " << camB << " -> " << camA << std::endl;
        return true;
    }
}

bool TcpReceiver::ReassignClientId(int fromId, int toId) {
    return SwapClientIds(fromId, toId);
}

std::vector<ConnectedClient> TcpReceiver::GetConnectedClients() {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    std::vector<ConnectedClient> list;
    for (const auto& pair : m_clients) {
        list.push_back(pair.second);
    }
    return list;
}

double TcpReceiver::GetClientRttMs(int deviceId) {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    auto it = m_clients.find(deviceId);
    if (it != m_clients.end() && it->second.socket != INVALID_SOCKET) {
        TCP_INFO_v0 tcpInfo{};
        DWORD bytesReturned = 0;
        int res = WSAIoctl(it->second.socket, SIO_TCP_INFO, NULL, 0, &tcpInfo, sizeof(tcpInfo), &bytesReturned, NULL, NULL);
        if (res == 0 && tcpInfo.RttUs > 0) {
            return static_cast<double>(tcpInfo.RttUs) / 1000.0;
        }
        if (it->second.ip == "127.0.0.1") {
            return 1.5; // Sub-millisecond localhost loopback
        }
    }
    return 0.0;
}

std::string TcpReceiver::GetClientIp(int deviceId) {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    auto it = m_clients.find(deviceId);
    if (it != m_clients.end()) {
        return it->second.ip;
    }
    return "";
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

    auto deviceIdRef = std::make_shared<std::atomic<int>>(-1);

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

            // Check if device is ignored/unlinked
            {
                std::lock_guard<std::mutex> lock(m_clientsMutex);
                if (m_ignoredClients.find(clientIp) != m_ignoredClients.end() ||
                    (!devName.empty() && m_ignoredClients.find(devName) != m_ignoredClients.end())) {
                    std::cout << "[TcpReceiver] Blocked reconnect from unlinked client: " << devName 
                              << " (" << clientIp << ")" << std::endl;
                    break;
                }
            }

            int assignedId = -1;

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
                        pair.second.deviceIdRef = deviceIdRef;
                        deviceIdRef->store(assignedId);
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
                    client.deviceIdRef = deviceIdRef;
                    deviceIdRef->store(assignedId);
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

            int currentId = deviceIdRef->load();
            if (currentId <= 0) break;

            if (header.payload_size > 5 * 1024 * 1024) {
                std::cerr << "[TcpReceiver][Device " << currentId << "] Frame payload too large (" 
                          << header.payload_size << " bytes). Terminating connection." << std::endl;
                break;
            }

            if (payloadBuffer.size() < header.payload_size) {
                payloadBuffer.resize(header.payload_size);
            }

            if (!ReceiveExact(clientSocket, payloadBuffer.data(), header.payload_size)) {
                break;
            }

            if (m_frameCallback && currentId > 0) {
                m_frameCallback(currentId, header, payloadBuffer.data(), header.payload_size);
            }
        } else if (packetType == BOULECAM_PKT_CAMERA_STATE) {
            BouleCamCameraState state{};
            if (!ReceiveExact(clientSocket, (uint8_t*)&state + 5, sizeof(state) - 5)) {
                break;
            }

            int currentId = deviceIdRef->load();
            if (m_cameraStateCallback && currentId > 0) {
                m_cameraStateCallback(currentId, state);
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

            int currentId = deviceIdRef->load();
            if (m_audioCallback && currentId > 0) {
                m_audioCallback(currentId, audioHeader, payloadBuffer.data(), audioHeader.payload_size);
            }
        }
    }

    // Client disconnected or timed out: only erase if this socket is still the active one
    int finalDevId = deviceIdRef->load();
    if (finalDevId > 0) {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        if (m_clients.find(finalDevId) != m_clients.end() && m_clients[finalDevId].socket == clientSocket) {
            m_clients.erase(finalDevId);
            m_activeClientCount--;
            std::cout << "[TcpReceiver] Device " << finalDevId << " disconnected." << std::endl;
        }
    }
    closesocket(clientSocket);
}

} // namespace boulecam
