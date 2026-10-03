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
        scannerPool = Executors.newFixedThreadPool(48)

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
            if (isConnectedProvider?.invoke() == true) {
                try { Thread.sleep(2500) } catch (e: InterruptedException) { break }
                continue
            }
            try {
                if (socket == null || socket.isClosed) {
                    socket = try {
                        DatagramSocket(null).apply {
                            reuseAddress = true
                            broadcast = true
                            soTimeout = 1200
                            bind(InetSocketAddress(8089))
                        }
                    } catch (e: Exception) {
                        DatagramSocket().apply {
                            broadcast = true
                            soTimeout = 1200
                        }
                    }
                }

                val currentSock = socket ?: continue

                // Send proactive discovery probes (Global broadcast + Subnet broadcast + Unicast to last known host)
                try {
                    val probeMsg = "BOULECAM_DISCOVER".toByteArray()
                    val broadcastAddr = InetAddress.getByName("255.255.255.255")
                    currentSock.send(DatagramPacket(probeMsg, probeMsg.size, broadcastAddr, 8089))

                    val subnetBcast = getSubnetBroadcastAddress()
                    if (subnetBcast != null) {
                        currentSock.send(DatagramPacket(probeMsg, probeMsg.size, subnetBcast, 8089))
                    }

                    val prefs = context.getSharedPreferences("boulecam_prefs", Context.MODE_PRIVATE)
                    val lastHost = prefs.getString("last_wifi_host", null)
                    if (!lastHost.isNullOrBlank() && lastHost != "127.0.0.1") {
                        try {
                            val unicastAddr = InetAddress.getByName(lastHost)
                            currentSock.send(DatagramPacket(probeMsg, probeMsg.size, unicastAddr, 8089))
                        } catch (ignored: Exception) {}
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
                try { Thread.sleep(1500) } catch (ignored: InterruptedException) { break }
            }
        }
        try { socket?.close() } catch (ignored: Exception) {}
    }

    /**
     * Ultra-fast multi-threaded Subnet Scanner.
     * Sweeps all 254 IPs in the Wi-Fi subnet in parallel with 48 concurrent workers (~0.8s).
     * Bypasses router UDP broadcast/multicast isolation completely.
     */
    private fun subnetScannerLoop() {
        while (isRunning.get()) {
            if (isConnectedProvider?.invoke() == true) {
                try { Thread.sleep(2500) } catch (e: InterruptedException) { break }
                continue
            }
            val phoneIp = getPhoneIp()
            if (phoneIp.isNotEmpty() && phoneIp.contains(".")) {
                val prefix = phoneIp.substringBeforeLast(".") + "."
                val myLastOctet = phoneIp.substringAfterLast(".").toIntOrNull() ?: 0

                val prefs = context.getSharedPreferences("boulecam_prefs", Context.MODE_PRIVATE)
                val lastSavedHost = prefs.getString("last_wifi_host", null) ?: ""
                val lastSavedOctet = if (lastSavedHost.startsWith(prefix)) {
                    lastSavedHost.substringAfterLast(".").toIntOrNull() ?: -1
                } else -1

                // Smart prioritization:
                // 1. Last saved IP (if in same subnet)
                // 2. Gateway (.1)
                // 3. Low IPs (.2 - .25)
                // 4. Common DHCP ranges (.100 - .130)
                // 5. Neighbors around phone octet
                // 6. All remaining IPs
                val prioritySet = LinkedHashSet<Int>()
                if (lastSavedOctet in 1..254 && lastSavedOctet != myLastOctet) {
                    prioritySet.add(lastSavedOctet)
                }
                if (myLastOctet != 1) prioritySet.add(1)
                for (o in 2..25) { if (o != myLastOctet) prioritySet.add(o) }
                for (o in 100..130) { if (o != myLastOctet) prioritySet.add(o) }
                for (d in 1..10) {
                    val p1 = myLastOctet - d
                    val p2 = myLastOctet + d
                    if (p1 in 1..254) prioritySet.add(p1)
                    if (p2 in 1..254) prioritySet.add(p2)
                }
                for (o in 1..254) {
                    if (o != myLastOctet) prioritySet.add(o)
                }

                val candidates = prioritySet.toList()
                val pool = scannerPool
                if (pool != null && !pool.isShutdown) {
                    val foundSignal = AtomicBoolean(false)
                    val latch = java.util.concurrent.CountDownLatch(candidates.size)

                    for (octet in candidates) {
                        if (!isRunning.get() || foundSignal.get()) {
                            latch.countDown()
                            continue
                        }
                        val targetIp = prefix + octet
                        pool.submit {
                            try {
                                if (!foundSignal.get()) {
                                    var discovered = false
                                    // Probe HTTP bridge port 8090 (does not interfere with the binary streaming server on 8088!)
                                    try {
                                        val sockHttp = Socket()
                                        sockHttp.connect(InetSocketAddress(targetIp, 8090), 200)
                                        sockHttp.close()
                                        discovered = true
                                    } catch (ignored: Exception) {}

                                    if (discovered && !foundSignal.getAndSet(true)) {
                                        Log.i(TAG, "Discovered PC via high-speed parallel scan on port 8090: $targetIp:8088")
                                        reportWifiDevice(targetIp, 8088, "BouleCam PC ($targetIp)")
                                    }
                                }
                            } finally {
                                latch.countDown()
                            }
                        }
                    }

                    // Await batch completion or early match within 1.8 seconds max
                    try {
                        latch.await(1800, TimeUnit.MILLISECONDS)
                    } catch (ignored: Exception) {}
                }
            }

            // Rescan rapidly (1.2s) when not connected
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
