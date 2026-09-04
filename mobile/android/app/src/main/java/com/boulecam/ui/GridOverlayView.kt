package com.boulecam.ui

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.util.AttributeSet
import android.view.View

/**
 * Overlay view that renders composition guides (Rule of Thirds 3x3 grid,
 * center crosshair, and framing boundary) over the camera preview.
 *
 * This overlay is rendered strictly on the phone screen and is NEVER sent
 * through the video encoder to OBS or the PC.
 */
class GridOverlayView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyle: Int = 0
) : View(context, attrs, defStyle) {

    private var ratioWidth = 9
    private var ratioHeight = 16

    private val gridPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = 0x99FFFFFF.toInt() // Semi-transparent white
        style = Paint.Style.STROKE
        strokeWidth = 1.5f * resources.displayMetrics.density
    }

    private val shadowPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = 0x55000000.toInt() // Subtle dark drop shadow
        style = Paint.Style.STROKE
        strokeWidth = 2.5f * resources.displayMetrics.density
    }

    private val borderPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = 0x55FFFFFF.toInt()
        style = Paint.Style.STROKE
        strokeWidth = 1.0f * resources.displayMetrics.density
    }

    private val crosshairPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = 0xDD4ADE80.toInt() // Vibrant green accent for center crosshair
        style = Paint.Style.STROKE
        strokeWidth = 2.0f * resources.displayMetrics.density
    }

    init {
        // Allow touches to pass through directly to preview / root container
        isClickable = false
        isFocusable = false
    }

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
            // Guarantee exact pixel-match with AutoFitTextureView
            if (width < height * ratioWidth / ratioHeight) {
                setMeasuredDimension(width, width * ratioHeight / ratioWidth)
            } else {
                setMeasuredDimension(height * ratioWidth / ratioHeight, height)
            }
        }
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val w = width.toFloat()
        val h = height.toFloat()
        if (w <= 0f || h <= 0f) return

        // 1. Outer Active Frame Border (Delimits exact recording frame)
        canvas.drawRect(0.5f, 0.5f, w - 0.5f, h - 0.5f, borderPaint)

        // 2. Rule of Thirds (3x3 Grid)
        val x1 = w / 3.0f
        val x2 = 2.0f * w / 3.0f
        val y1 = h / 3.0f
        val y2 = 2.0f * h / 3.0f

        // Draw shadow first for contrast on light scenes
        canvas.drawLine(x1, 0f, x1, h, shadowPaint)
        canvas.drawLine(x2, 0f, x2, h, shadowPaint)
        canvas.drawLine(0f, y1, w, y1, shadowPaint)
        canvas.drawLine(0f, y2, w, y2, shadowPaint)

        // Draw foreground grid lines
        canvas.drawLine(x1, 0f, x1, h, gridPaint)
        canvas.drawLine(x2, 0f, x2, h, gridPaint)
        canvas.drawLine(0f, y1, w, y1, gridPaint)
        canvas.drawLine(0f, y2, w, y2, gridPaint)

        // 3. Center Crosshair (+) for precise tripod leveling & horizon alignment
        val cx = w / 2.0f
        val cy = h / 2.0f
        val armLen = 14.0f * resources.displayMetrics.density

        // Crosshair shadow
        canvas.drawLine(cx - armLen, cy, cx + armLen, cy, shadowPaint)
        canvas.drawLine(cx, cy - armLen, cx, cy + armLen, shadowPaint)

        // Crosshair foreground (bright green indicator)
        canvas.drawLine(cx - armLen, cy, cx + armLen, cy, crosshairPaint)
        canvas.drawLine(cx, cy - armLen, cx, cy + armLen, crosshairPaint)

        // Small center dot
        val dotRadius = 2.0f * resources.displayMetrics.density
        canvas.drawCircle(cx, cy, dotRadius, crosshairPaint)
    }
}
