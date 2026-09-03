package com.boulecam

import android.Manifest
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.content.res.ColorStateList
import android.content.res.Configuration
import android.graphics.Color
import android.graphics.SurfaceTexture
import android.os.BatteryManager
import android.os.Build
import android.os.Bundle
import android.view.OrientationEventListener
import android.view.Surface
import android.view.TextureView
import android.view.View
import android.view.WindowManager
import android.widget.*
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import com.boulecam.audio.AudioCapturePipeline
import com.boulecam.camera.CameraCapturePipeline
import com.boulecam.encoder.HardwareEncoder
import com.boulecam.network.AutoDiscoveryManager
import com.boulecam.network.CameraCommand
import com.boulecam.network.StreamSender
import com.boulecam.service.BouleCamStreamService
import com.boulecam.ui.AutoFitTextureView

class MainActivity : AppCompatActivity() {
    companion object {
        const val COLOR_GREEN = 0xFF006D3B.toInt()
        const val COLOR_LIGHT_GREEN = 0xFF4ADE80.toInt()
        const val COLOR_GRAY = 0xFF7A7A7A.toInt()
        const val COLOR_DARK = 0xFF1A1A1A.toInt()
        const val COLOR_BLACK = 0xFF222222.toInt()
    }

    private val CAMERA_PERMISSION_CODE = 1001

    private var textureView: AutoFitTextureView? = null
    private var previewSurface: Surface? = null
    private var orientationListener: OrientationEventListener? = null

    private var statusTextView: TextView? = null
    private var fpsTextView: TextView? = null
    private var layoutModeToggle: FrameLayout? = null
    private var toggleThumb: View? = null
    private var txtThumbMode: TextView? = null
    private var txtToggleHint: TextView? = null
    private var dimScreenOverlay: FrameLayout? = null

    // Bottom Quick Dock Buttons (Identical to Desktop Studio)
    private var btnDockFlip: ImageButton? = null
    private var btnDockTorch: ImageButton? = null
    private var btnDockMic: ImageButton? = null
    private var btnDockMirror: ImageButton? = null
    private var btnDockRotate180: ImageButton? = null
    private var btnDockDim: ImageButton? = null
    private var btnDockPro: ImageButton? = null
    private var manualRotation180 = false
    private var manualMirror = false
    private var isDockTrayVisible = true

    private var proControlsLayout: LinearLayout? = null
    private var seekIso: SeekBar? = null
    private var lblIso: TextView? = null
    private var seekEv: SeekBar? = null
    private var lblEv: TextView? = null
    private var seekWb: SeekBar? = null
    private var lblWb: TextView? = null
    private var seekFocus: SeekBar? = null
    private var lblFocus: TextView? = null

    private var encoder: HardwareEncoder? = null
    private var sender: StreamSender? = null
    private var cameraPipeline: CameraCapturePipeline? = null
    private var audioPipeline: AudioCapturePipeline? = null
    private var discoveryManager: AutoDiscoveryManager? = null

    private var isMicEnabled = true
    private var isUsbMode = true
    private var isUsbCableConnected = false
    private var isDimScreenActive = false
    private var physicalOrientationDegrees = 0
    private var wifiToastShown = false
    private val cameraExecutor = java.util.concurrent.Executors.newSingleThreadExecutor()

    private var usbReceiver: BroadcastReceiver? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Keep screen awake while streaming
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        setContentView(R.layout.activity_main)

        // Start Foreground Service to keep CPU and Camera active on tripod
        BouleCamStreamService.start(this)

        initViews()
        setupUsbReceiver()
        checkUsbCableState()
        setupOrientationListener()

