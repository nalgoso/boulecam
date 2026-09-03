package com.boulecam.network

import android.content.Context
import android.net.wifi.WifiManager
import android.text.format.Formatter
import java.net.*
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

data class DiscoveredDevice(
    val ip: String,
    val port: Int,
    val name: String,
    val isUsb: Boolean
)

class AutoDiscoveryManager(
    private val context: Context,
    private val onDeviceFound: (DiscoveredDevice) -> Unit
) {
    private val isRunning = AtomicBoolean(false)
    private var multicastLock: WifiManager.MulticastLock? = null
    private val executor = Executors.newFixedThreadPool(3)

    fun start() {
        if (isRunning.getAndSet(true)) return

        try {
            val wifiManager = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
            multicastLock = wifiManager?.createMulticastLock("BouleCamMulticastLock")?.apply {
                setReferenceCounted(true)
                acquire()
            }
        } catch (ignored: Exception) {}

        // Task 1: Check USB (127.0.0.1) first
        executor.execute { checkUsbConnectionLoop() }

        // Task 2: UDP Broadcast Listener & Beacon Transmitter
        executor.execute { udpDiscoveryLoop() }

        // Task 3: Fast Subnet Scanner (Parallel TCP scan fallback)
        executor.execute { subnetScannerLoop() }
    }

    fun stop() {
        isRunning.set(false)
        try {
            if (multicastLock?.isHeld == true) {
                multicastLock?.release()
            }
        } catch (ignored: Exception) {}
        executor.shutdownNow()
    }

    /**
     * Checks if USB cable (ADB reverse tunnel on 127.0.0.1:8088) is active
     */
    private fun checkUsbConnectionLoop() {
        while (isRunning.get()) {
            try {
                val socket = Socket()
                socket.connect(InetSocketAddress("127.0.0.1", 8088), 600)
                socket.close()

                // USB is reachable!
                onDeviceFound(DiscoveredDevice("127.0.0.1", 8088, "PC USB Cable", true))
                Thread.sleep(3000)
            } catch (e: Exception) {
                try { Thread.sleep(1500) } catch (ignored: InterruptedException) { break }
            }
        }
    }

    /**
     * Sends UDP probe to 255.255.255.255:8089 and listens for PC Beacons
     */
    private fun udpDiscoveryLoop() {
        var socket: DatagramSocket? = null
        try {
            socket = DatagramSocket(null).apply {
                reuseAddress = true
                broadcast = true
                soTimeout = 2000
                bind(InetSocketAddress(8089))
            }

            val buffer = ByteArray(512)
            val packet = DatagramPacket(buffer, buffer.size)

            while (isRunning.get()) {
                // Send discovery probe
                try {
                    val probeMsg = "BOULECAM_DISCOVER".toByteArray()
                    val broadcastAddr = InetAddress.getByName("255.255.255.255")
                    val sendPacket = DatagramPacket(probeMsg, probeMsg.size, broadcastAddr, 8089)
                    socket.send(sendPacket)

                    // Also probe subnet broadcast
                    val subnetBroadcast = getSubnetBroadcastAddress()
                    if (subnetBroadcast != null) {
                        val sendPacketSubnet = DatagramPacket(probeMsg, probeMsg.size, subnetBroadcast, 8089)
                        socket.send(sendPacketSubnet)
                    }
                } catch (ignored: Exception) {}

                // Listen for responses / beacons
                try {
                    socket.receive(packet)
                    val senderIp = packet.address.hostAddress ?: continue
                    val msg = String(packet.data, 0, packet.length).trim()

                    // Parse "BOULECAM_BEACON:8088:<NAME>" or "BOULECAM_OFFER:8088:<NAME>"
                    if (msg.startsWith("BOULECAM_BEACON") || msg.startsWith("BOULECAM_OFFER")) {
                        val parts = msg.split(":")
                        val port = parts.getOrNull(1)?.toIntOrNull() ?: 8088
                        val name = parts.getOrNull(2) ?: "BouleCam PC"

                        onDeviceFound(DiscoveredDevice(senderIp, port, name, false))
                    }
                } catch (e: SocketTimeoutException) {
                    // Normal timeout
                } catch (ignored: Exception) {}

                try { Thread.sleep(1000) } catch (e: InterruptedException) { break }
            }
        } catch (e: Exception) {
            e.printStackTrace()
        } finally {
            socket?.close()
        }
    }

    /**
     * Fallback: Scans local /24 subnet for port 8088 if router blocks UDP broadcast
     */
    private fun subnetScannerLoop() {
        while (isRunning.get()) {
            val phoneIp = getPhoneIp()
            if (phoneIp.isNotEmpty() && phoneIp != "0.0.0.0" && phoneIp.contains(".")) {
                val prefix = phoneIp.substringBeforeLast(".") + "."
                val myLastOctet = phoneIp.substringAfterLast(".").toIntOrNull() ?: 0

                // Scan common IP range around our own IP first
                val candidates = mutableListOf<Int>()
                for (i in 1..254) {
                    if (i != myLastOctet) candidates.add(i)
                }

                // Sort by distance to our own IP for fast local match
                candidates.sortBy { Math.abs(it - myLastOctet) }

                for (octet in candidates) {
                    if (!isRunning.get()) break
                    val targetIp = prefix + octet
                    try {
                        val sock = Socket()
                        sock.connect(InetSocketAddress(targetIp, 8088), 120)
                        sock.close()

                        onDeviceFound(DiscoveredDevice(targetIp, 8088, "PC Wi-Fi ($targetIp)", false))
                        Thread.sleep(5000)
                    } catch (ignored: Exception) {}
                }
            }
            try { Thread.sleep(8000) } catch (e: InterruptedException) { break }
        }
    }

    private fun getPhoneIp(): String {
        try {
            val wifiManager = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
            val ip = wifiManager?.connectionInfo?.ipAddress ?: 0
            if (ip == 0) return ""
            return Formatter.formatIpAddress(ip)
        } catch (e: Exception) {
            return ""
        }
    }

    private fun getSubnetBroadcastAddress(): InetAddress? {
        try {
            val phoneIp = getPhoneIp()
            if (phoneIp.isNotEmpty()) {
                val prefix = phoneIp.substringBeforeLast(".") + ".255"
                return InetAddress.getByName(prefix)
            }
        } catch (ignored: Exception) {}
        return null
    }
}
