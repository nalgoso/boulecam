#include "discovery_beacon.h"
#include <iostream>
#include <chrono>
#include <iphlpapi.h>
#include <algorithm>

#pragma comment(lib, "iphlpapi.lib")

namespace boulecam {

struct SubnetDetail {
    std::string localIp;
    std::string subnetPrefix;
    std::string broadcastIp;
};

static std::vector<SubnetDetail> GetActiveSubnets() {
    std::vector<SubnetDetail> subnets;
    ULONG outBufLen = 32000;
    PIP_ADAPTER_ADDRESSES pAddresses = (IP_ADAPTER_ADDRESSES*)malloc(outBufLen);
    if (!pAddresses) return subnets;

    DWORD dwRet = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_INCLUDE_PREFIX, NULL, pAddresses, &outBufLen);
    if (dwRet == ERROR_BUFFER_OVERFLOW) {
        free(pAddresses);
        pAddresses = (IP_ADAPTER_ADDRESSES*)malloc(outBufLen);
        if (pAddresses) {
            dwRet = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_INCLUDE_PREFIX, NULL, pAddresses, &outBufLen);
        }
    }

    if (pAddresses && dwRet == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES pCurr = pAddresses; pCurr; pCurr = pCurr->Next) {
            if (pCurr->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            if (pCurr->OperStatus != IfOperStatusUp) continue;

            std::wstring desc = pCurr->Description ? pCurr->Description : L"";
            std::wstring friendly = pCurr->FriendlyName ? pCurr->FriendlyName : L"";
            bool isVirtual = (friendly.find(L"vEthernet") != std::wstring::npos ||
                              friendly.find(L"Tailscale") != std::wstring::npos ||
                              friendly.find(L"VirtualBox") != std::wstring::npos ||
                              friendly.find(L"VMware") != std::wstring::npos ||
                              friendly.find(L"ZeroTier") != std::wstring::npos ||
                              desc.find(L"Virtual") != std::wstring::npos ||
                              desc.find(L"Hyper-V") != std::wstring::npos);
            if (isVirtual) continue;

            for (PIP_ADAPTER_UNICAST_ADDRESS pUnicast = pCurr->FirstUnicastAddress; pUnicast; pUnicast = pUnicast->Next) {
                sockaddr_in* sa_in = (sockaddr_in*)pUnicast->Address.lpSockaddr;
                char ipStr[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &(sa_in->sin_addr), ipStr, INET_ADDRSTRLEN);
                std::string ip(ipStr);

                if (ip == "127.0.0.1" || ip.rfind("169.254.", 0) == 0 || ip.rfind("0.", 0) == 0) continue;

                size_t lastDot = ip.rfind('.');
                if (lastDot != std::string::npos) {
                    SubnetDetail detail;
                    detail.localIp = ip;
                    detail.subnetPrefix = ip.substr(0, lastDot + 1);
                    detail.broadcastIp = ip.substr(0, lastDot) + ".255";

                    bool alreadyExists = false;
                    for (const auto& s : subnets) {
                        if (s.subnetPrefix == detail.subnetPrefix) {
                            alreadyExists = true;
                            break;
                        }
                    }
                    if (!alreadyExists) {
                        subnets.push_back(detail);
                    }
                }
            }
        }
    }
    if (pAddresses) free(pAddresses);
    return subnets;
}

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

    SetHandleInformation((HANDLE)m_udpSocket, HANDLE_FLAG_INHERIT, 0);

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
    {
        std::lock_guard<std::mutex> lock(m_socketMutex);
        if (m_udpSocket != INVALID_SOCKET) {
            closesocket(m_udpSocket);
            m_udpSocket = INVALID_SOCKET;
        }
    }
    if (m_responderThread.joinable()) {
        m_responderThread.join();
    }
    if (m_beaconThread.joinable()) {
        m_beaconThread.join();
    }
}

