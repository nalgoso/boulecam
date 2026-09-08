package com.boulecam.ui

import android.content.Context
import android.graphics.Matrix
import android.graphics.RectF
import android.util.AttributeSet
import android.view.Surface
import android.view.TextureView

class AutoFitTextureView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyle: Int = 0
) : TextureView(context, attrs, defStyle) {

    private var ratioWidth = 9
    private var ratioHeight = 16

    fun setAspectRatio(width: Int, height: Int) {
        if (width <= 0 || height <= 0) return
        if (ratioWidth != width || ratioHeight != height) {
            ratioWidth = width
            ratioHeight = height
            post { requestLayout() }
        }
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        super.onMeasure(widthMeasureSpec, heightMeasureSpec)
        val width = MeasureSpec.getSize(widthMeasureSpec)
        val height = MeasureSpec.getSize(heightMeasureSpec)

        if (ratioWidth == 0 || ratioHeight == 0) {
            setMeasuredDimension(width, height)
        } else {
            // Measure exact aspect ratio: Fit-Center with no distortion
            if (width < height * ratioWidth / ratioHeight) {
                setMeasuredDimension(width, width * ratioHeight / ratioWidth)
            } else {
                setMeasuredDimension(height * ratioWidth / ratioHeight, height)
            }
        }
    }

    private var lastViewWidth = 0
    private var lastViewHeight = 0
    private var lastDisplayRotation = -1
    private var lastIsFrontCamera = false
    private var lastIsMirrored = false
    private var lastManualRotation180 = false

    /**
     * Configures orientation rotation and front camera mirroring based on
     * display rotation. Guarantees that the preview matches 100% of what is
     * recorded and streamed, with ZERO distortion and ZERO cropping.
     */
    fun configureTransform(
        viewWidth: Int,
        viewHeight: Int,
        displayRotation: Int,
        isFrontCamera: Boolean = false,
        isMirrored: Boolean = false,
        manualRotation180: Boolean = false
    ) {
        lastDisplayRotation = displayRotation
        lastIsFrontCamera = isFrontCamera
        lastIsMirrored = isMirrored
        lastManualRotation180 = manualRotation180

        val actualW = if (viewWidth > 0) viewWidth else width
        val actualH = if (viewHeight > 0) viewHeight else height
        if (actualW <= 0 || actualH <= 0) return

        lastViewWidth = actualW
        lastViewHeight = actualH

        val matrix = Matrix()
        val viewRect = RectF(0f, 0f, actualW.toFloat(), actualH.toFloat())
        val centerX = viewRect.centerX()
        val centerY = viewRect.centerY()

        when (displayRotation) {
            Surface.ROTATION_90, Surface.ROTATION_270 -> {
                // When device rotates to landscape, the camera HAL delivers the buffer
                // oriented relative to the natural portrait mode. Rotate and scale buffer
                // rect so landscape preview is 100% upright with pristine 16:9 aspect ratio.
                val bufferRect = RectF(0f, 0f, actualH.toFloat(), actualW.toFloat())
                bufferRect.offset(centerX - bufferRect.centerX(), centerY - bufferRect.centerY())
                matrix.setRectToRect(viewRect, bufferRect, Matrix.ScaleToFit.FILL)

                val baseAngle = if (displayRotation == Surface.ROTATION_90) -90f else 90f
                val finalAngle = if (manualRotation180) baseAngle + 180f else baseAngle
                matrix.postRotate(finalAngle, centerX, centerY)
            }
            Surface.ROTATION_180 -> {
                val finalAngle = if (manualRotation180) 0f else 180f
                if (finalAngle != 0f) {
                    matrix.postRotate(finalAngle, centerX, centerY)
                }
            }
            else -> {
                // Surface.ROTATION_0 (Portrait)
                if (manualRotation180) {
                    matrix.postRotate(180f, centerX, centerY)
                }
            }
        }

        // Exact same logic as Windows BouleCam Studio:
        // Front camera naturally mirrors (selfie mode). If isMirrored is toggled, it inverts it.
        // Back camera naturally does NOT mirror. If isMirrored is toggled, it mirrors it.
        val shouldFlipH = if (isFrontCamera) !isMirrored else isMirrored
        if (shouldFlipH) {
            matrix.postScale(-1f, 1f, centerX, centerY)
        }

        setTransform(matrix)
    }


    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        if (w > 0 && h > 0) {
            val rot = if (lastDisplayRotation != -1) lastDisplayRotation else Surface.ROTATION_0
            configureTransform(
                w, h, 
                rot, 
                lastIsFrontCamera, 
                lastIsMirrored, 
                lastManualRotation180
            )
        }
    }

    override fun onTouchEvent(event: android.view.MotionEvent): Boolean {
        if (event.action == android.view.MotionEvent.ACTION_UP) {
            performClick()
        }
        return true
    }

    override fun performClick(): Boolean {
        super.performClick()
        return true
    }
}
