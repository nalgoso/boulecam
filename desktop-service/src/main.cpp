#include "network/tcp_receiver.h"
#include "network/http_bridge.h"
#include "network/discovery_beacon.h"
#include "ipc/shm_producer.h"
#include "usb/adb_manager.h"
#include "usb/usbmuxd_client.h"
#include "decoder/video_decoder.h"
#include "audio/audio_output_manager.h"
#include <iostream>
#include <fstream>
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

    // 1. Initialize Shared Memory Producers map (Zero-Copy Triple-Buffering per device)
    std::map<int, std::unique_ptr<ShmProducer>> shmProducers;

    // Helper to get or create a ShmProducer for a specific camera slot (deviceId 1..3)
    auto getProducer = [&](int deviceId) -> ShmProducer* {
        auto it = shmProducers.find(deviceId);
        if (it != shmProducers.end()) return it->second.get();

        // Cap to 3 simultaneous cameras; deviceId is used directly as camIndex
        int camIndex = (deviceId >= 1 && deviceId <= 3) ? deviceId : 1;

        auto prod = std::make_unique<ShmProducer>();
        if (!prod->Initialize(camIndex)) {
            std::cerr << "[Fatal] Could not create Shared Memory IPC for device " << deviceId << "." << std::endl;
            return nullptr;
        }
        ShmProducer* pRaw = prod.get();
        shmProducers[deviceId] = std::move(prod);
        return pRaw;
    };

    // 2. Start TCP Low Latency Receiver, HTTP Control Bridge & UDP Auto-Discovery Beacon
    TcpReceiver tcpReceiver;

    // Check version cache or clear-cache argument on startup
    bool clearCacheRequested = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--clear-cache") {
            clearCacheRequested = true;
        }
    }

    char appDataPath[MAX_PATH];
    if (GetEnvironmentVariableA("APPDATA", appDataPath, MAX_PATH) > 0) {
        std::string versionFile = std::string(appDataPath) + "\\BouleCam\\version.txt";
        std::string currentVersion = "1.4.08";
        std::string savedVersion = "";
        std::ifstream vf(versionFile);
        if (vf.is_open()) {
            std::getline(vf, savedVersion);
            vf.close();
        }
        if (clearCacheRequested || savedVersion != currentVersion) {
            std::cout << "[Service] Version upgrade (" << currentVersion << ") or cache wipe requested. Clearing previous locks and blacklists..." << std::endl;
            tcpReceiver.ClearAllSlotLocks();
            tcpReceiver.ClearIgnoredClients();
            std::ofstream outVf(versionFile);
            if (outVf.is_open()) {
                outVf << currentVersion;
                outVf.close();
            }
        }
    }

    DiscoveryBeacon discoveryBeacon(BOULECAM_DEFAULT_TCP_PORT, BOULECAM_DEFAULT_UDP_PORT);
    discoveryBeacon.Start();

    HttpControlBridge httpBridge(tcpReceiver);
    AudioOutputManager audioOutput;
    audioOutput.Initialize(48000, 1, 16);
    httpBridge.SetAudioOutputManager(&audioOutput);
    httpBridge.Start(BOULECAM_DEFAULT_WS_PORT); // Port 8090 for Desktop App GUI

    // Wire up mic mute/gain commands to SHM so the virtual-camera DLL applies them in real-time
    httpBridge.SetMicMuteCallback([&shmProducers](int deviceId, bool muted) {
        auto it = shmProducers.find(deviceId);
        if (it != shmProducers.end() && it->second) {
            it->second->SetAudioMuted(muted);
        }
    });
    httpBridge.SetMicGainCallback([&shmProducers](int deviceId, float gainDb) {
        auto it = shmProducers.find(deviceId);
        if (it != shmProducers.end() && it->second) {
            it->second->SetAudioGainDb(gainDb);
        }
    });

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

    httpBridge.SetRescanCallback([&adbManager, &httpBridge, &discoveryBeacon, &tcpReceiver]() {
        std::cout << "[Service] Rescan triggered. Refreshing USB ADB reverse forwarding, clearing locks and sending Wi-Fi beacons..." << std::endl;
        if (AdbManager::IsAdbInstalled()) {
            bool usbOk = AdbManager::ExecuteAdbReverse(BOULECAM_DEFAULT_TCP_PORT, BOULECAM_DEFAULT_TCP_PORT);
            httpBridge.SetUsbStatus(usbOk);
        }
        tcpReceiver.ClearIgnoredClients();
        tcpReceiver.ClearAllSlotLocks();
        discoveryBeacon.TriggerBroadcast();
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
        [&decoders, &deviceRotations, &deviceFrameCounts, &deviceByteCounts, &decodersMutex, &getProducer, &httpBridge, &deviceLatencies](
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
                        [deviceId, &getProducer, &httpBridge, &decodersMutex, &deviceRotations, &deviceLatencies](
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

                            // Write to Shared Memory — all active cameras get their own SHM slot
                            ShmProducer* prod = getProducer(deviceId);
                            if (prod) {
                                bool isVert = (rot == 90 || rot == 270);
                                uint32_t vWidth  = isVert ? height : width;
                                uint32_t vHeight = isVert ? width  : height;
                                prod->WriteFrame(
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
        [&getProducer, &httpBridge, &tcpReceiver](int deviceId, const BouleCamHandshakeReq& handshake) {
            std::string clientIp = tcpReceiver.GetClientIp(deviceId);
            std::string uniqueId = tcpReceiver.GetClientUniqueId(deviceId);
            std::string deviceName = tcpReceiver.GetClientDeviceName(deviceId);
            bool isUsb = (clientIp == "127.0.0.1");
            std::cout << "[Stream] Connected [Device " << deviceId << "]: " << deviceName 
                      << " (ID: " << uniqueId << ")"
                      << " (" << (isUsb ? "USB Cable" : ("Wi-Fi " + clientIp)) << ")"
                      << " Resolution: " << handshake.width << "x" << handshake.height 
                      << " FPS: " << handshake.target_fps << std::endl;
            
            ShmProducer* prod = getProducer(deviceId);
            if (prod) {
                prod->UpdateFormat(handshake.width, handshake.height, handshake.target_fps);
                prod->SetStreamingActive(true);
            }
            httpBridge.SetDeviceMetadata(deviceId, deviceName, handshake.width, handshake.height, clientIp, isUsb, uniqueId);

            // Inform mobile client of PC's primary Wi-Fi IPv4 address so it can switch or reconnect seamlessly
            std::string pcWifiIp = "";
            {
                auto status = httpBridge.GetStatus();
                for (const auto& ip : status.localIps) {
                    if (ip != "127.0.0.1" && ip.find("169.254.") != 0) {
                        pcWifiIp = ip;
                        break;
                    }
                }
            }
            if (!pcWifiIp.empty()) {
                BouleCamCameraCmd cmd{};
                cmd.magic = BOULECAM_MAGIC;
                cmd.action = BOULECAM_ACTION_SET_CONN_MODE;
                cmd.int_param1 = isUsb ? 1 : 0;
                in_addr addr{};
                inet_pton(AF_INET, pcWifiIp.c_str(), &addr);
                cmd.long_param1 = static_cast<int64_t>(static_cast<uint32_t>(addr.s_addr));
                cmd.float_param1 = isUsb ? 1.0f : 0.0f;
                tcpReceiver.SendCameraCommand(cmd, deviceId);
            }
        },
        [&httpBridge](int deviceId, const BouleCamCameraState& state) {
            httpBridge.SetDeviceDimState(deviceId, state.dim_screen_active != 0);
            httpBridge.SetDeviceBatteryState(deviceId, state.battery_level, state.is_charging != 0);
        }
    );

    if (!serverStarted) {
        std::cerr << "[Fatal] Failed to bind TCP listener. Exiting." << std::endl;
        return 1;
    }

    tcpReceiver.SetDisconnectedCallback([&httpBridge, &shmProducers, &tcpReceiver](int deviceId) {
        if (tcpReceiver.IsClientConnected(deviceId)) {
            std::cout << "[Service] Device " << deviceId << " already reconnected with active socket. Skipping disconnect cleanup." << std::endl;
            return;
        }
        std::cout << "[Service] Device " << deviceId << " disconnected -> updating HTTP bridge." << std::endl;
        httpBridge.RemoveDevice(deviceId);
        auto it = shmProducers.find(deviceId);
        if (it != shmProducers.end() && it->second) {
            it->second->SetStreamingActive(false);
        }
    });

    tcpReceiver.SetAudioCallback([&shmProducers, &httpBridge, &audioOutput](
        int deviceId, const BouleCamAudioHeader& header, const uint8_t* payloadData, uint32_t payloadSize) {
        // Write raw PCM into the per-device SHM; virtual-camera DLL applies mute/gain in real-time
        auto it = shmProducers.find(deviceId);
        if (it != shmProducers.end() && it->second) {
            it->second->WriteAudio(payloadData, payloadSize, header.sample_rate, header.channels);
        }
        // Push to native Windows audio output device
        audioOutput.WriteAudio(payloadData, payloadSize);
        // Level metering for UI
        httpBridge.PushAudioData(deviceId, payloadData, payloadSize);
    });

    std::cout << "\n[Ready] BouleCam Desktop Service running. Waiting for mobile camera feed..." << std::endl;
    std::cout << "[Info] Connect your phone via Wi-Fi or USB Cable." << std::endl;
    std::cout << "[Info] Desktop UI Bridge: http://127.0.0.1:8090" << std::endl;
    std::cout << "[Info] Native Audio Device: " << audioOutput.GetDeviceName() << std::endl;
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
    audioOutput.Shutdown();
    for (auto& kv : shmProducers) {
        if (kv.second) kv.second->Shutdown();
    }

    {
        std::lock_guard<std::recursive_mutex> lock(decodersMutex);
        decoders.clear();
    }

    MFShutdown();
    CoUninitialize();

    std::cout << "[Service] Shutdown completed successfully." << std::endl;
    return 0;
}
