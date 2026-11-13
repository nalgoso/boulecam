#include "network/tcp_receiver.h"
#include "network/http_bridge.h"
#include "network/discovery_beacon.h"
#include "ipc/shm_producer.h"
#include "usb/adb_manager.h"
#include "usb/usbmuxd_client.h"
#include "decoder/video_decoder.h"
#include "audio/audio_output_manager.h"
#include "audio/audio_dsp.h"
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
    std::mutex shmMutex;
    std::map<int, std::unique_ptr<ShmProducer>> shmProducers;

    // Helper to get or create a ShmProducer for a specific camera slot (deviceId 1..4)
    auto getProducer = [&](int deviceId) -> ShmProducer* {
        if (deviceId < 1 || deviceId > 4) return nullptr;
        std::lock_guard<std::mutex> lock(shmMutex);
        auto it = shmProducers.find(deviceId);
        if (it != shmProducers.end()) return it->second.get();

        auto prod = std::make_unique<ShmProducer>();
        if (!prod->Initialize(deviceId)) {
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
        std::string currentVersion = "1.4.12";
        std::string savedVersion = "";
        std::ifstream vf(versionFile);
        if (vf.is_open()) {
            std::getline(vf, savedVersion);
            vf.close();
        }
        if (clearCacheRequested || savedVersion != currentVersion) {
            std::cout << "[Service] Version upgrade (" << currentVersion << ") or cache wipe requested. Clearing blacklists..." << std::endl;
            if (clearCacheRequested) {
                tcpReceiver.ClearAllSlotLocks();
            }
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
    httpBridge.SetDeviceNameCallback([&getProducer](int deviceId, const std::string& name) {
        ShmProducer* prod = getProducer(deviceId);
        if (prod) {
            prod->SetDeviceName(name);
        }
    });
    httpBridge.Start(BOULECAM_DEFAULT_WS_PORT); // Port 8095 for Desktop App GUI

    // Wire up mic mute/gain/mode commands to SHM and DSP engine
    std::mutex audioDspMutex;
    std::map<int, std::unique_ptr<AudioDspProcessor>> audioDspMap;

    httpBridge.SetMicMuteCallback([&getProducer](int deviceId, bool muted) {
        ShmProducer* prod = getProducer(deviceId);
        if (prod) {
            prod->SetAudioMuted(muted);
        }
    });
    httpBridge.SetMicGainCallback([&getProducer, &audioDspMap, &audioDspMutex](int deviceId, float gainDb) {
        ShmProducer* prod = getProducer(deviceId);
        if (prod) {
            prod->SetAudioGainDb(gainDb);
        }
        std::lock_guard<std::mutex> lock(audioDspMutex);
        auto& dsp = audioDspMap[deviceId];
        if (dsp) {
            dsp->SetUserGainDb(gainDb);
        }
    });
    httpBridge.SetAudioModeCallback([&audioDspMap, &audioDspMutex, &tcpReceiver](int deviceId, bool isAmbient) {
        {
            std::lock_guard<std::mutex> lock(audioDspMutex);
            auto& dsp = audioDspMap[deviceId];
            if (!dsp) {
                dsp = std::make_unique<AudioDspProcessor>();
            }
            dsp->SetAmbientMode(isAmbient);
        }
        std::cout << "[Audio DSP] Device " << deviceId << " modo: "
                  << (isAmbient ? "AMBIENTE (HPF Low-Cut 80Hz + -3dB)" : "PRINCIPAL (Respuesta Plana)") << std::endl;

        BouleCamCameraCmd cmd{};
        cmd.magic = BOULECAM_MAGIC;
        cmd.packet_type = BOULECAM_PKT_CAMERA_CMD;
        cmd.action = 13; // BOULECAM_ACTION_SET_AUDIO_MODE
        cmd.int_param1 = isAmbient ? 1 : 0;
        tcpReceiver.SendCameraCommand(cmd, deviceId);
    });

    // Multi-Device decoders and statistics (per-device mutexes allow parallel multi-camera H.264 decoding)
    std::recursive_mutex decodersMutex;
    std::map<int, std::shared_ptr<VideoDecoder>> decoders;
    std::map<int, std::shared_ptr<std::mutex>> deviceDecodeMutexes;
    std::map<int, std::pair<uint32_t, uint32_t>> deviceInitResolutions;
    std::map<int, uint16_t> deviceRotations;
    std::map<int, uint64_t> deviceFrameCounts;
    std::map<int, double> deviceLatencies;

    auto getDeviceDecodeMutex = [&](int devId) -> std::shared_ptr<std::mutex> {
        std::lock_guard<std::recursive_mutex> lock(decodersMutex);
        auto& mtx = deviceDecodeMutexes[devId];
        if (!mtx) mtx = std::make_shared<std::mutex>();
        return mtx;
    };

    auto resetDeviceDecoder = [&](int devId) {
        auto devMtx = getDeviceDecodeMutex(devId);
        std::lock_guard<std::mutex> devLock(*devMtx);
        std::lock_guard<std::recursive_mutex> lock(decodersMutex);
        decoders.erase(devId);
    };

    httpBridge.SetSwapCallback([&resetDeviceDecoder](int camA, int camB) {
        resetDeviceDecoder(camA);
        resetDeviceDecoder(camB);
    });

    // 3. Start ADB Reverse Daemon for Android USB Connections
    AdbManager adbManager;
    if (AdbManager::IsAdbInstalled()) {
        std::cout << "[USB] ADB detected. Enabling automatic USB reverse port forwarding for Android." << std::endl;
        adbManager.StartAutoReverse(BOULECAM_DEFAULT_TCP_PORT, BOULECAM_DEFAULT_TCP_PORT);
    } else {
        std::cout << "[USB] ADB not found in system PATH. Wi-Fi streaming and direct IP enabled." << std::endl;
    }

    httpBridge.SetRescanCallback([&adbManager, &httpBridge, &discoveryBeacon, &tcpReceiver]() {
        std::cout << "[Service] Rescan triggered. Refreshing USB ADB reverse forwarding, clearing ignored clients and triggering discovery sweep..." << std::endl;
        if (AdbManager::IsAdbInstalled()) {
            bool usbOk = AdbManager::ExecuteAdbReverse(BOULECAM_DEFAULT_TCP_PORT, BOULECAM_DEFAULT_TCP_PORT);
            httpBridge.SetUsbStatus(usbOk);
        }
        tcpReceiver.ClearIgnoredClients();
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
        [&decoders, &deviceInitResolutions, &deviceRotations, &deviceFrameCounts, &deviceByteCounts, &decodersMutex, &getDeviceDecodeMutex, &getProducer, &httpBridge, &tcpReceiver, &deviceLatencies](
            int deviceId, const BouleCamFrameHeader& header, const uint8_t* payloadData, uint32_t payloadSize) {
            
            uint64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();

            auto devMtx = getDeviceDecodeMutex(deviceId);
            std::lock_guard<std::mutex> devDecodeLock(*devMtx);

            std::shared_ptr<VideoDecoder> pDecoder;
            {
                std::lock_guard<std::recursive_mutex> lock(decodersMutex);
                deviceRotations[deviceId] = header.rotation_degrees;
                deviceFrameCounts[deviceId]++;
                deviceByteCounts[deviceId] += (payloadSize + sizeof(header));

                // Lazy initialize decoder for this mobile device slot
                if (decoders.find(deviceId) == decoders.end() || !decoders[deviceId]) {
                    uint32_t initW = 1920, initH = 1080;
                    auto resIt = deviceInitResolutions.find(deviceId);
                    if (resIt != deviceInitResolutions.end() && resIt->second.first > 0 && resIt->second.second > 0) {
                        initW = resIt->second.first;
                        initH = resIt->second.second;
                    }
                    auto newDec = std::make_shared<VideoDecoder>();
                    newDec->SetNeedKeyframeCallback([deviceId, &tcpReceiver, &deviceInitResolutions, &decodersMutex](uint32_t droppedFrames, bool missingSpsPps) {
                        if (droppedFrames == 1 || (droppedFrames % 15) == 0) {
                            BouleCamCameraCmd kfCmd{};
                            kfCmd.magic = BOULECAM_MAGIC;
                            kfCmd.packet_type = BOULECAM_PKT_CAMERA_CMD;
                            kfCmd.action = BOULECAM_ACTION_REQUEST_KEYFRAME;
                            tcpReceiver.SendCameraCommand(kfCmd, deviceId);

                            // Also send action 14 (Standby=0 / Wake) which triggers encoder.requestKeyFrame() on older APKs
                            BouleCamCameraCmd wakeCmd{};
                            wakeCmd.magic = BOULECAM_MAGIC;
                            wakeCmd.packet_type = BOULECAM_PKT_CAMERA_CMD;
                            wakeCmd.action = BOULECAM_ACTION_SET_STANDBY;
                            wakeCmd.int_param1 = 0;
                            tcpReceiver.SendCameraCommand(wakeCmd, deviceId);
                        }

                        // If a phone connected mid-stream on an older APK and hasn't sent SPS/PPS after 25 frames (~400ms),
                        // trigger a fast remote encoder restart so MediaCodec emits a fresh SPS + PPS + IDR header
                        if (missingSpsPps && (droppedFrames == 25 || droppedFrames == 90)) {
                            uint32_t targetW = 1920, targetH = 1080;
                            {
                                std::lock_guard<std::recursive_mutex> lk(decodersMutex);
                                auto it = deviceInitResolutions.find(deviceId);
                                if (it != deviceInitResolutions.end() && it->second.first > 0 && it->second.second > 0) {
                                    targetW = it->second.first;
                                    targetH = it->second.second;
                                }
                            }
                            std::cout << "[VideoDecoder] Device " << deviceId << " missing SPS/PPS after " << droppedFrames
                                      << " frames -> forcing remote MediaCodec SPS/PPS header refresh." << std::endl;
                            std::thread([deviceId, targetW, targetH, &tcpReceiver]() {
                                BouleCamCameraCmd bounceCmd{};
                                bounceCmd.magic = BOULECAM_MAGIC;
                                bounceCmd.packet_type = BOULECAM_PKT_CAMERA_CMD;
                                bounceCmd.action = BOULECAM_ACTION_SET_RESOLUTION;
                                bounceCmd.int_param1 = (targetW == 1920) ? 1280 : 1920;
                                bounceCmd.long_param1 = (targetW == 1920) ? 720 : 1080;
                                bounceCmd.float_param1 = 60.0f;
                                tcpReceiver.SendCameraCommand(bounceCmd, deviceId);

                                std::this_thread::sleep_for(std::chrono::milliseconds(120));

                                BouleCamCameraCmd restoreCmd{};
                                restoreCmd.magic = BOULECAM_MAGIC;
                                restoreCmd.packet_type = BOULECAM_PKT_CAMERA_CMD;
                                restoreCmd.action = BOULECAM_ACTION_SET_RESOLUTION;
                                restoreCmd.int_param1 = static_cast<int32_t>(targetW);
                                restoreCmd.long_param1 = static_cast<int64_t>(targetH);
                                restoreCmd.float_param1 = 60.0f;
                                tcpReceiver.SendCameraCommand(restoreCmd, deviceId);
                            }).detach();
                        }
                    });
                    newDec->Initialize(initW, initH, BOULECAM_CODEC_H264,
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

                            // Write to Shared Memory — all active cameras (1..4) get their own isolated SHM slot
                            ShmProducer* prod = getProducer(deviceId);
                            if (prod) {
                                bool isVert = (rot == 90 || rot == 270);
                                uint32_t vWidth  = isVert ? height : width;
                                uint32_t vHeight = isVert ? width  : height;
                                prod->WriteFrame(
                                    pDecodedData, dataSize, vWidth, vHeight, stride, pixelFormat, captureTs, decodedTs
                                );
                            }

                            // Feed live decoded frame to HTTP bridge for OBS, Projector and GUI preview
                            httpBridge.UpdateDecodedFrame(deviceId, pDecodedData, dataSize, width, height, pixelFormat, rot);
                        }
                    );
                    decoders[deviceId] = newDec;
                }

                pDecoder = decoders[deviceId];
            }

            if (pDecoder) {
                pDecoder->DecodeNALU(payloadData, payloadSize, nowUs);
            }
        },
        [&getProducer, &httpBridge, &tcpReceiver, &decoders, &decodersMutex, &deviceInitResolutions, &getDeviceDecodeMutex](int deviceId, const BouleCamHandshakeReq& handshake) {
            std::string clientIp = tcpReceiver.GetClientIp(deviceId);
            std::string uniqueId = tcpReceiver.GetClientUniqueId(deviceId);
            std::string deviceName = tcpReceiver.GetClientDeviceName(deviceId);
            bool isUsb = (clientIp == "127.0.0.1");
            std::cout << "[Stream] Connected [Device " << deviceId << "]: " << deviceName 
                      << " (ID: " << uniqueId << ")"
                      << " (" << (isUsb ? "USB Cable" : ("Wi-Fi " + clientIp)) << ")"
                      << " Resolution: " << handshake.width << "x" << handshake.height 
                      << " FPS: " << handshake.target_fps << std::endl;

            {
                auto devMtx = getDeviceDecodeMutex(deviceId);
                std::lock_guard<std::mutex> devLock(*devMtx);
                std::lock_guard<std::recursive_mutex> lock(decodersMutex);
                deviceInitResolutions[deviceId] = {
                    handshake.width > 0 ? handshake.width : 1920,
                    handshake.height > 0 ? handshake.height : 1080
                };
                auto itDec = decoders.find(deviceId);
                if (itDec != decoders.end() && itDec->second) {
                    itDec->second->ResetStreamSync();
                }
            }

            // Request immediate SPS/PPS + IDR keyframe from the newly connected phone (supports both new and older APKs)
            BouleCamCameraCmd kfCmd{};
            kfCmd.magic = BOULECAM_MAGIC;
            kfCmd.packet_type = BOULECAM_PKT_CAMERA_CMD;
            kfCmd.action = BOULECAM_ACTION_REQUEST_KEYFRAME;
            tcpReceiver.SendCameraCommand(kfCmd, deviceId);

            BouleCamCameraCmd wakeCmd{};
            wakeCmd.magic = BOULECAM_MAGIC;
            wakeCmd.packet_type = BOULECAM_PKT_CAMERA_CMD;
            wakeCmd.action = BOULECAM_ACTION_SET_STANDBY;
            wakeCmd.int_param1 = 0;
            tcpReceiver.SendCameraCommand(wakeCmd, deviceId);
            
            ShmProducer* prod = getProducer(deviceId);
            if (prod) {
                prod->UpdateFormat(handshake.width, handshake.height, handshake.target_fps);
                prod->SetStreamingActive(true);
            }
            httpBridge.SetDeviceMetadata(deviceId, deviceName, handshake.width, handshake.height, clientIp, isUsb, uniqueId);

            // For USB clients, inform mobile client of PC's primary Wi-Fi IPv4 address as a fallback.
            // Never force Wi-Fi clients to switch hosts when they are already actively connected.
            if (isUsb) {
                std::string pcWifiIp = "";
                auto status = httpBridge.GetStatus();
                for (const auto& ip : status.localIps) {
                    if (ip != "127.0.0.1" && ip.find("169.254.") != 0) {
                        pcWifiIp = ip;
                        break;
                    }
                }
                if (!pcWifiIp.empty()) {
                    BouleCamCameraCmd cmd{};
                    cmd.magic = BOULECAM_MAGIC;
                    cmd.action = BOULECAM_ACTION_SET_CONN_MODE;
                    cmd.int_param1 = 1; // USB
                    in_addr addr{};
                    inet_pton(AF_INET, pcWifiIp.c_str(), &addr);
                    cmd.long_param1 = static_cast<int64_t>(static_cast<uint32_t>(addr.s_addr));
                    cmd.float_param1 = 1.0f;
                    tcpReceiver.SendCameraCommand(cmd, deviceId);
                }
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

    tcpReceiver.SetDisconnectedCallback([&httpBridge, &getProducer, &tcpReceiver, &decoders, &decodersMutex, &getDeviceDecodeMutex](int deviceId) {
        if (tcpReceiver.IsClientConnected(deviceId)) {
            std::cout << "[Service] Device " << deviceId << " already reconnected with active socket. Skipping disconnect cleanup." << std::endl;
            return;
        }
        std::cout << "[Service] Device " << deviceId << " disconnected -> updating HTTP bridge." << std::endl;
        {
            auto devMtx = getDeviceDecodeMutex(deviceId);
            std::lock_guard<std::mutex> devLock(*devMtx);
            std::lock_guard<std::recursive_mutex> lock(decodersMutex);
            auto itDec = decoders.find(deviceId);
            if (itDec != decoders.end() && itDec->second) {
                itDec->second->ResetStreamSync();
            }
        }
        httpBridge.RemoveDevice(deviceId);
        ShmProducer* prod = getProducer(deviceId);
        if (prod) {
            prod->SetStreamingActive(false);
        }
    });

    tcpReceiver.SetAudioCallback([&getProducer, &httpBridge, &audioOutput, &audioDspMap, &audioDspMutex](
        int deviceId, const BouleCamAudioHeader& header, const uint8_t* payloadData, uint32_t payloadSize) {
        
        // Mutable buffer for real-time DSP (High-Pass Filter + Ambient level conditioning + Clock Drift Compensation)
        std::vector<uint8_t> pcmData(payloadData, payloadData + payloadSize);
        {
            std::lock_guard<std::mutex> lock(audioDspMutex);
            auto& dsp = audioDspMap[deviceId];
            if (!dsp) {
                dsp = std::make_unique<AudioDspProcessor>();
            }
            dsp->ProcessPcm16(pcmData, header.sample_rate, header.channels);
        }

        // Write processed PCM into per-device SHM; virtual-camera DLL applies mute/gain in real-time
        ShmProducer* prod = getProducer(deviceId);
        if (prod) {
            prod->WriteAudio(pcmData.data(), static_cast<uint32_t>(pcmData.size()), header.sample_rate, header.channels);
        }
        // Push to native Windows audio output device (Virtual Microphone) for the active device
        // so multiple simultaneous phones don't interleave/corrupt the single global PCM ring buffer
        if (deviceId == httpBridge.GetActiveDeviceId()) {
            audioOutput.WriteAudio(pcmData.data(), static_cast<uint32_t>(pcmData.size()));
        }
        // Per-camera audio buffer & level metering for UI and /obs/X browser sources
        httpBridge.PushAudioData(deviceId, pcmData.data(), static_cast<uint32_t>(pcmData.size()));
    });

    std::cout << "\n[Ready] BouleCam Desktop Service running. Waiting for mobile camera feed..." << std::endl;
    std::cout << "[Info] Connect your phone via Wi-Fi or USB Cable." << std::endl;
    std::cout << "[Info] Desktop UI Bridge: http://127.0.0.1:8095" << std::endl;
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
