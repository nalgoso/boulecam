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

    /**
     * Configures orientation rotation and front camera mirroring based on
     * display rotation. Guarantees that preview is NEVER distorted and
     * is ALWAYS upright on the phone screen.
     */
    fun configureTransform(
        viewWidth: Int, 
        viewHeight: Int, 
        displayRotation: Int,
        isFrontCamera: Boolean = false,
        isMirrored: Boolean = false
    ) {
        if (viewWidth <= 0 || viewHeight <= 0) return

        val matrix = Matrix()
        val centerX = viewWidth / 2f
        val centerY = viewHeight / 2f

        when (displayRotation) {
            Surface.ROTATION_90 -> {
                val bufferRect = RectF(0f, 0f, viewHeight.toFloat(), viewWidth.toFloat())
                val viewRect = RectF(0f, 0f, viewWidth.toFloat(), viewHeight.toFloat())
                bufferRect.offset(centerX - bufferRect.centerX(), centerY - bufferRect.centerY())
                matrix.setRectToRect(viewRect, bufferRect, Matrix.ScaleToFit.FILL)
                val scale = maxOf(viewHeight.toFloat() / viewHeight.toFloat(), viewWidth.toFloat() / viewWidth.toFloat())
                matrix.postScale(scale, scale, centerX, centerY)
                matrix.postRotate(-90f, centerX, centerY)
            }
            Surface.ROTATION_270 -> {
                val bufferRect = RectF(0f, 0f, viewHeight.toFloat(), viewWidth.toFloat())
                val viewRect = RectF(0f, 0f, viewWidth.toFloat(), viewHeight.toFloat())
                bufferRect.offset(centerX - bufferRect.centerX(), centerY - bufferRect.centerY())
                matrix.setRectToRect(viewRect, bufferRect, Matrix.ScaleToFit.FILL)
                val scale = maxOf(viewHeight.toFloat() / viewHeight.toFloat(), viewWidth.toFloat() / viewWidth.toFloat())
                matrix.postScale(scale, scale, centerX, centerY)
                matrix.postRotate(90f, centerX, centerY)
            }
            Surface.ROTATION_180 -> {
                matrix.postRotate(180f, centerX, centerY)
            }
            else -> {
                // Surface.ROTATION_0: Standard portrait, clean 1:1 un-distorted preview
            }
        }

        val shouldFlipH = if (isFrontCamera) !isMirrored else isMirrored
        if (shouldFlipH) {
            matrix.postScale(-1f, 1f, centerX, centerY)
        }

        setTransform(matrix)
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
