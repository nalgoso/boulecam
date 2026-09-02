package com.boulecam.camera

import android.annotation.SuppressLint
import android.content.Context
import android.hardware.camera2.*
import android.os.Handler
import android.os.HandlerThread
import android.util.Range
import android.util.Size
import android.view.Surface

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

    private var currentLensFacing = CameraCharacteristics.LENS_FACING_BACK
    private var isTorchOn = false
    private var manualIso: Int = -1
    private var manualExposureNs: Long = -1L
    private var manualEv: Int = 0
    private var manualWbMode: Int = CaptureRequest.CONTROL_AWB_MODE_AUTO
    private var manualFocusAuto: Boolean = true
    private var manualFocusDistance: Float = 0.0f

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
        currentLensFacing = if (currentLensFacing == CameraCharacteristics.LENS_FACING_BACK) {
            CameraCharacteristics.LENS_FACING_FRONT
        } else {
            CameraCharacteristics.LENS_FACING_BACK
        }
        isTorchOn = false
        reopenCamera()
    }

    fun setLensFacing(facing: Int) {
        val target = if (facing == 1) CameraCharacteristics.LENS_FACING_FRONT else CameraCharacteristics.LENS_FACING_BACK
        if (currentLensFacing != target) {
            currentLensFacing = target
            isTorchOn = false
            reopenCamera()
        }
    }

    fun getCurrentLensFacing(): Int = if (currentLensFacing == CameraCharacteristics.LENS_FACING_FRONT) 1 else 0

    fun getSensorOrientation(): Int {
        val cameraId = getCameraId(currentLensFacing) ?: return 90
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
        val cameraId = getCameraId(currentLensFacing) ?: return

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