        if (checkCameraPermission()) {
            setupStreamingPipeline()
        } else {
            requestCameraPermission()
        }
    }

    private fun initViews() {
        textureView = findViewById(R.id.camera_preview)

        statusTextView = findViewById(R.id.status_text)
        fpsTextView = findViewById(R.id.fps_text)

        layoutModeToggle = findViewById(R.id.layout_mode_toggle)
        toggleThumb = findViewById(R.id.toggle_thumb)
        txtThumbMode = findViewById(R.id.txt_thumb_mode)
        txtToggleHint = findViewById(R.id.txt_toggle_hint)
        dimScreenOverlay = findViewById(R.id.dim_screen_overlay)

        btnDockFlip = findViewById(R.id.btn_dock_flip)
        btnDockTorch = findViewById(R.id.btn_dock_torch)
        btnDockMic = findViewById(R.id.btn_dock_mic)
        btnDockMirror = findViewById(R.id.btn_dock_mirror)
        btnDockRotate180 = findViewById(R.id.btn_dock_rotate_180)
        btnDockDim = findViewById(R.id.btn_dock_dim)
        btnDockPro = findViewById(R.id.btn_dock_pro)

        proControlsLayout = findViewById(R.id.pro_controls_layout)
        seekIso = findViewById(R.id.seek_iso)
        lblIso = findViewById(R.id.lbl_iso)
        seekEv = findViewById(R.id.seek_ev)
        lblEv = findViewById(R.id.lbl_ev)
        seekWb = findViewById(R.id.seek_wb)
        lblWb = findViewById(R.id.lbl_wb)
        seekFocus = findViewById(R.id.seek_focus)
        lblFocus = findViewById(R.id.lbl_focus)

        setupEventListeners()
    }

    private fun setupUsbReceiver() {
        usbReceiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context?, intent: Intent?) {
                val action = intent?.action ?: return
                if (action == "android.hardware.usb.action.USB_STATE") {
                    val connected = intent.getBooleanExtra("connected", false)
                    updateUsbState(connected)
                } else if (action == Intent.ACTION_POWER_CONNECTED || action == Intent.ACTION_POWER_DISCONNECTED) {
                    checkUsbCableState()
                }
            }
        }
        val filter = IntentFilter().apply {
            addAction("android.hardware.usb.action.USB_STATE")
            addAction(Intent.ACTION_POWER_CONNECTED)
            addAction(Intent.ACTION_POWER_DISCONNECTED)
        }
        registerReceiver(usbReceiver, filter)
    }

    private fun checkUsbCableState() {
        try {
            val batteryIntent = registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
            val plugged = batteryIntent?.getIntExtra(BatteryManager.EXTRA_PLUGGED, -1) ?: -1
            val isPlugged = (plugged == BatteryManager.BATTERY_PLUGGED_USB || plugged == BatteryManager.BATTERY_PLUGGED_AC)
            updateUsbState(isPlugged)
        } catch (e: Exception) {
            updateUsbState(false)
        }
    }

    private fun updateUsbState(connected: Boolean) {
        isUsbCableConnected = connected
        runOnUiThread {
            if (!connected && isUsbMode) {
                // If in USB mode and cable unplugs, auto fallback to Wi-Fi
                setConnectionMode(usb = false)
                Toast.makeText(this@MainActivity, "Cable USB desconectado. Cambiando a Wi-Fi...", Toast.LENGTH_LONG).show()
            }
            updateConnectionButtonsUI()
        }
    }

    private fun updateConnectionButtonsUI() {
        val toggle = layoutModeToggle ?: return
        val thumb = toggleThumb ?: return
        val txtThumb = txtThumbMode ?: return
        val txtHint = txtToggleHint ?: return

        toggle.alpha = if (isUsbCableConnected || !isUsbMode) 1.0f else 0.5f

        toggle.post {
            val totalWidth = toggle.width
            val thumbWidth = thumb.width
            val padding = (3 * resources.displayMetrics.density).toInt()
            val maxTravel = (totalWidth - thumbWidth - (padding * 2)).toFloat().coerceAtLeast(0f)

            if (isUsbMode) {
                thumb.animate().translationX(0f).setDuration(200).start()
                txtThumb.text = "USB"
                txtHint.text = "WiFi"
                (txtHint.layoutParams as? FrameLayout.LayoutParams)?.gravity = android.view.Gravity.END or android.view.Gravity.CENTER_VERTICAL
                txtHint.requestLayout()
            } else {
                thumb.animate().translationX(maxTravel).setDuration(200).start()
                txtThumb.text = "WiFi"
                txtHint.text = "USB"
                (txtHint.layoutParams as? FrameLayout.LayoutParams)?.gravity = android.view.Gravity.START or android.view.Gravity.CENTER_VERTICAL
                txtHint.requestLayout()
            }
        }
    }

    private fun setConnectionMode(usb: Boolean) {
        if (usb && !isUsbCableConnected) {
            Toast.makeText(this, "Conecta el cable USB a la PC para activar este modo", Toast.LENGTH_SHORT).show()
            return
        }

        isUsbMode = usb
        if (isUsbMode) {
            discoveryManager?.stop()
            sender?.setHost("127.0.0.1", 8088)
            Toast.makeText(this, "Modo Cable USB activado", Toast.LENGTH_SHORT).show()
        } else {
            wifiToastShown = false
            discoveryManager?.start()
            Toast.makeText(this, "Buscando PC por Wi-Fi automáticamente...", Toast.LENGTH_SHORT).show()
        }
        updateConnectionButtonsUI()
    }

    private fun setupOrientationListener() {
        orientationListener = object : OrientationEventListener(this) {
            override fun onOrientationChanged(orientation: Int) {
                if (orientation == ORIENTATION_UNKNOWN) return

                val newAngle = when (orientation) {
                    in 45..134 -> 180   // Reverse landscape
                    in 135..224 -> 270  // Inverted portrait
                    in 225..314 -> 0    // Standard landscape
                    else -> 90          // Standard vertical portrait
                }

                if (newAngle != physicalOrientationDegrees) {
                    physicalOrientationDegrees = newAngle
                    updatePreviewTransform()
                }
            }
        }
        if (orientationListener?.canDetectOrientation() == true) {
            orientationListener?.enable()
        }
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        updatePreviewTransform()
    }

    private fun updatePreviewTransform() {
        val tv = textureView ?: return

        val displayRotation = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            display?.rotation ?: Surface.ROTATION_0
        } else {
            @Suppress("DEPRECATION")
            windowManager.defaultDisplay.rotation
        }

        val isScreenLandscape = (resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE)
        if (isScreenLandscape) {
            tv.setAspectRatio(16, 9)
        } else {
            tv.setAspectRatio(9, 16)
        }

        val isFront = cameraPipeline?.getCurrentLensFacing() == 1
        if (tv.width > 0 && tv.height > 0) {
            tv.configureTransform(tv.width, tv.height, displayRotation, isFront, manualMirror)
        }

        // Map configuration & display rotation to exact stream orientation metadata:
        // 90 / 270 = Vertical Streaming (1080x1920)
        // 0 / 180  = Horizontal Streaming (1920x1080)
        val baseRot = if (isScreenLandscape) {
            if (displayRotation == Surface.ROTATION_270 || displayRotation == Surface.ROTATION_180) 180 else 0
        } else {
            if (displayRotation == Surface.ROTATION_180) 270 else 90
        }
        physicalOrientationDegrees = if (manualRotation180) (baseRot + 180) % 360 else baseRot
    }

    private fun toggleDockTrayVisibility() {
        isDockTrayVisible = !isDockTrayVisible
        val topBar = findViewById<View>(R.id.top_bar)
        val dock = findViewById<View>(R.id.video_quick_dock)
        val bottomPanel = findViewById<View>(R.id.bottom_panel)

        if (isDockTrayVisible) {
            topBar?.visibility = View.VISIBLE
            dock?.visibility = View.VISIBLE
            bottomPanel?.visibility = View.VISIBLE
            topBar?.animate()?.alpha(1.0f)?.translationY(0f)?.setDuration(200)?.start()
            dock?.animate()?.alpha(1.0f)?.translationY(0f)?.setDuration(200)?.start()
            bottomPanel?.animate()?.alpha(1.0f)?.translationY(0f)?.setDuration(200)?.start()
        } else {
            topBar?.animate()?.alpha(0.0f)?.translationY(-50f)?.setDuration(200)?.withEndAction {
                topBar.visibility = View.GONE
            }?.start()
            dock?.animate()?.alpha(0.0f)?.translationY(60f)?.setDuration(200)?.withEndAction {
                dock.visibility = View.GONE
            }?.start()
            bottomPanel?.animate()?.alpha(0.0f)?.translationY(60f)?.setDuration(200)?.withEndAction {
                bottomPanel.visibility = View.GONE
                proControlsLayout?.visibility = View.GONE
            }?.start()
        }
    }

    private fun setDimScreen(dim: Boolean, notifyPeer: Boolean = true) {
        isDimScreenActive = dim
        runOnUiThread {
            val lp = window.attributes
            if (dim) {
                // 1% brightness keeps AMOLED / LCD backlight on without triggering OS sleep/lock
                lp.screenBrightness = 0.01f
                window.attributes = lp
                dimScreenOverlay?.visibility = View.VISIBLE
                btnDockDim?.imageTintList = ColorStateList.valueOf(COLOR_LIGHT_GREEN)
            } else {
                lp.screenBrightness = WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
                window.attributes = lp
                dimScreenOverlay?.visibility = View.GONE
                btnDockDim?.imageTintList = ColorStateList.valueOf(Color.WHITE)
            }
        }
        if (notifyPeer) {
            sender?.sendDimState(dim)
        }
    }

    private fun setupEventListeners() {
        // Screen Tap anywhere to Hide / Show Bottom Button Tray
        val tapListener = View.OnClickListener {
            if (isDimScreenActive) {
                setDimScreen(false, notifyPeer = true)
            } else {
                toggleDockTrayVisibility()
            }
        }
        textureView?.setOnClickListener(tapListener)
        findViewById<View>(R.id.root_container)?.setOnClickListener(tapListener)

        // USB / Wi-Fi Circular Pill Toggle
        layoutModeToggle?.setOnClickListener {
            setConnectionMode(usb = !isUsbMode)
        }

        // Dim Screen (Atenuar pantalla) - Dismiss when touching dim overlay
        dimScreenOverlay?.setOnClickListener {
            setDimScreen(false, notifyPeer = true)
        }

        // Bottom Quick Dock Controls (Matching Desktop Studio)
        btnDockFlip?.setOnClickListener {
            cameraPipeline?.switchCamera()
            updatePreviewTransform()
        }

        btnDockTorch?.setOnClickListener {
            val isTorch = cameraPipeline?.isTorchActive() ?: false
            cameraPipeline?.setTorch(!isTorch)
            btnDockTorch?.imageTintList = ColorStateList.valueOf(
                if (!isTorch) COLOR_LIGHT_GREEN else Color.WHITE
            )
        }

        btnDockMic?.setOnClickListener {
            isMicEnabled = !isMicEnabled
            btnDockMic?.imageTintList = ColorStateList.valueOf(
                if (isMicEnabled) Color.WHITE else Color.parseColor("#EF4444")
            )
            Toast.makeText(this, if (isMicEnabled) "Micrófono activado" else "Micrófono silenciado", Toast.LENGTH_SHORT).show()
        }

        btnDockMirror?.setOnClickListener {
            manualMirror = !manualMirror
            btnDockMirror?.imageTintList = ColorStateList.valueOf(
                if (manualMirror) COLOR_LIGHT_GREEN else Color.WHITE
            )
            updatePreviewTransform()
            Toast.makeText(this, if (manualMirror) "Espejo activado" else "Espejo normal", Toast.LENGTH_SHORT).show()
        }

        btnDockRotate180?.setOnClickListener {
            manualRotation180 = !manualRotation180
            btnDockRotate180?.imageTintList = ColorStateList.valueOf(
                if (manualRotation180) COLOR_LIGHT_GREEN else Color.WHITE
            )
            updatePreviewTransform()
            Toast.makeText(this, if (manualRotation180) "Rotación 180° invertida" else "Rotación normal", Toast.LENGTH_SHORT).show()
        }

        btnDockDim?.setOnClickListener {
            setDimScreen(!isDimScreenActive, notifyPeer = true)
        }

        btnDockPro?.setOnClickListener {
            val isVisible = proControlsLayout?.visibility == View.VISIBLE
            proControlsLayout?.visibility = if (isVisible) View.GONE else View.VISIBLE
            btnDockPro?.imageTintList = ColorStateList.valueOf(
                if (!isVisible) COLOR_LIGHT_GREEN else Color.WHITE
            )
        }

        // ISO Slider: Auto, 100, 200, 400, 800, 1600, 3200
        val isoValues = intArrayOf(-1, 100, 200, 400, 800, 1600, 3200)
        seekIso?.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                val iso = isoValues[progress.coerceIn(0, isoValues.size - 1)]
                lblIso?.text = if (iso == -1) "ISO: Auto" else "ISO: $iso"
                if (fromUser) cameraPipeline?.setIso(iso)
            }
            override fun onStartTrackingTouch(seekBar: SeekBar?) {}
            override fun onStopTrackingTouch(seekBar: SeekBar?) {}
        })

        // EV Slider: -6 to +6 steps (offset 6)
        seekEv?.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                val ev = progress - 6
                lblEv?.text = if (ev == 0) "EV: 0" else if (ev > 0) "EV: +$ev" else "EV: $ev"
                if (fromUser) cameraPipeline?.setExposureCompensation(ev)
            }
            override fun onStartTrackingTouch(seekBar: SeekBar?) {}
            override fun onStopTrackingTouch(seekBar: SeekBar?) {}
        })

        // WB Slider: 0=Auto, 1=Incandescent, 2=Fluorescent, 3=Daylight, 4=Cloudy, 5=Shade
        val wbNames = arrayOf("Auto", "Incandescent", "Fluorescent", "Daylight", "Cloudy", "Shade")
        seekWb?.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                val name = wbNames[progress.coerceIn(0, wbNames.size - 1)]
                lblWb?.text = "WB: $name"
                if (fromUser) cameraPipeline?.setWhiteBalance(progress)
            }
            override fun onStartTrackingTouch(seekBar: SeekBar?) {}
            override fun onStopTrackingTouch(seekBar: SeekBar?) {}
        })

        // Focus Slider: 0 = AF, 1..100 = Manual Focus
        seekFocus?.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                if (progress == 0) {
                    lblFocus?.text = "Focus: Auto"
                    if (fromUser) cameraPipeline?.setFocus(true, 0.0f)
                } else {
                    val dist = progress / 100.0f
                    lblFocus?.text = "Focus: %.2f".format(dist)
                    if (fromUser) cameraPipeline?.setFocus(false, dist)
                }
            }
            override fun onStartTrackingTouch(seekBar: SeekBar?) {}
            override fun onStopTrackingTouch(seekBar: SeekBar?) {}
        })
    }

    private fun checkCameraPermission(): Boolean {
        val cam = ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
        val mic = ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED
        return cam && mic
    }

    private fun requestCameraPermission() {
        ActivityCompat.requestPermissions(
            this,
            arrayOf(Manifest.permission.CAMERA, Manifest.permission.RECORD_AUDIO),
            CAMERA_PERMISSION_CODE
        )
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == CAMERA_PERMISSION_CODE && grantResults.isNotEmpty() && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
            setupStreamingPipeline()
        } else {
            Toast.makeText(this, "Se requieren permisos de cámara y micrófono para BouleCam", Toast.LENGTH_LONG).show()
        }
    }

    private fun getFriendlyDeviceName(): String {
        try {
            val devName = android.provider.Settings.Global.getString(contentResolver, android.provider.Settings.Global.DEVICE_NAME)
            if (!devName.isNullOrBlank()) return devName
        } catch (e: Exception) {}
        try {
            val devName = android.provider.Settings.Secure.getString(contentResolver, "bluetooth_name")
            if (!devName.isNullOrBlank()) return devName
        } catch (e: Exception) {}
        val manufacturer = android.os.Build.MANUFACTURER.replaceFirstChar { it.uppercase() }
        return "$manufacturer ${android.os.Build.MODEL}"
    }

    private var telemetryTimer: java.util.Timer? = null

    private fun getBatteryInfo(): Pair<Float, Float> {
        return try {
            val filter = IntentFilter(Intent.ACTION_BATTERY_CHANGED)
            val bIntent = registerReceiver(null, filter)
            val level = bIntent?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
            val scale = bIntent?.getIntExtra(BatteryManager.EXTRA_SCALE, -1) ?: -1
            val pct = if (level >= 0 && scale > 0) (level / scale.toFloat()) * 100f else -1.0f
            val tempRaw = bIntent?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0) ?: 0
            val tempC = tempRaw / 10.0f
            pct to tempC
        } catch (e: Exception) {
            -1.0f to 0.0f
        }
    }

    private fun startTelemetryTimer() {
        stopTelemetryTimer()
        telemetryTimer = java.util.Timer().apply {
            scheduleAtFixedRate(object : java.util.TimerTask() {
                override fun run() {
                    if (sender?.isConnected() == true) {
                        val (bat, temp) = getBatteryInfo()
                        val mask = cameraPipeline?.getLensesMask() ?: 3
                        val zoom = cameraPipeline?.getCurrentZoom() ?: 1.0f
                        val lens = cameraPipeline?.getCurrentLens() ?: 0
                        sender?.sendTelemetry(
                            batteryLevel = bat,
                            temperatureC = temp,
                            lensesMask = mask,
                            currentZoom = zoom,
                            currentLens = lens,
                            isDimmed = isDimScreenActive,
                            isTorchOn = cameraPipeline?.isTorchActive() ?: false
                        )
                    }
                }
            }, 500, 2000)
        }
    }

    private fun stopTelemetryTimer() {
        try {
            telemetryTimer?.cancel()
            telemetryTimer = null
        } catch (ignored: Exception) {}
    }

    private fun setupStreamingPipeline() {
        val devName = getFriendlyDeviceName()

        // 1. Initialize Network Streamer with Bidirectional Command Handler
        sender = StreamSender(
            host = "127.0.0.1", 
            port = 8088,
            deviceName = devName,
            onConnectionStateChanged = { connected ->
                runOnUiThread {
                    if (connected) {
                        val modeLabel = if (isUsbMode) "Cable USB" else "WiFi"
                        statusTextView?.text = "STATUS: EN VIVO - $modeLabel"
                        statusTextView?.setTextColor(COLOR_LIGHT_GREEN)
                        // START / RESUME RECORDING ONLY WHEN CONNECTED TO PC
                        encoder?.setSuspended(false)
                        encoder?.requestKeyFrame()
                        if (isMicEnabled) {
                            audioPipeline?.start()
                        }
                        startTelemetryTimer()
                    } else {
                        val searchingLabel = if (isUsbMode) "Cable USB" else "WiFi"
                        statusTextView?.text = "STATUS: BUSCANDO PC ($searchingLabel)..."
                        statusTextView?.setTextColor(COLOR_GRAY)
                        // PAUSE HARDWARE ENCODER AND AUDIO TO SAVE BATTERY AND PREVENT OVERHEATING!
                        encoder?.setSuspended(true)
                        audioPipeline?.stop()
                        stopTelemetryTimer()
                    }
                }
            },
            onCommandReceived = { cmd ->
                handleRemoteCommand(cmd)
            }
        )
        sender?.start()

        // 2. Start Auto-Discovery Manager for Wi-Fi & USB Auto-Detect
        discoveryManager = AutoDiscoveryManager(this) { device ->
            runOnUiThread {
                if (device.isUsb) {
                    isUsbCableConnected = true
                    updateConnectionButtonsUI()
                    if (isUsbMode) {
                        sender?.setHost("127.0.0.1", 8088)
                    }
                } else {
                    // Wi-Fi PC detected
                    if (!isUsbCableConnected || !isUsbMode) {
                        if (isUsbMode && !isUsbCableConnected) {
                            isUsbMode = false
                            updateConnectionButtonsUI()
                        }
                        if (sender?.getHost() != device.ip || sender?.getPort() != device.port) {
                            sender?.setHost(device.ip, device.port)
                        }
                        if (!wifiToastShown) {
                            wifiToastShown = true
                            Toast.makeText(this, "PC BouleCam encontrada por WiFi (${device.ip})", Toast.LENGTH_SHORT).show()
                        }
                    }
                }
            }
        }
        discoveryManager?.start()

        // 3. Initialize Hardware MediaCodec H.264 Encoder (1920x1080 60 FPS)
        encoder = HardwareEncoder(1920, 1080, 60, 8_000_000) { isKeyFrame, timestampUs, naluData ->
            if (sender?.isConnected() == true) {
                sender?.sendFrame(isKeyFrame, timestampUs, naluData, physicalOrientationDegrees)
            }
        }
        encoder?.start()
        encoder?.setSuspended(true) // Suspended until PC connection is established!

        // 3.5. Prepare Ultra Low Latency PCM Audio Pipeline (Starts only when connected)
        audioPipeline = AudioCapturePipeline(sampleRate = 48000) { pcmData, size ->
            if (isMicEnabled && sender?.isConnected() == true) {
                sender?.sendAudio(pcmData, size)
            }
        }

        // 4. Initialize TextureView Surface Listener with AutoFit transform
        textureView?.surfaceTextureListener = object : TextureView.SurfaceTextureListener {
            override fun onSurfaceTextureAvailable(surfaceTexture: SurfaceTexture, width: Int, height: Int) {
                surfaceTexture.setDefaultBufferSize(1920, 1080)
                updatePreviewTransform()
                previewSurface = Surface(surfaceTexture)
                startCameraPipeline()
            }

            override fun onSurfaceTextureSizeChanged(surfaceTexture: SurfaceTexture, width: Int, height: Int) {
                updatePreviewTransform()
            }

            override fun onSurfaceTextureDestroyed(surfaceTexture: SurfaceTexture): Boolean {
                cameraPipeline?.stop()
                cameraPipeline = null
                previewSurface?.release()
                previewSurface = null
                return true
            }

            override fun onSurfaceTextureUpdated(surfaceTexture: SurfaceTexture) {}
        }

        if (textureView?.isAvailable == true) {
            val st = textureView!!.surfaceTexture
            if (st != null) {
                st.setDefaultBufferSize(1920, 1080)
                previewSurface = Surface(st)
                updatePreviewTransform()
                startCameraPipeline()
            }
        }
    }

    private fun startCameraPipeline() {
        val encSurface = encoder?.inputSurface ?: return
        val pSurface = previewSurface ?: return

        cameraExecutor.execute {
            try {
                cameraPipeline?.stop()
                cameraPipeline = CameraCapturePipeline(
                    context = this@MainActivity,
                    previewSurface = pSurface,
                    encoderSurface = encSurface
                )
                cameraPipeline?.start()
            } catch (e: Exception) {
                // Silently recover if camera is momentarily unavailable
            }
        }
    }

    private fun handleRemoteCommand(cmd: CameraCommand) {
        runOnUiThread {
            when (cmd.action) {
                1 -> { // BOULECAM_ACTION_SET_LENS (0 = Back Main, 1 = Front, 2 = UltraWide, 3 = Tele/Macro)
                    cameraExecutor.execute {
                        cameraPipeline?.setLens(cmd.intParam1)
                        runOnUiThread { updatePreviewTransform() }
                    }
                }
                2 -> { // BOULECAM_ACTION_SET_TORCH (0 = Off, 1 = On)
                    val on = cmd.intParam1 != 0
                    cameraPipeline?.setTorch(on)
                    btnDockTorch?.imageTintList = ContextCompat.getColorStateList(
                        this, if (on) android.R.color.holo_orange_light else android.R.color.white
                    )
                }
                3 -> { // BOULECAM_ACTION_SET_ISO
                    cameraPipeline?.setIso(cmd.intParam1)
                    lblIso?.text = if (cmd.intParam1 == -1) "ISO: Auto" else "ISO: ${cmd.intParam1}"
                }
                4 -> { // BOULECAM_ACTION_SET_EXPOSURE (nanoseconds)
                    cameraPipeline?.setExposureTime(cmd.longParam1)
                }
                5 -> { // BOULECAM_ACTION_SET_EV
                    cameraPipeline?.setExposureCompensation(cmd.intParam1)
                    seekEv?.progress = (cmd.intParam1 + 6).coerceIn(0, 12)
                }
                6 -> { // BOULECAM_ACTION_SET_WB
                    cameraPipeline?.setWhiteBalance(cmd.intParam1)
                    seekWb?.progress = cmd.intParam1.coerceIn(0, 5)
                }
                7 -> { // BOULECAM_ACTION_SET_FOCUS
                    val auto = cmd.intParam1 == 0
                    cameraPipeline?.setFocus(auto, cmd.floatParam1)
                    seekFocus?.progress = if (auto) 0 else (cmd.floatParam1 * 100).toInt()
                }
                8 -> { // BOULECAM_ACTION_SET_MIC
                    isMicEnabled = cmd.intParam1 != 0
                    btnDockMic?.imageTintList = ContextCompat.getColorStateList(
                        this, if (isMicEnabled) android.R.color.white else android.R.color.holo_red_light
                    )
                }
                10 -> { // BOULECAM_ACTION_SET_DIM_SCREEN (0 = Normal, 1 = Dim)
                    setDimScreen(cmd.intParam1 != 0, notifyPeer = false)
                }
                11 -> { // BOULECAM_ACTION_SET_ZOOM (floatParam1 = zoom ratio)
                    cameraPipeline?.setZoom(cmd.floatParam1)
                }
            }
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        stopTelemetryTimer()
        BouleCamStreamService.stop(this)

        try {
            if (usbReceiver != null) {
                unregisterReceiver(usbReceiver)
                usbReceiver = null
            }
        } catch (ignored: Exception) {}

        orientationListener?.disable()
        discoveryManager?.stop()
        cameraExecutor.execute {
            cameraPipeline?.stop()
        }
        cameraExecutor.shutdown()
        audioPipeline?.stop()
        encoder?.stop()
        sender?.stop()
        previewSurface?.release()
    }
}
