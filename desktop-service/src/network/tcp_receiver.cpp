#include "tcp_receiver.h"
#include <iostream>
#include <fstream>
#include <sstream>
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

    // Load persisted slot locks
    LoadLockedSlots();
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
    } else if (hasA) {
        ConnectedClient clientA = m_clients[camA];
        clientA.id = camB;
        if (clientA.deviceIdRef) clientA.deviceIdRef->store(camB);
        m_clients[camB] = clientA;
        m_clients.erase(camA);
    } else {
        ConnectedClient clientB = m_clients[camB];
        clientB.id = camA;
        if (clientB.deviceIdRef) clientB.deviceIdRef->store(camA);
        m_clients[camA] = clientB;
        m_clients.erase(camB);
    }

    // Update lock mappings if any slot was locked
    bool hadLockA = (m_lockedSlots.find(camA) != m_lockedSlots.end() && m_lockedSlots[camA].isLocked);
    bool hadLockB = (m_lockedSlots.find(camB) != m_lockedSlots.end() && m_lockedSlots[camB].isLocked);
    if (hadLockA || hadLockB) {
        auto lockA = hadLockA ? m_lockedSlots[camA] : LockedCameraSlot{};
        auto lockB = hadLockB ? m_lockedSlots[camB] : LockedCameraSlot{};
        if (hadLockA) { lockA.camId = camB; m_lockedSlots[camB] = lockA; } else { m_lockedSlots.erase(camB); }
        if (hadLockB) { lockB.camId = camA; m_lockedSlots[camA] = lockB; } else { m_lockedSlots.erase(camA); }
        SaveLockedSlots();
    }

    std::cout << "[TcpReceiver] Swapped/Reassigned client IDs: " << camA << " <-> " << camB << std::endl;
    return true;
}

bool TcpReceiver::ReassignClientId(int fromId, int toId) {
    return SwapClientIds(fromId, toId);
}

std::string TcpReceiver::GetLocksFilePath() {
    char appData[MAX_PATH];
    if (GetEnvironmentVariableA("APPDATA", appData, MAX_PATH) > 0) {
        std::string dir = std::string(appData) + "\\BouleCam";
        CreateDirectoryA(dir.c_str(), NULL);
        return dir + "\\locked_cameras.json";
    }
    return "boulecam_locks.json";
}

void TcpReceiver::SaveLockedSlots() {
    std::string path = GetLocksFilePath();
    std::ofstream ofs(path);
    if (!ofs.is_open()) return;

    ofs << "{\n  \"lockedSlots\": [\n";
    size_t count = 0;
    for (const auto& pair : m_lockedSlots) {
        if (!pair.second.isLocked) continue;
        if (count > 0) ofs << ",\n";
        ofs << "    {\n"
            << "      \"camId\": " << pair.second.camId << ",\n"
            << "      \"uniqueId\": \"" << pair.second.uniqueId << "\",\n"
            << "      \"deviceName\": \"" << pair.second.deviceName << "\",\n"
            << "      \"isLocked\": true\n"
            << "    }";
        count++;
    }
    ofs << "\n  ]\n}\n";
    ofs.close();
}

