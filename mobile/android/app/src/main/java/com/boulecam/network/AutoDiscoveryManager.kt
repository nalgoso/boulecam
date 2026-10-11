package com.boulecam.network

import android.content.Context
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.net.wifi.WifiManager
import android.text.format.Formatter
import android.util.Log
import java.net.*
import java.util.concurrent.Executors
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
    private var probePool: java.util.concurrent.ExecutorService? = null

    // Debounce reported hosts
    @Volatile private var lastReportedWifiIp = ""
    @Volatile private var lastReportedWifiTimestampMs = 0L
    private val WIFI_REPORT_COOLDOWN_MS = 1_500L

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
        probePool = Executors.newFixedThreadPool(8)

        mainExecutor?.execute { udpDiscoveryLoop() }
        mainExecutor?.execute { subnetScannerLoop() }
    }

    fun stop() {
        if (!isRunning.getAndSet(false)) return
        try {
            if (multicastLock?.isHeld == true) multicastLock?.release()
        } catch (ignored: Exception) {}

        try { mainExecutor?.shutdownNow() } catch (ignored: Exception) {}
        try { probePool?.shutdownNow() } catch (ignored: Exception) {}
        mainExecutor = null
        probePool = null
    }

    /**
     * Dual Broadcast + Unicast UDP Prober & Listener.
     * Listens for PC beacons and proactively probes the network via broadcast
     * and unicast datagrams across the subnet to bypass router AP isolation.
     */
    private fun udpDiscoveryLoop() {
        var socket: DatagramSocket? = null
        while (isRunning.get()) {
            if (isConnectedProvider?.invoke() == true) {
                try { Thread.sleep(2000) } catch (e: InterruptedException) { break }
                continue
            }
            try {
                if (socket == null || socket.isClosed) {
                    socket = try {
                        DatagramSocket(null).apply {
                            reuseAddress = true
                            broadcast = true
                            soTimeout = 1000
                            bind(InetSocketAddress(8089))
                        }
                    } catch (e: Exception) {
                        DatagramSocket().apply {
                            broadcast = true
                            soTimeout = 1000
                        }
                    }
                }

                val currentSock = socket ?: continue
                val probeMsg = "BOULECAM_DISCOVER".toByteArray()

                // 1. Global broadcast (255.255.255.255)
                try {
                    val broadcastAddr = InetAddress.getByName("255.255.255.255")
                    currentSock.send(DatagramPacket(probeMsg, probeMsg.size, broadcastAddr, 8089))
                } catch (ignored: Exception) {}

                // 2. Directed subnet broadcast (e.g. 192.168.1.255)
                val subnetBcast = getSubnetBroadcastAddress()
                if (subnetBcast != null) {
                    try {
                        currentSock.send(DatagramPacket(probeMsg, probeMsg.size, subnetBcast, 8089))
                    } catch (ignored: Exception) {}
                }

                // 3. Unicast probe to last known Wi-Fi PC
                val prefs = context.getSharedPreferences("boulecam_prefs", Context.MODE_PRIVATE)
                val lastHost = prefs.getString("last_wifi_host", null)
                if (!lastHost.isNullOrBlank() && lastHost != "127.0.0.1") {
                    try {
                        val unicastAddr = InetAddress.getByName(lastHost)
                        currentSock.send(DatagramPacket(probeMsg, probeMsg.size, unicastAddr, 8089))
                    } catch (ignored: Exception) {}
                }

                // 4. Fast Unicast UDP sweep across local subnet (completely bypasses router broadcast blocks)
                val phoneIp = getPhoneIp()
                if (phoneIp.isNotEmpty() && phoneIp.contains(".")) {
                    val prefix = phoneIp.substringBeforeLast(".") + "."
                    val myLastOctet = phoneIp.substringAfterLast(".").toIntOrNull() ?: 0

                    for (hostOctet in 1..254) {
                        if (hostOctet == myLastOctet) continue
                        try {
                            val hostAddr = InetAddress.getByName(prefix + hostOctet)
                            currentSock.send(DatagramPacket(probeMsg, probeMsg.size, hostAddr, 8089))
                        } catch (ignored: Exception) {}
                    }
                }

                // 5. Receive incoming beacons or offers
                val buffer = ByteArray(512)
                val packet = DatagramPacket(buffer, buffer.size)

                val deadline = System.currentTimeMillis() + 1500L
                while (System.currentTimeMillis() < deadline && isRunning.get()) {
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
                            break
                        }
                    } catch (e: SocketTimeoutException) {
                        // Normal socket timeout
                        break
                    } catch (ignored: Exception) {
                        break
                    }
                }

            } catch (e: Exception) {
                try { socket?.close() } catch (ignored: Exception) {}
                socket = null
                try { Thread.sleep(1000) } catch (ignored: InterruptedException) { break }
            }
        }
        try { socket?.close() } catch (ignored: Exception) {}
    }

    /**
     * Backup TCP Subnet Scanner.
     * Sweeps candidate IPs on port 8090 with verified HTTP handshake
     * in case UDP is completely blocked by PC-side firewall rules.
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

                // Prioritized candidate list
                val prioritySet = LinkedHashSet<Int>()
                if (lastSavedOctet in 1..254 && lastSavedOctet != myLastOctet) {
                    prioritySet.add(lastSavedOctet)
                }
                if (myLastOctet != 1) prioritySet.add(1) // Gateway
                for (d in 1..15) {
                    val p1 = myLastOctet - d
                    val p2 = myLastOctet + d
                    if (p1 in 1..254) prioritySet.add(p1)
                    if (p2 in 1..254) prioritySet.add(p2)
                }
                for (o in 2..30) { if (o != myLastOctet) prioritySet.add(o) }
                for (o in 100..130) { if (o != myLastOctet) prioritySet.add(o) }
                for (o in 1..254) { if (o != myLastOctet) prioritySet.add(o) }

                val candidates = prioritySet.toList()
                val pool = probePool
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
                                    var verified = false
                                    // Probe HTTP control port 8090 with verification
                                    try {
                                        val sockHttp = Socket()
                                        sockHttp.connect(InetSocketAddress(targetIp, 8090), 450)
                                        sockHttp.soTimeout = 600

                                        // Verify BouleCam signature
                                        val out = sockHttp.getOutputStream()
                                        out.write("GET /api/status HTTP/1.0\r\nHost: $targetIp\r\n\r\n".toByteArray())
                                        out.flush()

                                        val inp = sockHttp.getInputStream()
                                        val readBuf = ByteArray(256)
                                        val bytes = inp.read(readBuf)
                                        if (bytes > 0) {
                                            val respStr = String(readBuf, 0, bytes)
                                            if (respStr.contains("connected") || respStr.contains("activeDeviceId") || respStr.contains("BouleCam") || respStr.contains("obsUrl")) {
                                                verified = true
                                            }
                                        }
                                        sockHttp.close()
                                    } catch (ignored: Exception) {}

                                    if (verified && !foundSignal.getAndSet(true)) {
                                        Log.i(TAG, "Discovered and verified BouleCam PC on port 8090: $targetIp:8088")
                                        reportWifiDevice(targetIp, 8088, "BouleCam PC ($targetIp)")
                                    }
                                }
                            } finally {
                                latch.countDown()
                            }
                        }
                    }

                    try {
                        latch.await(3000, TimeUnit.MILLISECONDS)
                    } catch (ignored: Exception) {}
                }
            }

            try { Thread.sleep(2500) } catch (e: InterruptedException) { break }
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
