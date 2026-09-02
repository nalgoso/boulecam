#include "usbmuxd_client.h"
#include <ws2tcpip.h>
#include <iostream>
#include <sstream>

namespace boulecam {

UsbmuxdClient::UsbmuxdClient() {
}

UsbmuxdClient::~UsbmuxdClient() {
}

SOCKET UsbmuxdClient::ConnectToUsbmuxdDaemon() {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return INVALID_SOCKET;

    sockaddr_in daemonAddr{};
    daemonAddr.sin_family = AF_INET;
    daemonAddr.sin_port = htons(27015); // Default Apple usbmuxd port on Windows
    inet_pton(AF_INET, "127.0.0.1", &daemonAddr.sin_addr);

    if (connect(sock, (sockaddr*)&daemonAddr, sizeof(daemonAddr)) == SOCKET_ERROR) {
        closesocket(sock);
        return INVALID_SOCKET;
    }

    int nodelay = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
    return sock;
}

bool UsbmuxdClient::IsUsbmuxdRunning() {
    UsbmuxdClient client;
    SOCKET s = client.ConnectToUsbmuxdDaemon();
    if (s != INVALID_SOCKET) {
        closesocket(s);
        return true;
    }
    return false;
}

SOCKET UsbmuxdClient::ConnectToDevice(uint32_t deviceId, uint16_t targetPort) {
    SOCKET s = ConnectToUsbmuxdDaemon();
    if (s == INVALID_SOCKET) {
        std::cerr << "[UsbmuxdClient] Apple Mobile Device Service (usbmuxd) not reachable on 127.0.0.1:27015." << std::endl;
        return INVALID_SOCKET;
    }

    // Convert port to network byte order (Big Endian) as required by usbmuxd plist
    uint16_t portBE = htons(targetPort);

    // Build XML Plist payload for 'Connect' message
    std::ostringstream plist;
    plist << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
          << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
          << "<plist version=\"1.0\">\n"
          << "<dict>\n"
          << "    <key>MessageType</key>\n"
          << "    <string>Connect</string>\n"
          << "    <key>ClientVersionString</key>\n"
          << "    <string>BouleCam-1.0</string>\n"
          << "    <key>ProgName</key>\n"
          << "    <string>BouleCamDesktop</string>\n"
          << "    <key>DeviceID</key>\n"
          << "    <integer>" << deviceId << "</integer>\n"
          << "    <key>PortNumber</key>\n"
          << "    <integer>" << portBE << "</integer>\n"
          << "</dict>\n"
          << "</plist>";

    std::string xmlStr = plist.str();
    uint32_t totalLength = static_cast<uint32_t>(sizeof(UsbmuxHeader) + xmlStr.length());

    UsbmuxHeader hdr{};
    hdr.length = totalLength;
    hdr.version = 1; // XML Plist version
    hdr.message = 1; // Plist message
    hdr.tag = 101;

    // Send packet
    send(s, (const char*)&hdr, sizeof(hdr), 0);
    send(s, xmlStr.c_str(), static_cast<int>(xmlStr.length()), 0);

    // Read result header from usbmuxd
    UsbmuxHeader respHdr{};
    int received = recv(s, (char*)&respHdr, sizeof(respHdr), 0);
    if (received < sizeof(respHdr)) {
        closesocket(s);
        return INVALID_SOCKET;
    }

    // Read response plist
    std::vector<char> respBody(respHdr.length - sizeof(respHdr) + 1, 0);
    recv(s, respBody.data(), static_cast<int>(respHdr.length - sizeof(respHdr)), 0);

    std::string responseXml(respBody.data());
    // Check for "<integer>0</integer>" in response indicating Number=0 (Success)
    if (responseXml.find("<integer>0</integer>") != std::string::npos ||
        responseXml.find("<key>Number</key><integer>0</integer>") != std::string::npos) {
        std::cout << "[UsbmuxdClient] Tunnel established to iOS device ID " << deviceId << " over USB cable!" << std::endl;
        return s; // Socket is now a raw TCP stream directly into the iPhone app!
    }

    std::cerr << "[UsbmuxdClient] Usbmuxd connect rejected: " << responseXml << std::endl;
    closesocket(s);
    return INVALID_SOCKET;
}

bool UsbmuxdClient::QueryConnectedDevices(std::vector<uint32_t>& outDeviceIds) {
    SOCKET s = ConnectToUsbmuxdDaemon();
    if (s == INVALID_SOCKET) return false;

    // Send Listen message to discover devices
    std::string xmlStr = 
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\"><dict><key>MessageType</key><string>Listen</string>"
        "<key>ClientVersionString</key><string>BouleCam-1.0</string></dict></plist>";

    UsbmuxHeader hdr{};
    hdr.length = static_cast<uint32_t>(sizeof(UsbmuxHeader) + xmlStr.length());
    hdr.version = 1;
    hdr.message = 1;
    hdr.tag = 1;

    send(s, (const char*)&hdr, sizeof(hdr), 0);
    send(s, xmlStr.c_str(), static_cast<int>(xmlStr.length()), 0);

    // Read response with devices
    UsbmuxHeader respHdr{};
    int received = recv(s, (char*)&respHdr, sizeof(respHdr), 0);
    if (received >= sizeof(respHdr)) {
        std::vector<char> respBody(respHdr.length - sizeof(respHdr) + 1, 0);
        recv(s, respBody.data(), static_cast<int>(respHdr.length - sizeof(respHdr)), 0);
        std::string resp(respBody.data());
        
        // Simple search for DeviceID tag
        size_t pos = 0;
        while ((pos = resp.find("<key>DeviceID</key>", pos)) != std::string::npos) {
            size_t intStart = resp.find("<integer>", pos);
            size_t intEnd = resp.find("</integer>", intStart);
            if (intStart != std::string::npos && intEnd != std::string::npos) {
                std::string idStr = resp.substr(intStart + 9, intEnd - intStart - 9);
                uint32_t devId = static_cast<uint32_t>(std::stoul(idStr));
                outDeviceIds.push_back(devId);
            }
            pos = intEnd;
        }
    }

    closesocket(s);
    return !outDeviceIds.empty();
}

} // namespace boulecam