void TcpReceiver::LoadLockedSlots() {
    std::string path = GetLocksFilePath();
    std::ifstream ifs(path);
    if (!ifs.is_open()) return;

    std::string line;
    LockedCameraSlot currentSlot;
    bool inObject = false;

    auto cleanStr = [](std::string s) {
        size_t first = s.find_first_not_of(" \t\r\n\",");
        if (first == std::string::npos) return std::string("");
        size_t last = s.find_last_not_of(" \t\r\n\",");
        return s.substr(first, (last - first + 1));
    };

    while (std::getline(ifs, line)) {
        if (line.find("{") != std::string::npos && line.find("lockedSlots") == std::string::npos) {
            inObject = true;
            currentSlot = LockedCameraSlot();
        } else if (line.find("}") != std::string::npos && inObject) {
            if (currentSlot.camId > 0 && !currentSlot.uniqueId.empty() && currentSlot.isLocked) {
                m_lockedSlots[currentSlot.camId] = currentSlot;
            }
            inObject = false;
        } else if (inObject) {
            auto posColon = line.find(":");
            if (posColon != std::string::npos) {
                std::string key = line.substr(0, posColon);
                std::string val = line.substr(posColon + 1);

                if (key.find("camId") != std::string::npos) {
                    try { currentSlot.camId = std::stoi(cleanStr(val)); } catch (...) {}
                } else if (key.find("uniqueId") != std::string::npos) {
                    currentSlot.uniqueId = cleanStr(val);
                } else if (key.find("deviceName") != std::string::npos) {
                    currentSlot.deviceName = cleanStr(val);
                } else if (key.find("isLocked") != std::string::npos) {
                    currentSlot.isLocked = (val.find("true") != std::string::npos || val.find("1") != std::string::npos);
                }
            }
        }
    }
    std::cout << "[TcpReceiver] Loaded " << m_lockedSlots.size() << " locked camera slot(s) from " << path << std::endl;
}

bool TcpReceiver::LockCameraSlot(int camId, bool lock, const std::string& uniqueId, const std::string& devName) {
    std::lock_guard<std::mutex> lockGuard(m_clientsMutex);
    if (camId <= 0) return false;

    if (!lock) {
        auto it = m_lockedSlots.find(camId);
        if (it != m_lockedSlots.end()) {
            it->second.isLocked = false;
            m_lockedSlots.erase(it);
            SaveLockedSlots();
            std::cout << "[TcpReceiver] Cam " << camId << " UNLOCKED." << std::endl;
        }
        return true;
    }

    std::string targetUid = uniqueId;
    std::string targetName = devName;

    if (targetUid.empty()) {
        auto itClient = m_clients.find(camId);
        if (itClient != m_clients.end()) {
            targetUid = itClient->second.uniqueId;
            if (targetName.empty()) targetName = itClient->second.deviceName;
        }
    }

    if (targetUid.empty()) {
        std::cerr << "[TcpReceiver] Cannot lock Cam " << camId << ": no unique ID available." << std::endl;
        return false;
    }

    // Remove any previous lock for this unique ID on another slot
    for (auto it = m_lockedSlots.begin(); it != m_lockedSlots.end(); ) {
        if (it->second.uniqueId == targetUid) {
            it = m_lockedSlots.erase(it);
        } else {
            ++it;
        }
    }

    LockedCameraSlot slot;
    slot.camId = camId;
    slot.uniqueId = targetUid;
    slot.deviceName = targetName.empty() ? ("Móvil " + std::to_string(camId)) : targetName;
    slot.isLocked = true;
    m_lockedSlots[camId] = slot;
    SaveLockedSlots();

    std::cout << "[TcpReceiver] Cam " << camId << " LOCKED to device '" << slot.deviceName 
              << "' (ID: " << targetUid << ")" << std::endl;
    return true;
}

bool TcpReceiver::IsCameraSlotLocked(int camId) {
    std::lock_guard<std::mutex> lockGuard(m_clientsMutex);
    auto it = m_lockedSlots.find(camId);
    return (it != m_lockedSlots.end() && it->second.isLocked);
}

std::vector<LockedCameraSlot> TcpReceiver::GetLockedSlots() {
    std::lock_guard<std::mutex> lockGuard(m_clientsMutex);
    std::vector<LockedCameraSlot> list;
    for (const auto& pair : m_lockedSlots) {
        if (pair.second.isLocked) {
            list.push_back(pair.second);
        }
    }
    return list;
}

LockedCameraSlot TcpReceiver::GetSlotLock(int camId) {
    std::lock_guard<std::mutex> lockGuard(m_clientsMutex);
    auto it = m_lockedSlots.find(camId);
    if (it != m_lockedSlots.end() && it->second.isLocked) {
        return it->second;
    }
    return LockedCameraSlot{};
}

