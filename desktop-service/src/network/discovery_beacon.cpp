#include "discovery_beacon.h"
#include <iostream>
#include <chrono>
#include <iphlpapi.h>

#pragma comment(lib, "iphlpapi.lib")

namespace boulecam {

DiscoveryBeacon::DiscoveryBeacon(uint16_t tcpPort, uint16_t udpPort)
    : m_tcpPort(tcpPort)
    , m_udpPort(udpPort)
    , m_udpSocket(INVALID_SOCKET)
    , m_isRunning(false) {
    char hostname[256] = "BouleCam-PC";
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        m_hostname = hostname;
    } else {
        m_hostname = "Windows-PC";
    }
}

DiscoveryBeacon::~DiscoveryBeacon() {
    Stop();
}

bool DiscoveryBeacon::Start() {
    if (m_isRunning.load()) return true;

    m_udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_udpSocket == INVALID_SOCKET) {
        std::cerr << "[Discovery] Failed to create UDP discovery socket: " << WSAGetLastError() << std::endl;
        return false;
    }

    // Enable Broadcast
    int broadcastOpt = 1;
    setsockopt(m_udpSocket, SOL_SOCKET, SO_BROADCAST, (const char*)&broadcastOpt, sizeof(broadcastOpt));

    // Enable Reuse Address
    int reuseOpt = 1;
    setsockopt(m_udpSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuseOpt, sizeof(reuseOpt));

    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    bindAddr.sin_port = htons(m_udpPort);

    if (bind(m_udpSocket, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
        std::cerr << "[Discovery] Failed to bind UDP discovery port " << m_udpPort << ": " << WSAGetLastError() << std::endl;
        closesocket(m_udpSocket);
        m_udpSocket = INVALID_SOCKET;
        return false;
    }

    std::cout << "[Discovery] Auto-Discovery UDP beacon active on port " << m_udpPort << " (" << m_hostname << ")" << std::endl;

    m_isRunning.store(true);
    m_responderThread = std::thread(&DiscoveryBeacon::ResponderWorker, this);
    m_beaconThread = std::thread(&DiscoveryBeacon::BeaconWorker, this);
    return true;
}

void DiscoveryBeacon::Stop() {
    m_isRunning.store(false);
    if (m_udpSocket != INVALID_SOCKET) {
        closesocket(m_udpSocket);
        m_udpSocket = INVALID_SOCKET;
    }
    if (m_responderThread.joinable()) {
        m_responderThread.join();
    }
    if (m_beaconThread.joinable()) {
        m_beaconThread.join();
    }
}

static std::vector<std::string> GetLocalBroadcastAddresses() {
    std::vector<std::string> bcastList;
    ULONG outBufLen = 15000;
    PIP_ADAPTER_ADDRESSES pAddresses = (IP_ADAPTER_ADDRESSES*)malloc(outBufLen);
    if (!pAddresses) return bcastList;

    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, NULL, pAddresses, &outBufLen) == ERROR_BUFFER_OVERFLOW) {
        free(pAddresses);
        pAddresses = (IP_ADAPTER_ADDRESSES*)malloc(outBufLen);
    }

    if (pAddresses && GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, NULL, pAddresses, &outBufLen) == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES pCurr = pAddresses; pCurr; pCurr = pCurr->Next) {
            if (pCurr->OperStatus != IfOperStatusUp) continue;
            for (PIP_ADAPTER_UNICAST_ADDRESS pUnicast = pCurr->FirstUnicastAddress; pUnicast; pUnicast = pUnicast->Next) {
                sockaddr_in* sa_in = (sockaddr_in*)pUnicast->Address.lpSockaddr;
                char ipStr[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &(sa_in->sin_addr), ipStr, INET_ADDRSTRLEN);
                std::string ip(ipStr);
                if (ip != "127.0.0.1") {
                    size_t lastDot = ip.rfind('.');
                    if (lastDot != std::string::npos) {
                        bcastList.push_back(ip.substr(0, lastDot) + ".255");
                    }
                }
            }
        }
    }
    if (pAddresses) free(pAddresses);
    return bcastList;
}

void DiscoveryBeacon::BeaconWorker() {
    std::string beaconMsg = "BOULECAM_BEACON:" + std::to_string(m_tcpPort) + ":" + m_hostname;

    sockaddr_in broadcastAddr{};
    broadcastAddr.sin_family = AF_INET;
    broadcastAddr.sin_addr.s_addr = INADDR_BROADCAST; // 255.255.255.255
    broadcastAddr.sin_port = htons(m_udpPort);

    while (m_isRunning.load()) {
        if (m_udpSocket != INVALID_SOCKET) {
            // Global broadcast
            sendto(m_udpSocket, beaconMsg.c_str(), static_cast<int>(beaconMsg.size()), 0,
                   (sockaddr*)&broadcastAddr, sizeof(broadcastAddr));

            // Specific local subnet broadcasts for all active network adapters
            auto subnets = GetLocalBroadcastAddresses();
            for (const auto& bcastIp : subnets) {
                sockaddr_in subAddr{};
                subAddr.sin_family = AF_INET;
                inet_pton(AF_INET, bcastIp.c_str(), &subAddr.sin_addr);
                subAddr.sin_port = htons(m_udpPort);
                sendto(m_udpSocket, beaconMsg.c_str(), static_cast<int>(beaconMsg.size()), 0,
                       (sockaddr*)&subAddr, sizeof(subAddr));
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void DiscoveryBeacon::ResponderWorker() {
    char buffer[512];
    while (m_isRunning.load()) {
        sockaddr_in clientAddr{};
        int clientLen = sizeof(clientAddr);
        int bytes = recvfrom(m_udpSocket, buffer, sizeof(buffer) - 1, 0, (sockaddr*)&clientAddr, &clientLen);

        if (bytes <= 0) {
            if (!m_isRunning.load()) break;
            continue;
        }

        buffer[bytes] = '\0';
        std::string msg(buffer);

        // If a mobile phone sends "BOULECAM_DISCOVER", reply immediately with "BOULECAM_OFFER:<PORT>:<HOSTNAME>"
        if (msg.find("BOULECAM_DISCOVER") != std::string::npos) {
            char clientIp[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(clientAddr.sin_addr), clientIp, INET_ADDRSTRLEN);

            std::string replyMsg = "BOULECAM_OFFER:" + std::to_string(m_tcpPort) + ":" + m_hostname;
            // Reply directly to client address and port
            sendto(m_udpSocket, replyMsg.c_str(), static_cast<int>(replyMsg.size()), 0,
                   (sockaddr*)&clientAddr, clientLen);

            // Also reply to client address on standard discovery port 8089
            sockaddr_in port8089 = clientAddr;
            port8089.sin_port = htons(m_udpPort);
            sendto(m_udpSocket, replyMsg.c_str(), static_cast<int>(replyMsg.size()), 0,
                   (sockaddr*)&port8089, sizeof(port8089));

            std::cout << "[Discovery] Received probe from mobile " << clientIp << ". Sent auto-discovery offer." << std::endl;
        }
    }
}

} // namespace boulecam
