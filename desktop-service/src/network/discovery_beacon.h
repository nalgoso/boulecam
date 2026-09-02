#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string>
#include <thread>
#include <atomic>
#include <vector>

namespace boulecam {

class DiscoveryBeacon {
public:
    DiscoveryBeacon(uint16_t tcpPort = 8088, uint16_t udpPort = 8089);
    ~DiscoveryBeacon();

    bool Start();
    void Stop();

private:
    void BeaconWorker();
    void ResponderWorker();

    uint16_t m_tcpPort;
    uint16_t m_udpPort;
    SOCKET m_udpSocket;
    std::atomic<bool> m_isRunning;
    std::string m_hostname;

    std::thread m_beaconThread;
    std::thread m_responderThread;
};

} // namespace boulecam
