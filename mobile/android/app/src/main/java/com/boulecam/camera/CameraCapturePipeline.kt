package com.boulecam.camera

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Rect
import android.hardware.camera2.*
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.util.Range
import android.util.Size
import android.view.Surface

data class LensInfo(val type: Int, val cameraId: String, val label: String, val facing: Int)

class CameraCapturePipeline(
    private val context: Context,
    private val previewSurface: Surface?,
    private val encoderSurface: Surface,
    private val targetResolution: Size = Size(1920, 1080),
    private val targetFps: Int = 60
) {
    private val cameraManager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
    private var cameraDevice: CameraDevice? = null
    private var captureSession: CameraCaptureSession? = null
    private var currentRequestBuilder: CaptureRequest.Builder? = null

    private var backgroundThread: HandlerThread? = null
    private var backgroundHandler: Handler? = null

    val availableLenses = mutableListOf<LensInfo>()
    private var lensesMask: Int = 1
    private var activeCameraId: String? = null
    private var currentLensType: Int = 0 // 0=Back, 1=Front, 2=UltraWide, 3=Tele/Macro
    private var currentLensFacing = CameraCharacteristics.LENS_FACING_BACK
    private var currentZoomRatio = 1.0f

    private var isTorchOn = false
    private var manualIso: Int = -1
    private var manualExposureNs: Long = -1L
    private var manualEv: Int = 0
    private var manualWbMode: Int = CaptureRequest.CONTROL_AWB_MODE_AUTO
    private var manualFocusAuto: Boolean = true
    private var manualFocusDistance: Float = 0.0f

    init {
        discoverLenses()
    }

    private fun discoverLenses() {
        availableLenses.clear()
        var mask = 0
        try {
            val backCameras = mutableListOf<Pair<String, Float>>()
            val frontCameras = mutableListOf<String>()

            for (id in cameraManager.cameraIdList) {
                val chars = cameraManager.getCameraCharacteristics(id)
                val facing = chars.get(CameraCharacteristics.LENS_FACING)
                val focalLengths = chars.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)
                val focal = focalLengths?.firstOrNull() ?: 4.0f

                if (facing == CameraCharacteristics.LENS_FACING_BACK) {
                    backCameras.add(id to focal)
                } else if (facing == CameraCharacteristics.LENS_FACING_FRONT) {
                    frontCameras.add(id)
                }
            }

            if (backCameras.isNotEmpty()) {
                backCameras.sortBy { it.second }
                if (backCameras.size == 1) {
                    availableLenses.add(LensInfo(0, backCameras[0].first, "Trasera Principal", CameraCharacteristics.LENS_FACING_BACK))
                    mask = mask or 1
                } else if (backCameras.size == 2) {
                    val ultra = backCameras[0]
                    val main = backCameras[1]
                    availableLenses.add(LensInfo(0, main.first, "Trasera Principal", CameraCharacteristics.LENS_FACING_BACK))
                    availableLenses.add(LensInfo(2, ultra.first, "Gran Angular", CameraCharacteristics.LENS_FACING_BACK))
                    mask = mask or 1 or 4
                } else {
                    val ultra = backCameras[0]
                    val main = backCameras[1]
                    val tele = backCameras.last()
                    availableLenses.add(LensInfo(0, main.first, "Trasera Principal", CameraCharacteristics.LENS_FACING_BACK))
                    availableLenses.add(LensInfo(2, ultra.first, "Gran Angular", CameraCharacteristics.LENS_FACING_BACK))
                    availableLenses.add(LensInfo(3, tele.first, "Tele / Macro", CameraCharacteristics.LENS_FACING_BACK))
                    mask = mask or 1 or 4 or 8
                }
            }

            if (frontCameras.isNotEmpty()) {
                availableLenses.add(LensInfo(1, frontCameras[0], "Frontal", CameraCharacteristics.LENS_FACING_FRONT))
                mask = mask or 2
            }
        } catch (e: Exception) {
            e.printStackTrace()
            mask = 3
        }
        lensesMask = if (mask == 0) 3 else mask
        activeCameraId = availableLenses.firstOrNull()?.cameraId
    }

    fun getLensesMask(): Int = lensesMask
    fun getCurrentZoom(): Float = currentZoomRatio
    fun getCurrentLens(): Int = currentLensType

    fun start() {
        startBackgroundThread()
        openCamera()
    }

    private fun startBackgroundThread() {
        backgroundThread = HandlerThread("BouleCamCameraThread").apply {
            start()
            backgroundHandler = Handler(looper)
        }
    }

    fun switchCamera() {
        val nextType = if (currentLensFacing == CameraCharacteristics.LENS_FACING_BACK) 1 else 0
        setLens(nextType)
    }

    fun setLensFacing(facing: Int) {
        val targetType = if (facing == 1) 1 else 0
        setLens(targetType)
    }

    fun setLens(lensType: Int) {
        val target = availableLenses.find { it.type == lensType }
            ?: (if (lensType == 1) availableLenses.find { it.facing == CameraCharacteristics.LENS_FACING_FRONT }
                else availableLenses.find { it.facing == CameraCharacteristics.LENS_FACING_BACK })
            ?: return

        if (activeCameraId != target.cameraId) {
            activeCameraId = target.cameraId
            currentLensFacing = target.facing
            currentLensType = target.type
            currentZoomRatio = 1.0f
            isTorchOn = false
            reopenCamera()
        }
    }

    fun setZoom(ratio: Float) {
        val clamped = ratio.coerceIn(1.0f, 10.0f)
        if (Math.abs(currentZoomRatio - clamped) >= 0.02f) {
            currentZoomRatio = clamped
            applyAllSettings()
        }
    }

    fun getCurrentLensFacing(): Int = if (currentLensFacing == CameraCharacteristics.LENS_FACING_FRONT) 1 else 0

    fun getSensorOrientation(): Int {
        val cameraId = activeCameraId ?: getCameraId(currentLensFacing) ?: return 90
        return try {
            val characteristics = cameraManager.getCameraCharacteristics(cameraId)
            characteristics.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 90
        } catch (e: Exception) {
            90
        }
    }

    private fun reopenCamera() {
        try {
            captureSession?.stopRepeating()
            captureSession?.close()
            captureSession = null
            cameraDevice?.close()
            cameraDevice = null
        } catch (ignored: Exception) {}

        openCamera()
    }

    @SuppressLint("MissingPermission")
    private fun openCamera() {
        val cameraId = activeCameraId ?: getCameraId(currentLensFacing) ?: return

        try {
            cameraManager.openCamera(cameraId, object : CameraDevice.StateCallback() {
                override fun onOpened(camera: CameraDevice) {
                    cameraDevice = camera
                    createCaptureSession(cameraId)
                }

                override fun onDisconnected(camera: CameraDevice) {
                    camera.close()
                    cameraDevice = null
                }

                override fun onError(camera: CameraDevice, error: Int) {
                    camera.close()
                    cameraDevice = null
                }
            }, backgroundHandler)
        } catch (e: Exception) {
            e.printStackTrace()
        }
    }

    private fun createCaptureSession(cameraId: String) {
        val device = cameraDevice ?: return
        val surfaces = mutableListOf<Surface>(encoderSurface)
        if (previewSurface != null && previewSurface.isValid) {
            surfaces.add(previewSurface)
        }

        try {
            val characteristics = cameraManager.getCameraCharacteristics(cameraId)
            val fpsRanges = characteristics.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES) ?: emptyArray()
            
            val bestFpsRange = fpsRanges.find { it.upper >= targetFps } 
                ?: fpsRanges.maxByOrNull { it.upper } 
                ?: Range(30, 30)

            device.createCaptureSession(surfaces, object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(session: CameraCaptureSession) {
                    captureSession = session
                    try {
                        val requestBuilder = device.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                            addTarget(encoderSurface)
                            if (previewSurface != null && previewSurface.isValid) {
                                addTarget(previewSurface)
                            }
                            set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, bestFpsRange)
                        }

                        currentRequestBuilder = requestBuilder
                        applyAllSettings()
                    } catch (e: Exception) {
                        e.printStackTrace()
                    }
                }

                override fun onConfigureFailed(session: CameraCaptureSession) {}
            }, backgroundHandler)
        } catch (e: Exception) {
            e.printStackTrace()
        }
    }

    fun setTorch(enabled: Boolean) {
        isTorchOn = enabled
        applyAllSettings()
    }

    fun isTorchActive(): Boolean = isTorchOn

    fun setIso(iso: Int) {
        manualIso = iso
        applyAllSettings()
    }

    fun setExposureTime(ns: Long) {
        manualExposureNs = ns
        applyAllSettings()
    }

    fun setExposureCompensation(ev: Int) {
        manualEv = ev
        applyAllSettings()
    }

    fun setWhiteBalance(mode: Int) {
        manualWbMode = when (mode) {
            1 -> CaptureRequest.CONTROL_AWB_MODE_INCANDESCENT
            2 -> CaptureRequest.CONTROL_AWB_MODE_FLUORESCENT
            3 -> CaptureRequest.CONTROL_AWB_MODE_DAYLIGHT
            4 -> CaptureRequest.CONTROL_AWB_MODE_CLOUDY_DAYLIGHT
            5 -> CaptureRequest.CONTROL_AWB_MODE_SHADE
            else -> CaptureRequest.CONTROL_AWB_MODE_AUTO
        }
        applyAllSettings()
    }

    fun setFocus(auto: Boolean, distance: Float) {
        manualFocusAuto = auto
        manualFocusDistance = distance
        applyAllSettings()
    }

    private fun applyAllSettings() {
        val session = captureSession ?: return
        val builder = currentRequestBuilder ?: return

        try {
            // 1. Torch / Flash
            if (currentLensFacing == CameraCharacteristics.LENS_FACING_BACK) {
                if (isTorchOn) {
                    builder.set(CaptureRequest.FLASH_MODE, CaptureRequest.FLASH_MODE_TORCH)
                } else {
                    builder.set(CaptureRequest.FLASH_MODE, CaptureRequest.FLASH_MODE_OFF)
                }
            }

            // 2. ISO & Exposure (Manual vs Auto AE)
            if (manualIso > 0 && manualExposureNs > 0) {
                builder.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_OFF)
                builder.set(CaptureRequest.SENSOR_SENSITIVITY, manualIso)
                builder.set(CaptureRequest.SENSOR_EXPOSURE_TIME, manualExposureNs)
            } else {
                builder.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON)
                builder.set(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, manualEv)
            }

            // 3. White Balance
            builder.set(CaptureRequest.CONTROL_AWB_MODE, manualWbMode)

            // 4. Focus Mode
            if (manualFocusAuto) {
                builder.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
            } else {
                builder.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_OFF)
                // 0.0 = infinity, max distance = macro
                builder.set(CaptureRequest.LENS_FOCUS_DISTANCE, manualFocusDistance * 10.0f)
            }

            // 5. Digital Zoom
            val targetId = activeCameraId ?: getCameraId(currentLensFacing)
            if (targetId != null) {
                try {
                    val chars = cameraManager.getCameraCharacteristics(targetId)
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                        val zoomRange = chars.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)
                        val maxZoom = zoomRange?.upper ?: 10.0f
                        val minZoom = zoomRange?.lower ?: 1.0f
                        val clamped = currentZoomRatio.coerceIn(minZoom, maxZoom)
                        builder.set(CaptureRequest.CONTROL_ZOOM_RATIO, clamped)
                    } else {
                        val sensorRect = chars.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE)
                        if (sensorRect != null && currentZoomRatio > 1.0f) {
                            val cropW = (sensorRect.width() / currentZoomRatio).toInt()
                            val cropH = (sensorRect.height() / currentZoomRatio).toInt()
                            val cropX = sensorRect.left + (sensorRect.width() - cropW) / 2
                            val cropY = sensorRect.top + (sensorRect.height() - cropH) / 2
                            builder.set(CaptureRequest.SCALER_CROP_REGION, Rect(cropX, cropY, cropX + cropW, cropY + cropH))
                        } else if (sensorRect != null) {
                            builder.set(CaptureRequest.SCALER_CROP_REGION, sensorRect)
                        }
                    }
                } catch (ignored: Exception) {}
            }

            session.setRepeatingRequest(builder.build(), null, backgroundHandler)
        } catch (e: Exception) {
            e.printStackTrace()
        }
    }

    private fun getCameraId(facing: Int): String? {
        try {
            for (id in cameraManager.cameraIdList) {
                val characteristics = cameraManager.getCameraCharacteristics(id)
                if (characteristics.get(CameraCharacteristics.LENS_FACING) == facing) {
                    return id
                }
            }
        } catch (e: Exception) {
            e.printStackTrace()
        }
        return cameraManager.cameraIdList.firstOrNull()
    }

    fun stop() {
        try {
            captureSession?.stopRepeating()
            captureSession?.close()
            captureSession = null

            cameraDevice?.close()
            cameraDevice = null
        } catch (ignored: Exception) {}

        backgroundThread?.quitSafely()
        try {
            backgroundThread?.join()
            backgroundThread = null
            backgroundHandler = null
        } catch (ignored: Exception) {}
    }
}