void DiscoveryBeacon::SendDiscoveryProbes() {
    std::string beaconMsg = "BOULECAM_BEACON:" + std::to_string(m_tcpPort) + ":" + m_hostname;

    std::lock_guard<std::mutex> lock(m_socketMutex);
    if (m_udpSocket == INVALID_SOCKET) return;

    // 1. Global Broadcast to 255.255.255.255
    sockaddr_in broadcastAddr{};
    broadcastAddr.sin_family = AF_INET;
    broadcastAddr.sin_addr.s_addr = INADDR_BROADCAST;
    broadcastAddr.sin_port = htons(m_udpPort);
    sendto(m_udpSocket, beaconMsg.c_str(), static_cast<int>(beaconMsg.size()), 0,
           (sockaddr*)&broadcastAddr, sizeof(broadcastAddr));

    // 2. Directed subnet broadcast + Unicast sweeps for all active physical subnets
    auto subnets = GetActiveSubnets();
    for (const auto& sub : subnets) {
        // Directed subnet broadcast (e.g. 192.168.1.255)
        sockaddr_in subAddr{};
        subAddr.sin_family = AF_INET;
        inet_pton(AF_INET, sub.broadcastIp.c_str(), &subAddr.sin_addr);
        subAddr.sin_port = htons(m_udpPort);
        sendto(m_udpSocket, beaconMsg.c_str(), static_cast<int>(beaconMsg.size()), 0,
               (sockaddr*)&subAddr, sizeof(subAddr));

        // Unicast sweep across local subnet (bypasses router broadcast/multicast isolation entirely)
        for (int host = 1; host <= 254; ++host) {
            std::string targetIp = sub.subnetPrefix + std::to_string(host);
            if (targetIp == sub.localIp) continue; // Skip self

            sockaddr_in targetAddr{};
            targetAddr.sin_family = AF_INET;
            inet_pton(AF_INET, targetIp.c_str(), &targetAddr.sin_addr);
            targetAddr.sin_port = htons(m_udpPort);
            sendto(m_udpSocket, beaconMsg.c_str(), static_cast<int>(beaconMsg.size()), 0,
                   (sockaddr*)&targetAddr, sizeof(targetAddr));
        }
    }
}

void DiscoveryBeacon::BeaconWorker() {
    while (m_isRunning.load()) {
        SendDiscoveryProbes();
        // Periodically announce presence every 2 seconds
        for (int i = 0; i < 20 && m_isRunning.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void DiscoveryBeacon::TriggerBroadcast() {
    SendDiscoveryProbes();
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

        // Discard own beacons echoed back on INADDR_ANY
        if (msg.rfind("BOULECAM_BEACON", 0) == 0 || msg.rfind("BOULECAM_OFFER", 0) == 0) {
            continue;
        }

        // If a mobile phone sends "BOULECAM_DISCOVER", reply immediately with "BOULECAM_OFFER:<PORT>:<HOSTNAME>"
        if (msg.find("BOULECAM_DISCOVER") != std::string::npos) {
            char clientIp[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(clientAddr.sin_addr), clientIp, INET_ADDRSTRLEN);

            std::string replyMsg = "BOULECAM_OFFER:" + std::to_string(m_tcpPort) + ":" + m_hostname;

            std::lock_guard<std::mutex> lock(m_socketMutex);
            if (m_udpSocket != INVALID_SOCKET) {
                // Reply directly to client source address and port (unicast)
                sendto(m_udpSocket, replyMsg.c_str(), static_cast<int>(replyMsg.size()), 0,
                       (sockaddr*)&clientAddr, clientLen);

                // Also reply to client address on standard discovery port 8089
                sockaddr_in port8089 = clientAddr;
                port8089.sin_port = htons(m_udpPort);
                sendto(m_udpSocket, replyMsg.c_str(), static_cast<int>(replyMsg.size()), 0,
                       (sockaddr*)&port8089, sizeof(port8089));
            }

            std::cout << "[Discovery] Received probe from mobile " << clientIp << ". Sent auto-discovery offer." << std::endl;
        }
    }
}

} // namespace boulecam
