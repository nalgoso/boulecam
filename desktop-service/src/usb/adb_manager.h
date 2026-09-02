#pragma once

#include <string>
#include <vector>
#include <thread>
#include <atomic>

namespace boulecam {

class AdbManager {
public:
    AdbManager();
    ~AdbManager();

    bool StartAutoReverse(uint16_t localPort = 8088, uint16_t phonePort = 8088);
    void Stop();

    static bool ExecuteAdbReverse(uint16_t localPort, uint16_t phonePort);
    static bool IsAdbInstalled();
    bool IsDeviceConnected() const { return m_isDeviceConnected.load(); }

private:
    void MonitorThreadWorker();

    uint16_t m_localPort;
    uint16_t m_phonePort;
    std::atomic<bool> m_isRunning;
    std::atomic<bool> m_isDeviceConnected{false};
    std::thread m_monitorThread;
};

} // namespace boulecam
