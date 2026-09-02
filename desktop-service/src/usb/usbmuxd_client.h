#pragma once

#include <winsock2.h>
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace boulecam {

/**
 * Usbmuxd protocol header (16 bytes)
 */
#pragma pack(push, 1)
struct UsbmuxHeader {
    uint32_t length;    // Total packet length including header
    uint32_t version;   // Protocol version (1 for XML plist, 0 for binary legacy)
    uint32_t message;   // Message type (e.g. 1 for plist / result)
    uint32_t tag;       // Client-generated request tag
};
#pragma pack(pop)

class UsbmuxdClient {
public:
    UsbmuxdClient();
    ~UsbmuxdClient();

    // Connects to local usbmuxd service (127.0.0.1:27015) and requests tunnel to iOS app port
    SOCKET ConnectToDevice(uint32_t deviceId, uint16_t targetPort);

    // Queries available iOS devices connected via USB
    bool QueryConnectedDevices(std::vector<uint32_t>& outDeviceIds);

    static bool IsUsbmuxdRunning();

private:
    SOCKET ConnectToUsbmuxdDaemon();
};

} // namespace boulecam
