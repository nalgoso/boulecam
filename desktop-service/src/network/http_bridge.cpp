#include "http_bridge.h"
#include "tcp_receiver.h"
#include <iostream>
#include <sstream>
#include <iphlpapi.h>

#pragma comment(lib, "iphlpapi.lib")

namespace boulecam {

static void ConvertNV12ToBMP(const uint8_t* nv12, uint32_t width, uint32_t height, std::vector<uint8_t>& bmpOut, uint16_t rotation = 0, uint32_t dataSize = 0) {
    // If rotation is 90 or 270 (Vertical), swap width and height!
    bool isVertical = (rotation == 90 || rotation == 270);
    uint32_t outWidth = isVertical ? (height / 2) : (width / 2);
    uint32_t outHeight = isVertical ? (width / 2) : (height / 2);

    uint32_t rowSize = (outWidth * 3 + 3) & ~3; // 4-byte aligned rows
    uint32_t imageSize = rowSize * outHeight;
    uint32_t fileSize = 54 + imageSize;

    bmpOut.resize(fileSize);
    uint8_t* p = bmpOut.data();

    // BMP Header (14 bytes)
    p[0] = 'B'; p[1] = 'M';
    *(uint32_t*)(p + 2) = fileSize;
    *(uint32_t*)(p + 6) = 0;
    *(uint32_t*)(p + 10) = 54; // offset to pixel data

    // DIB Header (40 bytes)
    *(uint32_t*)(p + 14) = 40;
    *(int32_t*)(p + 18) = outWidth;
    *(int32_t*)(p + 22) = -static_cast<int32_t>(outHeight); // Top-down BMP
    *(uint16_t*)(p + 26) = 1;
    *(uint16_t*)(p + 28) = 24; // 24-bit BGR
    *(uint32_t*)(p + 30) = 0; // BI_RGB
    *(uint32_t*)(p + 34) = imageSize;
    *(int32_t*)(p + 38) = 2835;
    *(int32_t*)(p + 42) = 2835;
    *(uint32_t*)(p + 46) = 0;
    *(uint32_t*)(p + 50) = 0;

    uint8_t* dst = p + 54;
    const uint8_t* yPlane = nv12;

    // Media Foundation aligns 1080p to 1088 lines for macroblocks
    size_t yPlaneHeight = height;
    if (dataSize >= width * 1088 * 3 / 2) {
        yPlaneHeight = 1088;
    }
    const uint8_t* uvPlane = nv12 + (width * yPlaneHeight);

    for (uint32_t y = 0; y < outHeight; ++y) {
        uint8_t* dstRow = dst + y * rowSize;

        for (uint32_t x = 0; x < outWidth; ++x) {
            uint32_t srcX = 0;
            uint32_t srcY = 0;

            if (rotation == 90) {
                // Vertical (Portrait upright): rotate 90 deg clockwise
                srcX = y * 2;
                srcY = (outWidth - 1 - x) * 2;
            } else if (rotation == 270) {
                // Vertical (Portrait inverted): rotate 270 deg
                srcX = (outHeight - 1 - y) * 2;
                srcY = x * 2;
            } else if (rotation == 180) {
                // Horizontal (Landscape inverted): rotate 180 deg
                srcX = (outWidth - 1 - x) * 2;
                srcY = (outHeight - 1 - y) * 2;
            } else {
                // Horizontal (Landscape standard): 0 deg
                srcX = x * 2;
                srcY = y * 2;
            }

            if (srcX >= width) srcX = width - 1;
            if (srcY >= height) srcY = height - 1;

            const uint8_t* yRow = yPlane + srcY * width;
            int yVal = yRow[srcX];

            size_t uvOffset = (srcY / 2) * width + (srcX & ~1);
            size_t maxUv = (width * yPlaneHeight) / 2;
            int uVal = 0, vVal = 0;
            if (uvOffset + 1 < maxUv) {
                uVal = uvPlane[uvOffset] - 128;
                vVal = uvPlane[uvOffset + 1] - 128;
            }

            int r = yVal + ((359 * vVal) >> 8);
            int g = yVal - ((88 * uVal + 183 * vVal) >> 8);
            int b = yVal + ((454 * uVal) >> 8);

            dstRow[x * 3 + 0] = static_cast<uint8_t>(b < 0 ? 0 : (b > 255 ? 255 : b));
            dstRow[x * 3 + 1] = static_cast<uint8_t>(g < 0 ? 0 : (g > 255 ? 255 : g));
            dstRow[x * 3 + 2] = static_cast<uint8_t>(r < 0 ? 0 : (r > 255 ? 255 : r));
        }
    }
}

static std::vector<std::string> GetLocalIPv4Addresses() {
    std::vector<std::string> ips;
    ULONG outBufLen = 15000;
    PIP_ADAPTER_ADDRESSES pAddresses = (IP_ADAPTER_ADDRESSES*)malloc(outBufLen);
    if (!pAddresses) return ips;

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
                if (ip != "127.0.0.1") ips.push_back(ip);
            }
        }
    }
    if (pAddresses) free(pAddresses);
    return ips;
}

