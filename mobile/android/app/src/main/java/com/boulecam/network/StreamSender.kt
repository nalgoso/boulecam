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
    private val deviceId: String = "",
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

    // Flow Control & Anti-Bufferbloat state
    private val lastAckedSequence = java.util.concurrent.atomic.AtomicLong(0L)
    private val isCongestionReported = AtomicBoolean(false)
    private var droppedFramesCount = 0L

    fun isConnected(): Boolean = isConnected.get()

    // Capacity 32: absorbs Wi-Fi bursts & audio+video interleaving without dropping H.264 P-frames
    private val sendQueue = java.util.concurrent.ArrayBlockingQueue<ByteArray>(32)
    private var workerThread: Thread? = null
    private val streamLock = Any()

    var onStatsUpdated: ((fps: Int, bitrateKbps: Int) -> Unit)? = null
    var onConnectionFailed: ((failedHost: String, failures: Int) -> Unit)? = null
    var onFrameDropped: (() -> Unit)? = null
    private var framesSentInInterval = 0
    private var bytesSentInInterval = 0L
    private var lastStatsTimestamp = 0L

    // Tracks consecutive failed connection attempts for exponential backoff
    private var consecutiveFailures = 0

    fun setHost(newHost: String, newPort: Int = 8088) {
        val cleanHost = newHost.trim().removePrefix("http://").removePrefix("https://").trim('/')
        var finalHost = cleanHost
        var finalPort = newPort
        if (cleanHost.contains(":")) {
            val parts = cleanHost.split(":")
            finalHost = parts[0]
            finalPort = parts[1].toIntOrNull() ?: newPort
        }

        val hostChanged = (host != finalHost || port != finalPort)
        host = finalHost
        port = finalPort

        if (hostChanged || !isConnected.get()) {
            Log.i(TAG, "Setting host to $finalHost:$finalPort (reconnecting immediately)")
            consecutiveFailures = 0
            closeSocket()
            workerThread?.interrupt()
        }
    }

    fun forceReconnect() {
        consecutiveFailures = 0
        closeSocket()
        workerThread?.interrupt()
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

        val currentSeq = sequenceNumber++

        // Build exact 24-byte BouleCamFrameHeader (no padding)
        val headerBuffer = ByteBuffer.allocate(24).order(ByteOrder.LITTLE_ENDIAN)
        headerBuffer.putInt(0x4243414D) // Magic "BCAM" (4 bytes)
        headerBuffer.put(0x10.toByte())  // BOULECAM_PKT_FRAME_DATA (1 byte)
        headerBuffer.put((if (isKeyFrame) 1 else 0).toByte()) // (1 byte)
        headerBuffer.putShort(rotation.toShort()) // (2 bytes)
        headerBuffer.putInt(currentSeq.toInt()) // (4 bytes)
        headerBuffer.putLong(timestampUs) // (8 bytes)
        headerBuffer.putInt(naluData.size) // (4 bytes)

        val fullPacket = ByteArray(24 + naluData.size)
        System.arraycopy(headerBuffer.array(), 0, fullPacket, 0, 24)
        System.arraycopy(naluData, 0, fullPacket, 24, naluData.size)

        // Ultra-low latency: drop stale frames if network is severely congested and request immediate IDR keyframe
        if (!sendQueue.offer(fullPacket)) {
            sendQueue.clear()
            sendQueue.offer(fullPacket)
            onFrameDropped?.invoke()
        }

        framesSentInInterval++
        bytesSentInInterval += fullPacket.size
        val now = System.currentTimeMillis()
        if (now - lastStatsTimestamp >= 1000) {
            val fps = framesSentInInterval
            val kbps = ((bytesSentInInterval * 8) / 1000).toInt()
            framesSentInInterval = 0
            bytesSentInInterval = 0L
            lastStatsTimestamp = now
            onStatsUpdated?.invoke(fps, kbps)
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

        // Enqueue onto lock-free sendQueue instead of blocking the real-time AudioRecord capture thread
        sendQueue.offer(fullPacket)
    }

    private fun runNetworkLoop() {
        Log.i(TAG, "Network loop started. Target: $host:$port")
        consecutiveFailures = 0
        while (isRunning.get()) {
            val targetHost = host
            val targetPort = port
            try {
                val sock = Socket()
                sock.tcpNoDelay = true
                sock.sendBufferSize = 2 * 1024 * 1024
                sock.soTimeout = 5000 // 5s read timeout; enough to not false-positive on idle periods
                sock.connect(InetSocketAddress(targetHost, targetPort), 2500)

                socket = sock
                outputStream = sock.getOutputStream()
                inputStream = sock.getInputStream()

                Log.i(TAG, "Socket connected to $targetHost:$targetPort. Performing handshake...")

                if (performHandshake()) {
                    Log.i(TAG, "Handshake OK — streaming to $targetHost:$targetPort")
                    consecutiveFailures = 0
                    // Flush any stale pre-connection P-frames so the new socket starts cleanly on SPS/PPS + IDR
                    sendQueue.clear()
                    isConnected.set(true)
                    onConnectionStateChanged(true)

                    // Spawn reader for incoming desktop commands
                    val readerThread = Thread { readIncomingCommands(sock) }.apply {
                        name = "BouleCamCmdReader"
                        isDaemon = true
                        start()
                    }

                    // Main send loop — exits on host change, stop, or socket failure
                    try {
                        while (isRunning.get() && !sock.isClosed && isConnected.get() && host == targetHost && port == targetPort) {
                            val packet = sendQueue.poll(100, java.util.concurrent.TimeUnit.MILLISECONDS)
                            if (packet != null) {
                                synchronized(streamLock) {
                                    outputStream?.write(packet)
                                    outputStream?.flush()
                                }
                            }
                        }
                    } catch (e: Exception) {
                        Log.w(TAG, "Send loop error: ${e.message}")
                    }

                    try { readerThread.join(300) } catch (ignored: Exception) {}

                    // If we exited because host changed (not because of a network error), don't
                    // report a disconnect — the loop will immediately reconnect to the new host.
                    val hostChanged = (host != targetHost || port != targetPort)
                    if (!hostChanged && isRunning.get()) {
                        // Genuine network drop
                        if (isConnected.getAndSet(false)) {
                            Log.i(TAG, "Connection lost to $targetHost:$targetPort")
                            onConnectionStateChanged(false)
                        }
                    }
                    closeSocket()
                } else {
                    Log.w(TAG, "Handshake failed on $targetHost:$targetPort")
                    closeSocket()
                    consecutiveFailures++
                    onConnectionFailed?.invoke(targetHost, consecutiveFailures)
                    if (isConnected.getAndSet(false)) {
                        onConnectionStateChanged(false)
                    }
                }
            } catch (e: Exception) {
                closeSocket()
                consecutiveFailures++
                onConnectionFailed?.invoke(targetHost, consecutiveFailures)
                if (isConnected.getAndSet(false)) {
                    Log.i(TAG, "Could not connect to $targetHost:$targetPort — ${e.message}")
                    onConnectionStateChanged(false)
                }
            }

            if (!isRunning.get()) break

            // Exponential back-off: 1s, 2s, 3s … capped at 5s
            val delayMs = (minOf(consecutiveFailures, 5) * 1000L).coerceAtLeast(1000L)
            try { Thread.sleep(delayMs) } catch (ignored: InterruptedException) {}
        }
    }

    private fun readIncomingCommands(sock: Socket) {
        val inStream = inputStream ?: return
        val header5 = ByteArray(5) // 4 bytes Magic (BCAM) + 1 byte PacketType

        while (isRunning.get() && isConnected.get() && !sock.isClosed) {
            try {
                if (!readExact(inStream, header5, 0, 5)) {
                    Log.i(TAG, "Socket closed by remote server (EOF). Disconnecting client.")
                    try { sock.close() } catch (ignored: Exception) {}
                    if (isConnected.getAndSet(false)) {
                        onConnectionStateChanged(false)
                    }
                    return
                }

                val hdrBuf = ByteBuffer.wrap(header5).order(ByteOrder.LITTLE_ENDIAN)
                val magic = hdrBuf.getInt()
                val type = hdrBuf.get().toInt() and 0xFF

                if (magic != 0x4243414D) {
                    continue
                }

                if (type == 0x30) { // BOULECAM_PKT_CAMERA_CMD (22 bytes total, 17 remaining)
                    val payload = ByteArray(17)
                    if (!readExact(inStream, payload, 0, 17)) break
                    val buf = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
                    val action = buf.get().toInt() and 0xFF
                    val intParam1 = buf.getInt()
                    val longParam1 = buf.getLong()
                    val floatParam1 = buf.getFloat()

                    onCommandReceived?.invoke(
                        CameraCommand(action, intParam1, longParam1, floatParam1)
                    )
                } else if (type == 0x12) { // BOULECAM_PKT_FRAME_ACK (24 bytes total, 19 remaining)
                    val payload = ByteArray(19)
                    if (!readExact(inStream, payload, 0, 19)) break
                    val buf = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
                    val statusFlags = buf.get().toInt() and 0xFF
                    val reserved = buf.getShort()
                    val lastSeqAcked = buf.getInt().toLong() and 0xFFFFFFFFL
                    val ackTsUs = buf.getLong()
                    val queueDepthMs = buf.getInt()

                    lastAckedSequence.set(lastSeqAcked)
                    isCongestionReported.set((statusFlags and 1 != 0) || (queueDepthMs > 50))
                }
            } catch (e: java.net.SocketTimeoutException) {
                continue
            } catch (e: Exception) {
                Log.w(TAG, "Reader thread socket error: ${e.message}")
                try { sock.close() } catch (ignored: Exception) {}
                if (isConnected.getAndSet(false)) {
                    onConnectionStateChanged(false)
                }
                break
            }
        }
    }

    private fun readExact(inStream: InputStream, buffer: ByteArray, offset: Int, length: Int): Boolean {
        var total = 0
        while (total < length && isConnected.get()) {
            val r = inStream.read(buffer, offset + total, length - total)
            if (r < 0) return false
            total += r
        }
        return total == length
    }

    private fun performHandshake(): Boolean {
        val out = outputStream ?: return false
        val `in` = inputStream ?: return false

        try {
            // Build Handshake Request (Exact 88 bytes: 4+1+1+1+1+4+4+4+4+48+16 = 88)
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
            val devBytes = ByteArray(48)
            System.arraycopy(nameBytes, 0, devBytes, 0, minOf(nameBytes.size, 47))
            hsBuffer.put(devBytes) // (48)

            val idBytes = deviceId.toByteArray(Charsets.UTF_8)
            val idArr = ByteArray(16)
            System.arraycopy(idBytes, 0, idArr, 0, minOf(idBytes.size, 16))
            hsBuffer.put(idArr) // (16)

            out.write(hsBuffer.array())
            out.flush()

            // Read Handshake Response (18 bytes) with resilience
            var attempts = 0
            while (attempts++ < 5) {
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

                if (magic == 0x4243414D && type == 0x02.toByte() && status == 0.toByte()) {
                    Log.i(TAG, "Handshake successful with desktop server")
                    return true
                } else if (magic == 0x4243414D && type == 0x30.toByte()) {
                    // Interleaved camera command packet (22 bytes total, 18 already read, drain last 4 bytes)
                    val extra = ByteArray(4)
                    var extraRead = 0
                    while (extraRead < 4) {
                        val r = `in`.read(extra, extraRead, 4 - extraRead)
                        if (r < 0) break
                        extraRead += r
                    }
                    Log.i(TAG, "Handshake absorbed initial command, awaiting response...")
                    continue
                } else {
                    Log.w(TAG, "Unexpected packet during handshake: magic=0x${Integer.toHexString(magic)}, type=$type, status=$status")
                    return false
                }
            }
            return false
        } catch (e: Exception) {
            Log.e(TAG, "Error in performHandshake: ${e.message}", e)
            return false
        }
    }

    fun sendCameraState(
        isDimmed: Boolean,
        batteryLevel: Float,
        isCharging: Boolean,
        currentLens: Int = 0,
        isTorchOn: Boolean = false,
        isMicEnabled: Boolean = true
    ) {
        if (!isConnected.get()) return
        try {
            val buffer = ByteBuffer.allocate(35).order(ByteOrder.LITTLE_ENDIAN)
            buffer.putInt(0x4243414D) // BOULECAM_MAGIC
            buffer.put(0x31.toByte())  // BOULECAM_PKT_CAMERA_STATE
            buffer.put(currentLens.toByte())
            buffer.put((if (isTorchOn) 1 else 0).toByte())
            buffer.putInt(0)           // current_iso
            buffer.putLong(0L)         // current_exposure_ns
            buffer.putInt(0)           // current_ev
            buffer.put(0.toByte())     // current_wb
            buffer.putFloat(0.0f)      // current_focus
            buffer.put((if (isMicEnabled) 1 else 0).toByte())
            buffer.putFloat(batteryLevel.coerceIn(0.0f, 1.0f))
            buffer.put((if (isDimmed) 1 else 0).toByte())
            buffer.put((if (isCharging) 1 else 0).toByte())

            val packet = buffer.array()
            sendQueue.offer(packet)
            Log.i(TAG, "Sent Telemetry state to PC: battery=${(batteryLevel * 100).toInt()}%, charging=$isCharging, dim=$isDimmed")
        } catch (e: Exception) {
            Log.e(TAG, "Error sending camera telemetry state: ${e.message}")
        }
    }

    fun sendDimState(isDimmed: Boolean, batteryLevel: Float = 1.0f, isCharging: Boolean = false) {
        sendCameraState(isDimmed, batteryLevel, isCharging)
    }

    private fun closeSocket() {
        try { outputStream?.close() } catch (ignored: Exception) {}
        try { inputStream?.close() } catch (ignored: Exception) {}
        try { socket?.close() } catch (ignored: Exception) {}
        socket = null
        outputStream = null
        inputStream = null
    }
}
