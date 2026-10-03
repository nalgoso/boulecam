package com.boulecam.network

import android.content.Context
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.net.wifi.WifiManager
import android.text.format.Formatter
import android.util.Log
import java.net.*
import java.util.concurrent.Executors
import java.util.concurrent.Future
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

data class DiscoveredDevice(
    val ip: String,
    val port: Int,
    val name: String,
    val isUsb: Boolean
)

class AutoDiscoveryManager(
    private val context: Context,
    private val isConnectedProvider: (() -> Boolean)? = null,
    private val onDeviceFound: (DiscoveredDevice) -> Unit
) {
    private val TAG = "BouleCamDiscovery"
    private val isRunning = AtomicBoolean(false)
    private var multicastLock: WifiManager.MulticastLock? = null
    private var mainExecutor: java.util.concurrent.ExecutorService? = null
    private var scannerPool: java.util.concurrent.ExecutorService? = null

    // Debounce reported hosts
    @Volatile private var lastReportedWifiIp = ""
    @Volatile private var lastReportedWifiTimestampMs = 0L
    private val WIFI_REPORT_COOLDOWN_MS = 2_000L

    fun start() {
        if (isRunning.getAndSet(true)) return

        try {
            val wifiManager = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
            multicastLock = wifiManager?.createMulticastLock("BouleCamMulticastLock")?.apply {
                setReferenceCounted(true)
                acquire()
            }
        } catch (ignored: Exception) {}

        mainExecutor = Executors.newFixedThreadPool(2)
        scannerPool = Executors.newFixedThreadPool(16)

        mainExecutor?.execute { udpDiscoveryLoop() }
        mainExecutor?.execute { subnetScannerLoop() }
    }

    fun stop() {
        if (!isRunning.getAndSet(false)) return
        try {
            if (multicastLock?.isHeld == true) multicastLock?.release()
        } catch (ignored: Exception) {}

        try { mainExecutor?.shutdownNow() } catch (ignored: Exception) {}
        try { scannerPool?.shutdownNow() } catch (ignored: Exception) {}
        mainExecutor = null
        scannerPool = null
    }

    /**
     * UDP beacon broadcaster and listener.
     * Listens for PC beacons and proactively probes the network.
     */
    private fun udpDiscoveryLoop() {
        var socket: DatagramSocket? = null
        while (isRunning.get()) {
            try {
                if (socket == null || socket.isClosed) {
                    socket = try {
                        DatagramSocket(null).apply {
                            reuseAddress = true
                            broadcast = true
                            soTimeout = 1500
                            bind(InetSocketAddress(8089))
                        }
                    } catch (e: Exception) {
                        // Fallback to ephemeral port for sending probes if 8089 cannot be bound
                        DatagramSocket().apply {
                            broadcast = true
                            soTimeout = 1500
                        }
                    }
                }

                val currentSock = socket ?: continue

                // Send proactive discovery probe
                try {
                    val probeMsg = "BOULECAM_DISCOVER".toByteArray()
                    val broadcastAddr = InetAddress.getByName("255.255.255.255")
                    currentSock.send(DatagramPacket(probeMsg, probeMsg.size, broadcastAddr, 8089))

                    val subnetBcast = getSubnetBroadcastAddress()
                    if (subnetBcast != null) {
                        currentSock.send(DatagramPacket(probeMsg, probeMsg.size, subnetBcast, 8089))
                    }
                } catch (ignored: Exception) {}

                // Receive beacons or responses
                val buffer = ByteArray(512)
                val packet = DatagramPacket(buffer, buffer.size)

                try {
                    currentSock.receive(packet)
                    val senderIp = packet.address.hostAddress ?: continue
                    val msg = String(packet.data, 0, packet.length).trim()
                    if (msg.startsWith("BOULECAM_BEACON") || msg.startsWith("BOULECAM_OFFER")) {
                        val parts = msg.split(":")
                        val port = parts.getOrNull(1)?.toIntOrNull() ?: 8088
                        val name = parts.getOrNull(2) ?: "BouleCam PC"
                        Log.i(TAG, "Discovered PC via UDP: $senderIp:$port ($name)")
                        reportWifiDevice(senderIp, port, name)
                    }
                } catch (e: SocketTimeoutException) {
                    // Normal timeout, loop again
                } catch (ignored: Exception) {}

            } catch (e: Exception) {
                try { socket?.close() } catch (ignored: Exception) {}
                socket = null
                try { Thread.sleep(2000) } catch (ignored: InterruptedException) { break }
            }
        }
        try { socket?.close() } catch (ignored: Exception) {}
    }

    /**
     * Ultra-fast multi-threaded Subnet Scanner.
     * Sweeps all 254 IPs in the Wi-Fi subnet in parallel (~1.5s).
     * Bypasses router UDP broadcast/multicast isolation.
     */
    private fun subnetScannerLoop() {
        while (isRunning.get()) {
            if (isConnectedProvider?.invoke() == true) {
                try { Thread.sleep(3000) } catch (e: InterruptedException) { break }
                continue
            }
            val phoneIp = getPhoneIp()
            if (phoneIp.isNotEmpty() && phoneIp.contains(".")) {
                val prefix = phoneIp.substringBeforeLast(".") + "."
                val myLastOctet = phoneIp.substringAfterLast(".").toIntOrNull() ?: 0

                val candidates = (1..254).filter { it != myLastOctet }.sortedBy { Math.abs(it - myLastOctet) }
                val pool = scannerPool
                if (pool != null && !pool.isShutdown) {
                    val foundSignal = AtomicBoolean(false)
                    val futures = mutableListOf<Future<*>>()

                    for (octet in candidates) {
                        if (!isRunning.get() || foundSignal.get()) break
                        val targetIp = prefix + octet
                        val task = pool.submit {
                            if (foundSignal.get()) return@submit
                            try {
                                val sock = Socket()
                                sock.connect(InetSocketAddress(targetIp, 8088), 350)
                                sock.close()
                                if (!foundSignal.getAndSet(true)) {
                                    Log.i(TAG, "Discovered PC via parallel subnet scan: $targetIp:8088")
                                    reportWifiDevice(targetIp, 8088, "BouleCam PC ($targetIp)")
                                }
                            } catch (ignored: Exception) {}
                        }
                        futures.add(task)
                    }

                    // Await batch completion or early match
                    for (f in futures) {
                        try {
                            f.get(400, TimeUnit.MILLISECONDS)
                            if (foundSignal.get()) break
                        } catch (ignored: Exception) {}
                    }
                }
            }

            // Rescan every 3 seconds if disconnected
            try { Thread.sleep(3_000) } catch (e: InterruptedException) { break }
        }
    }

    private fun reportWifiDevice(ip: String, port: Int, name: String) {
        val now = System.currentTimeMillis()
        if (ip == lastReportedWifiIp && (now - lastReportedWifiTimestampMs) < WIFI_REPORT_COOLDOWN_MS) {
            return
        }
        lastReportedWifiIp = ip
        lastReportedWifiTimestampMs = now
        onDeviceFound(DiscoveredDevice(ip, port, name, false))
    }

    /**
     * Resolves the phone's true Wi-Fi IPv4 address.
     * Prioritizes WLAN interfaces over cellular / mobile networks.
     */
    fun getPhoneIp(): String {
        // Priority 1: ConnectivityManager Active Network Link Properties (Android 6+)
        try {
            val cm = context.getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager
            val activeNet = cm?.activeNetwork
            if (activeNet != null) {
                val caps = cm.getNetworkCapabilities(activeNet)
                if (caps != null && (caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) || caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET))) {
                    val lp = cm.getLinkProperties(activeNet)
                    if (lp != null) {
                        for (la in lp.linkAddresses) {
                            val addr = la.address
                            if (addr is Inet4Address && !addr.isLoopbackAddress) {
                                val host = addr.hostAddress ?: continue
                                if (!host.startsWith("127.") && !host.startsWith("169.254")) {
                                    return host
                                }
                            }
                        }
                    }
                }
            }
        } catch (ignored: Exception) {}

        // Priority 2: NetworkInterface enumeration prioritizing 'wlan', 'wifi', 'eth'
        try {
            val interfaces = NetworkInterface.getNetworkInterfaces()?.toList() ?: emptyList()

            // Sort so wlan/wifi comes first, cellular (rmnet, ccmni, pdp) last
            val sorted = interfaces.sortedWith(Comparator { a, b ->
                val aName = a.name.lowercase()
                val bName = b.name.lowercase()
                val aIsWifi = aName.contains("wlan") || aName.contains("wifi") || aName.contains("eth")
                val bIsWifi = bName.contains("wlan") || bName.contains("wifi") || bName.contains("eth")
                when {
                    aIsWifi && !bIsWifi -> -1
                    !aIsWifi && bIsWifi -> 1
                    else -> 0
                }
            })

            for (iface in sorted) {
                if (!iface.isUp || iface.isLoopback) continue
                val name = iface.name.lowercase()
                // Strictly exclude cellular carrier interfaces
                if (name.contains("rmnet") || name.contains("ccmni") || name.contains("pdp") || name.contains("dummy")) {
                    continue
                }

                for (addr in iface.inetAddresses.asSequence()) {
                    if (addr is Inet4Address && !addr.isLoopbackAddress) {
                        val ip = addr.hostAddress ?: continue
                        if (ip.startsWith("192.168.") || ip.startsWith("10.") || ip.startsWith("172.")) {
                            return ip
                        }
                    }
                }
            }
        } catch (ignored: Exception) {}

        // Priority 3: WifiManager fallback
        try {
            val wifiManager = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
            @Suppress("DEPRECATION")
            val ip = wifiManager?.connectionInfo?.ipAddress ?: 0
            if (ip != 0) return Formatter.formatIpAddress(ip)
        } catch (ignored: Exception) {}

        return ""
    }

    private fun getSubnetBroadcastAddress(): InetAddress? {
        return try {
            val phoneIp = getPhoneIp()
            if (phoneIp.isNotEmpty() && phoneIp.contains(".")) {
                InetAddress.getByName(phoneIp.substringBeforeLast(".") + ".255")
            } else {
                null
            }
        } catch (ignored: Exception) { null }
    }
}
