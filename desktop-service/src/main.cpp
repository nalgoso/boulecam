#include "network/tcp_receiver.h"
#include "network/http_bridge.h"
#include "network/discovery_beacon.h"
#include "ipc/shm_producer.h"
#include "usb/adb_manager.h"
#include "usb/usbmuxd_client.h"
#include "decoder/video_decoder.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <iomanip>
#include <csignal>
#include <map>
#include <memory>
#include <mutex>

using namespace boulecam;

static std::atomic<bool> g_keepRunning(true);

void SignalHandler(int signum) {
    std::cout << "\n[Service] Interruption signal (" << signum << ") received. Shutting down gracefully..." << std::endl;
    g_keepRunning.store(false);
}

int main(int argc, char* argv[]) {
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    // Initialize COM and Media Foundation
    HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    std::cout << "==========================================================" << std::endl;
    std::cout << "        BouleCam Desktop Service Daemon v1.0.0            " << std::endl;
    std::cout << "    Ultra Low Latency Mobile to Windows Webcam Bridge     " << std::endl;
    std::cout << "==========================================================" << std::endl;

    // 1. Initialize Shared Memory Producer (Zero-Copy Triple-Buffering)
    ShmProducer shmProducer;
    if (!shmProducer.Initialize()) {
        std::cerr << "[Fatal] Could not create Shared Memory IPC. Exiting." << std::endl;
        return 1;
    }

    // 2. Start TCP Low Latency Receiver, HTTP Control Bridge & UDP Auto-Discovery Beacon
    TcpReceiver tcpReceiver;
    DiscoveryBeacon discoveryBeacon(BOULECAM_DEFAULT_TCP_PORT, BOULECAM_DEFAULT_UDP_PORT);
    discoveryBeacon.Start();

    HttpControlBridge httpBridge(tcpReceiver);
    httpBridge.Start(BOULECAM_DEFAULT_WS_PORT); // Port 8090 for Desktop App GUI

    // Multi-Device decoders and statistics
    std::recursive_mutex decodersMutex;
    std::map<int, std::unique_ptr<VideoDecoder>> decoders;
    std::map<int, uint16_t> deviceRotations;
    std::map<int, uint64_t> deviceFrameCounts;
    std::map<int, double> deviceLatencies;

    // 3. Start ADB Reverse Daemon for Android USB Connections
    AdbManager adbManager;
    if (AdbManager::IsAdbInstalled()) {
        std::cout << "[USB] ADB detected. Enabling automatic USB reverse port forwarding for Android." << std::endl;
        adbManager.StartAutoReverse(BOULECAM_DEFAULT_TCP_PORT, BOULECAM_DEFAULT_TCP_PORT);
    } else {
        std::cout << "[USB] ADB not found in system PATH. Wi-Fi streaming and direct IP enabled." << std::endl;
    }

    httpBridge.SetRescanCallback([&adbManager, &httpBridge]() {
        std::cout << "[Service] Rescan triggered. Refreshing USB ADB reverse forwarding..." << std::endl;
        if (AdbManager::IsAdbInstalled()) {
            bool usbOk = AdbManager::ExecuteAdbReverse(BOULECAM_DEFAULT_TCP_PORT, BOULECAM_DEFAULT_TCP_PORT);
            httpBridge.SetUsbStatus(usbOk);
        }
    });

    // 4. Check Apple usbmuxd service for iOS USB Connections
    if (UsbmuxdClient::IsUsbmuxdRunning()) {
        std::cout << "[USB] Apple Mobile Device Service (usbmuxd) active. iOS USB cable mode ready." << std::endl;
    } else {
        std::cout << "[USB] usbmuxd not running. iOS devices will connect via Wi-Fi." << std::endl;
    }

    // 5. Start TCP Server with Multi-Device Frame Callback
    std::map<int, uint64_t> deviceByteCounts;
    std::map<int, uint64_t> prevByteCounts;

    bool serverStarted = tcpReceiver.Start(
        BOULECAM_DEFAULT_TCP_PORT,
        [&decoders, &deviceRotations, &deviceFrameCounts, &deviceByteCounts, &decodersMutex, &shmProducer, &httpBridge, &deviceLatencies](
            int deviceId, const BouleCamFrameHeader& header, const uint8_t* payloadData, uint32_t payloadSize) {
            
            uint64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();

            VideoDecoder* pDecoder = nullptr;
            {
                std::lock_guard<std::recursive_mutex> lock(decodersMutex);
                deviceRotations[deviceId] = header.rotation_degrees;
                deviceFrameCounts[deviceId]++;
                deviceByteCounts[deviceId] += (payloadSize + sizeof(header));

                // Lazy initialize decoder for this mobile device
                if (decoders.find(deviceId) == decoders.end()) {
                    decoders[deviceId] = std::make_unique<VideoDecoder>();
                    decoders[deviceId]->Initialize(1920, 1080, BOULECAM_CODEC_H264,
                        [deviceId, &shmProducer, &httpBridge, &decodersMutex, &deviceRotations, &deviceLatencies](
                            const uint8_t* pDecodedData, uint32_t dataSize, uint32_t width, uint32_t height,
                            uint32_t stride, BouleCamPixelFormat pixelFormat, uint64_t captureTs, uint64_t decodedTs) {
                            
                            uint64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();
                            double decodeDurationMs = (captureTs > 0 && nowUs > captureTs) 
                                ? static_cast<double>(nowUs - captureTs) / 1000.0 : 3.0;

                            uint16_t rot = 0;
                            {
                                std::lock_guard<std::recursive_mutex> lk(decodersMutex);
                                if (deviceRotations.find(deviceId) != deviceRotations.end()) {
                                    rot = deviceRotations[deviceId];
                                }
                                if (decodeDurationMs > 0.0 && decodeDurationMs < 100.0) {
                                    deviceLatencies[deviceId] = deviceLatencies[deviceId] * 0.8 + decodeDurationMs * 0.2;
                                }
                            }

                            // If primary active camera, also write to Shared Memory Virtual Camera
                            if (deviceId == 1 || httpBridge.GetActiveDeviceId() == deviceId) {
                                bool isVert = (rot == 90 || rot == 270);
                                uint32_t vWidth = isVert ? height : width;
                                uint32_t vHeight = isVert ? width : height;
                                shmProducer.WriteFrame(
                                    pDecodedData, dataSize, vWidth, vHeight, stride, pixelFormat, captureTs, decodedTs
                                );
                            }

                            // Feed live decoded frame to HTTP bridge for OBS and GUI preview
                            httpBridge.UpdateDecodedFrame(deviceId, pDecodedData, dataSize, width, height, pixelFormat, rot);
                        }
                    );
                }

                pDecoder = decoders[deviceId].get();
            }

            if (pDecoder) {
                pDecoder->DecodeNALU(payloadData, payloadSize, nowUs);
            }
        },
        [&shmProducer, &httpBridge, &tcpReceiver](int deviceId, const BouleCamHandshakeReq& handshake) {
            std::string clientIp = tcpReceiver.GetClientIp(deviceId);
            bool isUsb = (clientIp == "127.0.0.1");
            std::cout << "[Stream] Connected [Device " << deviceId << "]: " << handshake.device_name 
                      << " (" << (isUsb ? "USB Cable" : ("Wi-Fi " + clientIp)) << ")"
                      << " Resolution: " << handshake.width << "x" << handshake.height 
                      << " FPS: " << handshake.target_fps << std::endl;
            
            if (deviceId == 1 || httpBridge.GetActiveDeviceId() == deviceId) {
                shmProducer.UpdateFormat(handshake.width, handshake.height, handshake.target_fps);
                shmProducer.SetStreamingActive(true);
            }
            httpBridge.SetDeviceMetadata(deviceId, handshake.device_name, handshake.width, handshake.height, clientIp, isUsb);
        },
        [&httpBridge](int deviceId, const BouleCamCameraState& state) {
            httpBridge.SetDeviceDimState(deviceId, state.dim_screen_active != 0);
        }
    );

    if (!serverStarted) {
        std::cerr << "[Fatal] Failed to bind TCP listener. Exiting." << std::endl;
        return 1;
    }

    tcpReceiver.SetAudioCallback([&httpBridge](
        int deviceId, const BouleCamAudioHeader& header, const uint8_t* payloadData, uint32_t payloadSize) {
        httpBridge.PushAudioData(deviceId, payloadData, payloadSize);
    });

    std::cout << "\n[Ready] BouleCam Desktop Service running. Waiting for mobile camera feed..." << std::endl;
    std::cout << "[Info] Connect your phone via Wi-Fi or USB Cable." << std::endl;
    std::cout << "[Info] Desktop UI Bridge: http://127.0.0.1:8090" << std::endl;
    std::cout << "[Info] Dedicated OBS Streams: http://127.0.0.1:8090/obs/1 , /obs/2 ..." << std::endl;
    std::cout << "[Info] Press Ctrl+C to terminate.\n" << std::endl;

    // Main status monitor loop (aggregates per-camera statistics)
    std::map<int, uint64_t> prevCounts;
    while (g_keepRunning.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        httpBridge.SetUsbStatus(adbManager.IsDeviceConnected());

        {
            std::lock_guard<std::recursive_mutex> lock(decodersMutex);
            for (auto& pair : deviceFrameCounts) {
                int devId = pair.first;
                uint64_t curr = pair.second;
                uint64_t prev = prevCounts[devId];
                uint64_t fps = (curr >= prev) ? (curr - prev) : 0;
                prevCounts[devId] = curr;

                uint64_t currBytes = deviceByteCounts[devId];
                uint64_t prevBytes = prevByteCounts[devId];
                uint64_t bytesInSec = (currBytes >= prevBytes) ? (currBytes - prevBytes) : 0;
                prevByteCounts[devId] = currBytes;

                // Real bitrate calculated from exact bytes received in last second
                uint32_t realBitrateKbps = static_cast<uint32_t>((bytesInSec * 8) / 1000);

                // Real latency: Network RTT / 2 + decode duration
                double rttMs = tcpReceiver.GetClientRttMs(devId);
                double netLatency = (rttMs > 0.0) ? (rttMs / 2.0) : (tcpReceiver.GetClientIp(devId) == "127.0.0.1" ? 1.0 : 8.0);
                double decLat = (deviceLatencies.find(devId) != deviceLatencies.end()) ? deviceLatencies[devId] : 3.0;
                double totalRealLatencyMs = netLatency + decLat;

                httpBridge.UpdateStats(devId, static_cast<float>(fps), static_cast<float>(totalRealLatencyMs), realBitrateKbps);
            }
        }
    }

    // Graceful cleanup
    std::cout << "[Service] Cleaning up resources..." << std::endl;
    discoveryBeacon.Stop();
    httpBridge.Stop();
    tcpReceiver.Stop();
    shmProducer.Shutdown();

    {
        std::lock_guard<std::recursive_mutex> lock(decodersMutex);
        decoders.clear();
    }

    MFShutdown();
    CoUninitialize();

    std::cout << "[Service] Shutdown completed successfully." << std::endl;
    return 0;
}