HttpControlBridge::HttpControlBridge(TcpReceiver& receiver)
    : m_receiver(receiver)
    , m_port(8090)
    , m_listenSocket(INVALID_SOCKET)
    , m_isRunning(false)
    , m_activeDeviceId(1) {
    m_status.connected = false;
    m_status.usbConnected = false;
    m_status.activeDeviceId = 1;
    m_status.deviceName = "Desconectado";
    m_status.width = 1920;
    m_status.height = 1080;
    m_status.isVertical = false;
    m_status.fps = 0.0f;
    m_status.latencyMs = 0.0f;
    m_status.bitrateKbps = 0;
    m_status.localIps = GetLocalIPv4Addresses();
}

HttpControlBridge::~HttpControlBridge() {
    Stop();
}

bool HttpControlBridge::Start(uint16_t port) {
    if (m_isRunning.load()) return true;

    m_port = port;
    m_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listenSocket == INVALID_SOCKET) return false;

    int opt = 1;
    setsockopt(m_listenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(m_port);

    if (bind(m_listenSocket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
        return false;
    }

    if (listen(m_listenSocket, 32) == SOCKET_ERROR) {
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
        return false;
    }

    m_isRunning.store(true);
    m_serverThread = std::thread(&HttpControlBridge::ServerWorker, this);

    std::cout << "[HttpControlBridge] HTTP Control Server listening on http://127.0.0.1:" << m_port << std::endl;
    return true;
}

void HttpControlBridge::Stop() {
    m_isRunning.store(false);

    if (m_listenSocket != INVALID_SOCKET) {
        closesocket(m_listenSocket);
        m_listenSocket = INVALID_SOCKET;
    }

    {
        std::lock_guard<std::mutex> lock(m_audioMutex);
        for (auto& pair : m_audioClients) {
            for (SOCKET s : pair.second) {
                closesocket(s);
            }
        }
        m_audioClients.clear();
    }

    if (m_serverThread.joinable()) {
        m_serverThread.join();
    }
}

void HttpControlBridge::SetUsbStatus(bool connected) {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    m_status.usbConnected = connected;
}

void HttpControlBridge::SetDeviceMetadata(int deviceId, const std::string& name, uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    DeviceInfo& dev = m_devices[deviceId];
    dev.id = deviceId;
    dev.name = name;
    dev.width = width;
    dev.height = height;

    if (deviceId == m_activeDeviceId.load() || !m_status.connected) {
        m_status.connected = true;
        m_status.activeDeviceId = deviceId;
        m_status.deviceName = name;
        m_status.width = width;
        m_status.height = height;
    }
}

void HttpControlBridge::SetDeviceDimState(int deviceId, bool isDimmed) {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    if (m_devices.find(deviceId) != m_devices.end()) {
        m_devices[deviceId].isDimmed = isDimmed;
    }
    if (deviceId == m_activeDeviceId.load() || m_devices.size() <= 1) {
        m_status.isDimmed = isDimmed;
    }
}

void HttpControlBridge::RemoveDevice(int deviceId) {
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_devices.erase(deviceId);
        if (m_devices.empty()) {
            m_status.connected = false;
            m_status.deviceName = "Desconectado";
        } else if (m_activeDeviceId.load() == deviceId) {
            m_activeDeviceId.store(m_devices.begin()->first);
            const auto& nextDev = m_devices.begin()->second;
            m_status.deviceName = nextDev.name;
            m_status.width = nextDev.width;
            m_status.height = nextDev.height;
            m_status.isVertical = nextDev.isVertical;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        m_deviceBmpFrames.erase(deviceId);
    }
    {
        std::lock_guard<std::mutex> lock(m_audioMutex);
        auto it = m_audioClients.find(deviceId);
        if (it != m_audioClients.end()) {
            for (SOCKET s : it->second) {
                closesocket(s);
            }
            m_audioClients.erase(it);
        }
    }
}

void HttpControlBridge::PushAudioData(int deviceId, const uint8_t* pcmData, uint32_t size) {
    if (!pcmData || size == 0) return;
    std::lock_guard<std::mutex> lock(m_audioMutex);
    auto it = m_audioClients.find(deviceId);
    if (it != m_audioClients.end()) {
        auto& clients = it->second;
        for (auto clientIt = clients.begin(); clientIt != clients.end(); ) {
            int sent = send(*clientIt, (const char*)pcmData, static_cast<int>(size), 0);
            if (sent <= 0) {
                closesocket(*clientIt);
                clientIt = clients.erase(clientIt);
            } else {
                ++clientIt;
            }
        }
    }
}

void HttpControlBridge::SetDeviceTransform(int deviceId, bool isMirrored, int rotation) {
    std::lock_guard<std::mutex> lock(m_transformMutex);
    m_deviceTransforms[deviceId] = { isMirrored, rotation };
}

HttpControlBridge::DeviceTransform HttpControlBridge::GetDeviceTransform(int deviceId) {
    std::lock_guard<std::mutex> lock(m_transformMutex);
    auto it = m_deviceTransforms.find(deviceId);
    if (it != m_deviceTransforms.end()) {
        return it->second;
    }
    return DeviceTransform{};
}

void HttpControlBridge::UpdateStats(int deviceId, float fps, float latencyMs, uint32_t bitrateKbps) {
    std::lock_guard<std::mutex> lock(m_statusMutex);
    if (m_devices.find(deviceId) != m_devices.end()) {
        DeviceInfo& dev = m_devices[deviceId];
        dev.fps = fps;
        dev.latencyMs = latencyMs;
        dev.bitrateKbps = bitrateKbps;
    }

    if (deviceId == m_activeDeviceId.load() || m_devices.size() <= 1) {
        m_status.fps = fps;
        m_status.latencyMs = latencyMs;
        m_status.bitrateKbps = bitrateKbps;
    }
}

void HttpControlBridge::UpdateDecodedFrame(int deviceId, const uint8_t* pDecodedData, uint32_t dataSize, uint32_t width, uint32_t height, BouleCamPixelFormat pixelFormat, uint16_t rotation) {
    if (!pDecodedData || dataSize == 0 || width == 0 || height == 0) return;

    std::vector<uint8_t> bmp;
    if (pixelFormat == BOULECAM_PIXFMT_NV12) {
        ConvertNV12ToBMP(pDecodedData, width, height, bmp, rotation, dataSize);
    }

    if (bmp.empty()) return;

    bool isVertical = (rotation == 90 || rotation == 270);
    uint32_t finalWidth = isVertical ? (height) : (width);
    uint32_t finalHeight = isVertical ? (width) : (height);

    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        if (m_devices.find(deviceId) != m_devices.end()) {
            m_devices[deviceId].width = finalWidth;
            m_devices[deviceId].height = finalHeight;
            m_devices[deviceId].isVertical = isVertical;
        }
        if (deviceId == m_activeDeviceId.load() || !m_status.connected) {
            m_status.connected = true;
            m_status.width = finalWidth;
            m_status.height = finalHeight;
            m_status.isVertical = isVertical;
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        m_deviceBmpFrames[deviceId] = bmp;
        if (deviceId == m_activeDeviceId.load() || m_latestBmpFrame.empty()) {
            m_latestBmpFrame = bmp;
        }
    }
}

void HttpControlBridge::ServerWorker() {
    while (m_isRunning.load()) {
        sockaddr_in clientAddr{};
        int clientLen = sizeof(clientAddr);
        SOCKET clientSock = accept(m_listenSocket, (sockaddr*)&clientAddr, &clientLen);

        if (clientSock == INVALID_SOCKET) {
            if (!m_isRunning.load()) break;
            continue;
        }

        // Set socket timeouts to prevent hanging client sockets
        DWORD timeout = 800;
        setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        setsockopt(clientSock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

        std::thread(&HttpControlBridge::HandleClient, this, clientSock).detach();
    }
}

void HttpControlBridge::HandleClient(SOCKET clientSock) {
    char buffer[4096];
    int bytes = recv(clientSock, buffer, sizeof(buffer) - 1, 0);
    if (bytes <= 0) {
        closesocket(clientSock);
        return;
    }

    buffer[bytes] = '\0';
    std::string requestStr(buffer);

    // Parse path
    std::string firstLine;
    size_t lineEnd = requestStr.find("\r\n");
    if (lineEnd != std::string::npos) firstLine = requestStr.substr(0, lineEnd);
    else firstLine = requestStr;

    // Extract device/cam query parameter (?cam=X or /obs/X or /api/snapshot/X)
    int requestedCamId = m_activeDeviceId.load();
    size_t camParam = requestStr.find("cam=");
    if (camParam != std::string::npos) {
        size_t endP = requestStr.find_first_of("& \r\n", camParam + 4);
        std::string numStr = requestStr.substr(camParam + 4, endP - (camParam + 4));
        try { requestedCamId = std::stoi(numStr); } catch (...) {}
    } else {
        // Check path like /obs/1 or /obs/2
        size_t obsSlash = firstLine.find("GET /obs/");
        if (obsSlash != std::string::npos) {
            size_t start = obsSlash + 9;
            size_t endP = firstLine.find_first_of("? \r\n", start);
            try { requestedCamId = std::stoi(firstLine.substr(start, endP - start)); } catch (...) {}
        }
    }

    // 0. OBS Studio Dedicated Stream per camera (/obs, /obs/1, /obs/2, /obs?cam=X)
    if (firstLine.find("GET /obs") != std::string::npos || firstLine.find("GET /stream") != std::string::npos) {
        std::string camParamStr = std::to_string(requestedCamId);
        std::string html = 
            "<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<title>BouleCam Stream - Cam " + camParamStr + "</title>"
            "<style>html,body{margin:0;padding:0;width:100%;height:100%;overflow:hidden;background:#000;}"
            "#v{width:100%;height:100%;object-fit:contain;display:block;transition:transform 0.15s ease;}</style></head>"
            "<body><img id='v' src='/api/snapshot?cam=" + camParamStr + "' alt='Stream'>"
            "<audio id='snd' src='/api/audio?cam=" + camParamStr + "' autoplay playsinline></audio>"
            "<script>"
            "const v = document.getElementById('v');"
            "const snd = document.getElementById('snd');"
            "if(snd){ snd.volume = 1.0; const playA = () => { snd.play().catch(()=>{}); }; playA(); window.addEventListener('click', playA); }"
            "const params = new URLSearchParams(window.location.search);"
            "let rot = parseInt(params.get('rot') || '0', 10);"
            "let mirror = params.get('mirror') === '1';"
            "function applyT(){"
            "  v.style.transform = (mirror ? 'scaleX(-1) ' : '') + (rot ? 'rotate(' + rot + 'deg)' : '');"
            "}"
            "applyT();"
            "let lastMirror = mirror;"
            "let lastRot = rot;"
            "async function syncLiveTransform(){"
            "  try {"
            "    const r = await fetch('/api/cam_transform?cam=" + camParamStr + "');"
            "    if(r.ok){"
            "      const d = await r.json();"
            "      if(d.mirror !== lastMirror || d.rot !== lastRot){"
            "        lastMirror = d.mirror;"
            "        lastRot = d.rot;"
            "        mirror = d.mirror;"
            "        rot = d.rot;"
            "        applyT();"
            "      }"
            "    }"
            "  }catch(e){}"
            "}"
            "setInterval(syncLiveTransform, 300);"
            "syncLiveTransform();"
            "window.addEventListener('dblclick', async () => {"
            "  mirror = !mirror;"
            "  applyT();"
            "  try {"
            "    await fetch('/api/cam_transform?cam=" + camParamStr + "&mirror=' + (mirror ? '1' : '0') + '&rot=' + rot, {method:'POST'});"
            "  }catch(e){}"
            "});"
            "let curUrl = null;"
            "async function loop(){"
            "  try {"
            "    const r = await fetch('/api/snapshot?cam=" + camParamStr + "');"
            "    if(r.ok){"
            "      const b = await r.blob();"
            "      if(b.size > 1000){"
            "        const nu = URL.createObjectURL(b);"
            "        v.src = nu;"
            "        if(curUrl) URL.revokeObjectURL(curUrl);"
            "        curUrl = nu;"
            "      }"
            "    }"
            "  }catch(e){}"
            "  requestAnimationFrame(loop);"
            "}"
            "requestAnimationFrame(loop);"
            "</script></body></html>";

        std::ostringstream oss;
        oss << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: text/html\r\n"
            << "Content-Length: " << html.size() << "\r\n"
            << "Access-Control-Allow-Origin: *\r\n"
            << "Connection: close\r\n\r\n"
            << html;
        std::string resp = oss.str();
        send(clientSock, resp.c_str(), static_cast<int>(resp.size()), 0);
        closesocket(clientSock);
        return;
    }

    // 0.1 Audio Live Stream endpoint (/api/audio, /api/audio?cam=X)
    if (firstLine.find("GET /api/audio") != std::string::npos) {
        #pragma pack(push, 1)
        struct WavStreamHeader {
            char riff[4] = {'R', 'I', 'F', 'F'};
            uint32_t fileSize = 0x7FFFFFFF;
            char wave[4] = {'W', 'A', 'V', 'E'};
            char fmt[4] = {'f', 'm', 't', ' '};
            uint32_t fmtSize = 16;
            uint16_t audioFormat = 1; // PCM
            uint16_t numChannels = 1; // Mono
            uint32_t sampleRate = 48000;
            uint32_t byteRate = 48000 * 1 * 2; // 96000
            uint16_t blockAlign = 2; // 1 * 16/8
            uint16_t bitsPerSample = 16;
            char data[4] = {'d', 'a', 't', 'a'};
            uint32_t dataSize = 0x7FFFFFFF;
        } wavHeader;
        #pragma pack(pop)

        std::ostringstream oss;
        oss << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: audio/wav\r\n"
            << "Access-Control-Allow-Origin: *\r\n"
            << "Cache-Control: no-cache, no-store\r\n"
            << "Connection: close\r\n\r\n";
        std::string resp = oss.str();
        send(clientSock, resp.c_str(), static_cast<int>(resp.size()), 0);
        send(clientSock, (const char*)&wavHeader, sizeof(wavHeader), 0);

        {
            std::lock_guard<std::mutex> lock(m_audioMutex);
            m_audioClients[requestedCamId].push_back(clientSock);
        }
        return;
    }

    // 0.5 Camera Live Transform Sync API (/api/cam_transform)
    if (firstLine.find("/api/cam_transform") != std::string::npos) {
        if (firstLine.find("POST") != std::string::npos) {
            bool mirror = false;
            int rot = 0;
            if (firstLine.find("mirror=1") != std::string::npos || firstLine.find("mirror=true") != std::string::npos) {
                mirror = true;
            }
            size_t rotPos = firstLine.find("rot=");
            if (rotPos != std::string::npos) {
                rot = std::atoi(firstLine.c_str() + rotPos + 4);
            }
            SetDeviceTransform(requestedCamId, mirror, rot);
        }
        auto transform = GetDeviceTransform(requestedCamId);
        std::ostringstream json;
        json << "{\"cam\":" << requestedCamId 
             << ",\"mirror\":" << (transform.isMirrored ? "true" : "false")
             << ",\"rot\":" << transform.rotation << "}";
        std::string body = json.str();
        std::ostringstream oss;
        oss << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: application/json\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Access-Control-Allow-Origin: *\r\n"
            << "Connection: close\r\n\r\n"
            << body;
        std::string resp = oss.str();
        send(clientSock, resp.c_str(), static_cast<int>(resp.size()), 0);
        closesocket(clientSock);
        return;
    }

    // 1. Snapshot Live Video Feed endpoint (/api/snapshot, /api/snapshot?cam=X)
    if (firstLine.find("GET /api/snapshot") != std::string::npos) {
        std::vector<uint8_t> frameData;
        {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            if (m_deviceBmpFrames.find(requestedCamId) != m_deviceBmpFrames.end()) {
                frameData = m_deviceBmpFrames[requestedCamId];
            } else if (!m_latestBmpFrame.empty()) {
                frameData = m_latestBmpFrame;
            }
        }

        if (!frameData.empty()) {
            std::ostringstream oss;
            oss << "HTTP/1.1 200 OK\r\n"
                << "Content-Type: image/bmp\r\n"
                << "Content-Length: " << frameData.size() << "\r\n"
                << "Access-Control-Allow-Origin: *\r\n"
                << "Cache-Control: no-cache, no-store, must-revalidate\r\n"
                << "Connection: close\r\n\r\n";
            std::string header = oss.str();
            send(clientSock, header.c_str(), static_cast<int>(header.size()), 0);
            send(clientSock, (const char*)frameData.data(), static_cast<int>(frameData.size()), 0);
        } else {
            std::string emptyResp = "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n";
            send(clientSock, emptyResp.c_str(), static_cast<int>(emptyResp.size()), 0);
        }
        closesocket(clientSock);
        return;
    }

    // 2. Status API (returns status + all connected devices list)
    if (firstLine.find("GET /api/status") != std::string::npos) {
        std::ostringstream json;
        {
            std::lock_guard<std::mutex> lock(m_statusMutex);
            int activeId = m_activeDeviceId.load();
            json << "{"
                 << "\"connected\":" << (m_status.connected ? "true" : "false") << ","
                 << "\"usbConnected\":" << (m_status.usbConnected ? "true" : "false") << ","
                 << "\"activeDeviceId\":" << activeId << ","
                 << "\"deviceName\":\"" << m_status.deviceName << "\","
                 << "\"width\":" << m_status.width << ","
                 << "\"height\":" << m_status.height << ","
                 << "\"isVertical\":" << (m_status.isVertical ? "true" : "false") << ","
                 << "\"isDimmed\":" << (m_status.isDimmed ? "true" : "false") << ","
                 << "\"fps\":" << m_status.fps << ","
                 << "\"latencyMs\":" << m_status.latencyMs << ","
                 << "\"bitrateKbps\":" << m_status.bitrateKbps << ","
                 << "\"localIps\":[";
            for (size_t i = 0; i < m_status.localIps.size(); ++i) {
                json << "\"" << m_status.localIps[i] << "\"";
                if (i + 1 < m_status.localIps.size()) json << ",";
            }
            json << "],\"devices\":[";

            size_t idx = 0;
            for (const auto& pair : m_devices) {
                const auto& d = pair.second;
                json << "{"
                     << "\"id\":" << d.id << ","
                     << "\"name\":\"" << d.name << "\","
                     << "\"width\":" << d.width << ","
                     << "\"height\":" << d.height << ","
                     << "\"isVertical\":" << (d.isVertical ? "true" : "false") << ","
                     << "\"isDimmed\":" << (d.isDimmed ? "true" : "false") << ","
                     << "\"fps\":" << d.fps << ","
                     << "\"latencyMs\":" << d.latencyMs << ","
                     << "\"bitrateKbps\":" << d.bitrateKbps << ","
                     << "\"obsUrl\":\"http://127.0.0.1:" << m_port << "/obs/" << d.id << "\""
                     << "}";
                if (++idx < m_devices.size()) json << ",";
            }
            json << "]}";
        }

        std::string body = json.str();
        std::ostringstream oss;
        oss << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: application/json\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Access-Control-Allow-Origin: *\r\n"
            << "Connection: close\r\n\r\n"
            << body;
        std::string resp = oss.str();
        send(clientSock, resp.c_str(), static_cast<int>(resp.size()), 0);
        closesocket(clientSock);
        return;
    }

    // 3. Select Camera API (/api/select_cam?cam=X)
    if (firstLine.find("POST /api/select_cam") != std::string::npos || firstLine.find("GET /api/select_cam") != std::string::npos) {
        if (requestedCamId > 0) {
            m_activeDeviceId.store(requestedCamId);
            std::lock_guard<std::mutex> lock(m_statusMutex);
            if (m_devices.find(requestedCamId) != m_devices.end()) {
                m_status.deviceName = m_devices[requestedCamId].name;
                m_status.width = m_devices[requestedCamId].width;
                m_status.height = m_devices[requestedCamId].height;
                m_status.isVertical = m_devices[requestedCamId].isVertical;
            }
        }
        std::string body = "{\"status\":\"ok\",\"activeDeviceId\":" + std::to_string(m_activeDeviceId.load()) + "}";
        std::ostringstream oss;
        oss << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: application/json\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Access-Control-Allow-Origin: *\r\n"
            << "Connection: close\r\n\r\n"
            << body;
        std::string resp = oss.str();
        send(clientSock, resp.c_str(), static_cast<int>(resp.size()), 0);
        closesocket(clientSock);
        return;
    }

    // 4. Command API (/api/command)
    if (firstLine.find("POST /api/command") != std::string::npos) {
        size_t bodyPos = requestStr.find("\r\n\r\n");
        if (bodyPos != std::string::npos) {
            std::string body = requestStr.substr(bodyPos + 4);
            int action = 0, intParam = 0, targetDevId = 0;
            long long longParam = 0;
            float floatParam = 0.0f;

            auto parseVal = [&](const std::string& key) -> std::string {
                size_t p = body.find("\"" + key + "\":");
                if (p == std::string::npos) return "";
                p += key.size() + 3;
                while (p < body.size() && (body[p] == ' ' || body[p] == ':')) p++;
                size_t end = body.find_first_of(",}", p);
                if (end == std::string::npos) end = body.size();
                return body.substr(p, end - p);
            };

            std::string a = parseVal("action");
            std::string ip = parseVal("intParam");
            std::string lp = parseVal("longParam");
            std::string fp = parseVal("floatParam");
            std::string dev = parseVal("deviceId");

            if (!a.empty()) action = std::stoi(a);
            if (!ip.empty()) intParam = std::stoi(ip);
            if (!lp.empty()) longParam = std::stoll(lp);
            if (!fp.empty()) floatParam = std::stof(fp);
            if (!dev.empty()) targetDevId = std::stoi(dev);
            else targetDevId = m_activeDeviceId.load();

            BouleCamCameraCmd cmd{};
            cmd.magic = BOULECAM_MAGIC;
            cmd.packet_type = BOULECAM_PKT_CAMERA_CMD;
            cmd.action = static_cast<uint8_t>(action);
            cmd.int_param1 = intParam;
            cmd.long_param1 = longParam;
            cmd.float_param1 = floatParam;

            bool ok = m_receiver.SendCameraCommand(cmd, targetDevId);
            if (!ok) {
                ok = m_receiver.SendCameraCommand(cmd, 0); // fallback to all connected clients
            }
            if (ok && action == 10) {
                SetDeviceDimState(targetDevId, intParam != 0);
            }

            std::string respBody = ok ? "{\"status\":\"ok\"}" : "{\"status\":\"failed\"}";
            std::ostringstream oss;
            oss << "HTTP/1.1 200 OK\r\n"
                << "Content-Type: application/json\r\n"
                << "Content-Length: " << respBody.size() << "\r\n"
                << "Access-Control-Allow-Origin: *\r\n"
                << "Connection: close\r\n\r\n"
                << respBody;
            std::string resp = oss.str();
            send(clientSock, resp.c_str(), static_cast<int>(resp.size()), 0);
            closesocket(clientSock);
            return;
        }
    }

    // Default 404
    std::string notFound = "HTTP/1.1 404 Not Found\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    send(clientSock, notFound.c_str(), static_cast<int>(notFound.size()), 0);
    closesocket(clientSock);
}

} // namespace boulecam
