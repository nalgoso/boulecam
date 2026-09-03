package com.boulecam.network

import android.util.Log
import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.ConcurrentLinkedQueue
import java.util.concurrent.atomic.AtomicBoolean

data class CameraCommand(
    val action: Int,
    val intParam1: Int,
    val longParam1: Long,
    val floatParam1: Float
)

class StreamSender(
    @Volatile private var host: String = "127.0.0.1",
    @Volatile private var port: Int = 8088,
    private val deviceName: String = "BouleCam Mobile",
    private val onConnectionStateChanged: (Boolean) -> Unit,
    private val onCommandReceived: ((CameraCommand) -> Unit)? = null
) {
    private val TAG = "BouleCamNetwork"

    private var socket: Socket? = null
    private var outputStream: OutputStream? = null
    private var inputStream: InputStream? = null

    private val isConnected = AtomicBoolean(false)
    private val isRunning = AtomicBoolean(false)
    private var sequenceNumber = 0L

    fun isConnected(): Boolean = isConnected.get()

    private val sendQueue = java.util.concurrent.ArrayBlockingQueue<ByteArray>(2)
    private var workerThread: Thread? = null
    private val streamLock = Any()

    fun setHost(newHost: String, newPort: Int = 8088) {
        val cleanHost = newHost.trim().removePrefix("http://").removePrefix("https://").trim('/')
        var finalHost = cleanHost
        var finalPort = newPort
        if (cleanHost.contains(":")) {
            val parts = cleanHost.split(":")
            finalHost = parts[0]
            finalPort = parts[1].toIntOrNull() ?: newPort
        }

        if (host != finalHost || port != finalPort) {
            Log.i(TAG, "Switching host to $finalHost:$finalPort")
            host = finalHost
            port = finalPort
            closeSocket()
            workerThread?.interrupt()
        }
    }

    fun getHost(): String = host
    fun getPort(): Int = port

    fun start() {
        if (isRunning.getAndSet(true)) return

        workerThread = Thread { runNetworkLoop() }.apply {
            name = "BouleCamNetworkThread"
            priority = Thread.MAX_PRIORITY
            start()
        }
    }

    fun stop() {
        isRunning.set(false)
        closeSocket()
        workerThread?.interrupt()
        try { workerThread?.join(500) } catch (ignored: Exception) {}
        workerThread = null
    }

    fun sendFrame(isKeyFrame: Boolean, timestampUs: Long, naluData: ByteArray, rotation: Int = 0) {
        if (!isConnected.get()) return

        // Build exact 24-byte BouleCamFrameHeader (no padding)
        val headerBuffer = ByteBuffer.allocate(24).order(ByteOrder.LITTLE_ENDIAN)
        headerBuffer.putInt(0x4243414D) // Magic "BCAM" (4 bytes)
        headerBuffer.put(0x10.toByte())  // BOULECAM_PKT_FRAME_DATA (1 byte)
        headerBuffer.put((if (isKeyFrame) 1 else 0).toByte()) // (1 byte)
        headerBuffer.putShort(rotation.toShort()) // (2 bytes)
        headerBuffer.putInt((sequenceNumber++).toInt()) // (4 bytes)
        headerBuffer.putLong(timestampUs) // (8 bytes)
        headerBuffer.putInt(naluData.size) // (4 bytes)

        val fullPacket = ByteArray(24 + naluData.size)
        System.arraycopy(headerBuffer.array(), 0, fullPacket, 0, 24)
        System.arraycopy(naluData, 0, fullPacket, 24, naluData.size)

        // Ultra-low latency: drop stale frame if network is congested
        if (!sendQueue.offer(fullPacket)) {
            sendQueue.poll()
            sendQueue.offer(fullPacket)
        }
    }

    fun sendAudio(pcmData: ByteArray, size: Int) {
        if (!isConnected.get()) return

        // 12-byte BouleCamAudioHeader (BOULECAM_PKT_AUDIO_DATA = 0x40)
        val headerBuffer = ByteBuffer.allocate(12).order(ByteOrder.LITTLE_ENDIAN)
        headerBuffer.putInt(0x4243414D)        // Magic "BCAM"
        headerBuffer.put(0x40.toByte())         // BOULECAM_PKT_AUDIO_DATA
        headerBuffer.put(1.toByte())            // 1 = Mono
        headerBuffer.putShort(48000.toShort())  // 48000 Hz
        headerBuffer.putInt(size)               // Payload size

        val fullPacket = ByteArray(12 + size)
        System.arraycopy(headerBuffer.array(), 0, fullPacket, 0, 12)
        System.arraycopy(pcmData, 0, fullPacket, 12, size)

        try {
            synchronized(streamLock) {
                outputStream?.write(fullPacket)
                outputStream?.flush()
            }
        } catch (ignored: Exception) {}
    }

    private fun runNetworkLoop() {
        Log.i(TAG, "Network loop started. Target: $host:$port")
        while (isRunning.get()) {
            val targetHost = host
            val targetPort = port
            try {
                val sock = Socket()
                sock.tcpNoDelay = true
                sock.sendBufferSize = 2 * 1024 * 1024
                sock.soTimeout = 4000
                sock.connect(InetSocketAddress(targetHost, targetPort), 2000)

                socket = sock
                outputStream = sock.getOutputStream()
                inputStream = sock.getInputStream()

                Log.i(TAG, "Socket connected to $targetHost:$targetPort. Performing Handshake...")

                // Perform Handshake
                if (performHandshake()) {
                    Log.i(TAG, "Handshake SUCCEEDED! Streaming live frames to $targetHost:$targetPort")
                    isConnected.set(true)
                    onConnectionStateChanged(true)

                    // Start Reader thread for incoming desktop camera control commands
                    val readerThread = Thread { readIncomingCommands(sock) }.apply {
                        name = "BouleCamCmdReader"
                        start()
                    }

                    while (isRunning.get() && isConnected.get() && !sock.isClosed && host == targetHost && port == targetPort) {
                        val packet = sendQueue.poll(50, java.util.concurrent.TimeUnit.MILLISECONDS)
                        if (packet != null) {
                            synchronized(streamLock) {
                                outputStream?.write(packet)
                                outputStream?.flush()
                            }
                        }
                    }

                    try { readerThread.join(200) } catch (ignored: Exception) {}
                } else {
                    Log.w(TAG, "Handshake failed on $targetHost:$targetPort")
                    closeSocket()
                }
            } catch (e: Exception) {
                closeSocket()
                if (isConnected.getAndSet(false)) {
                    Log.i(TAG, "Disconnected from $targetHost:$targetPort (${e.message})")
                    onConnectionStateChanged(false)
                }
                try { Thread.sleep(350) } catch (ignored: Exception) {}
            }
        }
    }

    private fun readIncomingCommands(sock: Socket) {
        val inStream = inputStream ?: return
        val cmdBuffer = ByteArray(22) // BouleCamCameraCmd: 4 + 1 + 1 + 4 + 8 + 4 = 22 bytes

        while (isRunning.get() && isConnected.get() && !sock.isClosed) {
            try {
                var total = 0
                while (total < 22 && isConnected.get()) {
                    val r = inStream.read(cmdBuffer, total, 22 - total)
                    if (r < 0) return
                    total += r
                }

                if (total == 22) {
                    val buf = ByteBuffer.wrap(cmdBuffer).order(ByteOrder.LITTLE_ENDIAN)
                    val magic = buf.getInt()
                    val type = buf.get()
                    if (magic == 0x4243414D && type == 0x30.toByte()) { // BOULECAM_PKT_CAMERA_CMD
                        val action = buf.get().toInt() and 0xFF
                        val intParam1 = buf.getInt()
                        val longParam1 = buf.getLong()
                        val floatParam1 = buf.getFloat()

                        onCommandReceived?.invoke(
                            CameraCommand(action, intParam1, longParam1, floatParam1)
                        )
                    }
                }
            } catch (e: java.net.SocketTimeoutException) {
                continue
            } catch (e: Exception) {
                break
            }
        }
    }

    private fun performHandshake(): Boolean {
        val out = outputStream ?: return false
        val `in` = inputStream ?: return false

        try {
            // Build Handshake Request (Exact 88 bytes: 4+1+1+1+1+4+4+4+4+64 = 88)
            val hsBuffer = ByteBuffer.allocate(88).order(ByteOrder.LITTLE_ENDIAN)
            hsBuffer.putInt(0x4243414D) // Magic "BCAM" (4)
            hsBuffer.put(0x01.toByte())  // BOULECAM_PKT_HANDSHAKE_REQ (1)
            hsBuffer.put(1.toByte())     // Version Major (1)
            hsBuffer.put(0.toByte())     // Version Minor (1)
            hsBuffer.put(1.toByte())     // Codec H.264 (1)
            hsBuffer.putInt(1920)        // Width (4)
            hsBuffer.putInt(1080)        // Height (4)
            hsBuffer.putInt(60)          // Target FPS (4)
            hsBuffer.putInt(8000000)     // Bitrate (8 Mbps) (4)

            val nameBytes = deviceName.toByteArray(Charsets.UTF_8)
            val devBytes = ByteArray(64)
            System.arraycopy(nameBytes, 0, devBytes, 0, minOf(nameBytes.size, 63))
            hsBuffer.put(devBytes) // (64)

            out.write(hsBuffer.array())
            out.flush()

            // Read Handshake Response (18 bytes)
            val respBuf = ByteArray(18)
            var totalRead = 0
            while (totalRead < 18) {
                val r = `in`.read(respBuf, totalRead, 18 - totalRead)
                if (r < 0) {
                    Log.w(TAG, "EOF while reading handshake response")
                    return false
                }
                totalRead += r
            }

            val resp = ByteBuffer.wrap(respBuf).order(ByteOrder.LITTLE_ENDIAN)
            val magic = resp.getInt()
            val type = resp.get()
            val status = resp.get()

            val success = magic == 0x4243414D && type == 0x02.toByte() && status == 0.toByte()
            Log.i(TAG, "Handshake response: magic=0x${Integer.toHexString(magic)}, type=$type, status=$status -> success=$success")
            return success
        } catch (e: Exception) {
            Log.e(TAG, "Error in performHandshake: ${e.message}", e)
            return false
        }
    }

    fun sendTelemetry(
        batteryLevel: Float,
        temperatureC: Float,
        lensesMask: Int,
        currentZoom: Float,
        currentLens: Int,
        isDimmed: Boolean,
        isTorchOn: Boolean = false
    ) {
        if (!isConnected.get()) return
        try {
            val buffer = ByteBuffer.allocate(43).order(ByteOrder.LITTLE_ENDIAN)
            buffer.putInt(0x4243414D) // BOULECAM_MAGIC
            buffer.put(0x31.toByte())  // BOULECAM_PKT_CAMERA_STATE
            buffer.put(currentLens.toByte()) // current_lens (0=Back, 1=Front, 2=UltraWide, 3=Tele)
            buffer.put((if (isTorchOn) 1 else 0).toByte()) // torch_on
            buffer.putInt(0)           // current_iso
            buffer.putLong(0L)         // current_exposure_ns
            buffer.putInt(0)           // current_ev
            buffer.put(0.toByte())     // current_wb
            buffer.putFloat(0.0f)      // current_focus
            buffer.put(1.toByte())     // mic_enabled
            buffer.putFloat(batteryLevel) // battery_level (0.0 - 100.0)
            buffer.put((if (isDimmed) 1 else 0).toByte()) // dim_screen_active
            buffer.putFloat(temperatureC) // device_temperature in Celsius
            buffer.put(lensesMask.toByte()) // available_lenses_mask
            buffer.putFloat(currentZoom) // current_zoom
            val packet = buffer.array()
            sendQueue.offer(packet)
        } catch (e: Exception) {
            Log.e(TAG, "Error sending telemetry: ${e.message}")
        }
    }

    fun sendDimState(isDimmed: Boolean) {
        sendTelemetry(
            batteryLevel = -1.0f,
            temperatureC = 0.0f,
            lensesMask = 3,
            currentZoom = 1.0f,
            currentLens = 0,
            isDimmed = isDimmed
        )
    }

    private fun closeSocket() {
        try {
            outputStream?.close()
            inputStream?.close()
            socket?.close()
        } catch (ignored: Exception) {}
        socket = null
        outputStream = null
        inputStream = null
    }
}