std::string TcpReceiver::GetClientUniqueId(int deviceId) {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    auto it = m_clients.find(deviceId);
    if (it != m_clients.end()) {
        return it->second.uniqueId;
    }
    auto itLock = m_lockedSlots.find(deviceId);
    if (itLock != m_lockedSlots.end() && itLock->second.isLocked) {
        return itLock->second.uniqueId;
    }
    return "";
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

            std::string devName(req.device_name, strnlen(req.device_name, sizeof(req.device_name)));
            std::string devUid(req.device_id, strnlen(req.device_id, sizeof(req.device_id)));

            // Extract fallback unique ID if not provided by client
            if (devUid.empty()) {
                if (clientIp == "127.0.0.1") {
                    devUid = "usb_" + (!devName.empty() ? devName : "android");
                } else {
                    devUid = "wifi_" + clientIp;
                }
            }

            // Check if device is ignored/unlinked
            {
                std::lock_guard<std::mutex> lock(m_clientsMutex);
                if (m_ignoredClients.find(clientIp) != m_ignoredClients.end() ||
                    (!devName.empty() && m_ignoredClients.find(devName) != m_ignoredClients.end()) ||
                    m_ignoredClients.find(devUid) != m_ignoredClients.end()) {
                    std::cout << "[TcpReceiver] Blocked reconnect from unlinked client: " << devName 
                              << " (" << devUid << " / " << clientIp << ")" << std::endl;
                    break;
                }
            }

            int assignedId = -1;

            {
                std::lock_guard<std::mutex> lock(m_clientsMutex);

                // PRIORITY 1: Check if this device has a LOCKED camera slot
                for (const auto& lockPair : m_lockedSlots) {
                    if (lockPair.second.isLocked && lockPair.second.uniqueId == devUid) {
                        assignedId = lockPair.first;
                        std::cout << "[TcpReceiver] Device '" << devName << "' (ID: " << devUid 
                                  << ") is LOCKED to Cam " << assignedId << std::endl;
                        break;
                    }
                }

                if (assignedId != -1) {
                    auto itOcc = m_clients.find(assignedId);
                    if (itOcc != m_clients.end()) {
                        if (itOcc->second.uniqueId == devUid) {
                            // Reconnect of same device
                            if (itOcc->second.socket != clientSocket && itOcc->second.socket != INVALID_SOCKET) {
                                closesocket(itOcc->second.socket);
                            }
                            itOcc->second.socket = clientSocket;
                            itOcc->second.port = clientPort;
                            itOcc->second.deviceName = devName;
                            itOcc->second.ip = clientIp;
                            itOcc->second.deviceIdRef = deviceIdRef;
                            deviceIdRef->store(assignedId);
                            std::cout << "[TcpReceiver] Reconnected locked [Device " << assignedId << "] (" << devName 
                                      << " from " << clientIp << ":" << clientPort << ")" << std::endl;
                        } else {
                            // Another transient (unlocked) device was in this slot: relocate it
                            int newFreeId = 1;
                            while (true) {
                                bool occ = (m_clients.find(newFreeId) != m_clients.end() || newFreeId == assignedId);
                                bool lck = (m_lockedSlots.find(newFreeId) != m_lockedSlots.end() && m_lockedSlots[newFreeId].isLocked);
                                if (!occ && !lck) break;
                                newFreeId++;
                            }
                            ConnectedClient relocated = itOcc->second;
                            relocated.id = newFreeId;
                            if (relocated.deviceIdRef) relocated.deviceIdRef->store(newFreeId);
                            m_clients[newFreeId] = relocated;
                            std::cout << "[TcpReceiver] Relocated transient device from locked Cam " << assignedId 
                                      << " -> Cam " << newFreeId << std::endl;

                            // Assign slot to the locked owner
                            ConnectedClient client;
                            client.id = assignedId;
                            client.deviceName = devName;
                            client.uniqueId = devUid;
                            client.socket = clientSocket;
                            client.ip = clientIp;
                            client.port = clientPort;
                            client.deviceIdRef = deviceIdRef;
                            deviceIdRef->store(assignedId);
                            m_clients[assignedId] = client;
                        }
                    } else {
                        // Slot is vacant: assign it
                        ConnectedClient client;
                        client.id = assignedId;
                        client.deviceName = devName;
                        client.uniqueId = devUid;
                        client.socket = clientSocket;
                        client.ip = clientIp;
                        client.port = clientPort;
                        client.deviceIdRef = deviceIdRef;
                        deviceIdRef->store(assignedId);
                        m_clients[assignedId] = client;
                        m_activeClientCount++;
                    }
                } else {
                    // PRIORITY 2: Not locked. Check if reconnecting to an existing slot
                    for (auto& pair : m_clients) {
                        bool sameUid = (!devUid.empty() && pair.second.uniqueId == devUid);
                        bool isUsb = (clientIp == "127.0.0.1" && pair.second.ip == "127.0.0.1");
                        bool sameDev = (!devName.empty() && pair.second.deviceName == devName);
                        bool sameWifiIp = (!isUsb && pair.second.ip == clientIp);

                        if (sameUid || (sameDev && (isUsb || sameWifiIp))) {
                            assignedId = pair.first;
                            if (pair.second.socket != clientSocket && pair.second.socket != INVALID_SOCKET) {
                                closesocket(pair.second.socket);
                            }
                            pair.second.socket = clientSocket;
                            pair.second.port = clientPort;
                            pair.second.deviceName = devName;
                            pair.second.uniqueId = devUid;
                            pair.second.ip = clientIp;
                            pair.second.deviceIdRef = deviceIdRef;
                            deviceIdRef->store(assignedId);
                            std::cout << "[TcpReceiver] Reconnected [Device " << assignedId << "] (" << devName 
                                      << " from " << clientIp << ":" << clientPort << ")" << std::endl;
                            break;
                        }
                    }

                    // PRIORITY 3: New device. Assign lowest available slot that is NOT OCCUPIED and NOT LOCKED to someone else
                    if (assignedId == -1) {
                        assignedId = 1;
                        while (true) {
                            bool occupied = (m_clients.find(assignedId) != m_clients.end());
                            bool lockedToOther = false;
                            auto itLock = m_lockedSlots.find(assignedId);
                            if (itLock != m_lockedSlots.end() && itLock->second.isLocked) {
                                if (itLock->second.uniqueId != devUid) {
                                    lockedToOther = true;
                                }
                            }
                            if (!occupied && !lockedToOther) {
                                break;
                            }
                            assignedId++;
                        }

                        ConnectedClient client;
                        client.id = assignedId;
                        client.deviceName = devName;
                        client.uniqueId = devUid;
                        client.socket = clientSocket;
                        client.ip = clientIp;
                        client.port = clientPort;
                        client.deviceIdRef = deviceIdRef;
                        deviceIdRef->store(assignedId);
                        m_clients[assignedId] = client;
                        m_activeClientCount++;
                        std::cout << "[TcpReceiver] New device accepted [Device " << assignedId << "] (" 
                                  << devName << " / ID: " << devUid << " from " << clientIp << ":" << clientPort << ")" << std::endl;
                    }
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
        bool notifyDisconnect = false;
        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            if (m_clients.find(finalDevId) != m_clients.end() && m_clients[finalDevId].socket == clientSocket) {
                m_clients.erase(finalDevId);
                m_activeClientCount--;
                notifyDisconnect = true;
                std::cout << "[TcpReceiver] Device " << finalDevId << " disconnected." << std::endl;
            }
        }
        if (notifyDisconnect && m_disconnectCallback) {
            m_disconnectCallback(finalDevId);
        }
    }
    closesocket(clientSocket);
}

} // namespace boulecam
