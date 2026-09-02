package com.boulecam.ui

import android.content.Context
import android.util.AttributeSet
import android.view.SurfaceView

class AutoFitSurfaceView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyle: Int = 0
) : SurfaceView(context, attrs, defStyle) {

    private var aspectRatioWidth = 0
    private var aspectRatioHeight = 0

    fun setAspectRatio(width: Int, height: Int) {
        if (width < 0 || height < 0) return
        aspectRatioWidth = width
        aspectRatioHeight = height
        requestLayout()
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        super.onMeasure(widthMeasureSpec, heightMeasureSpec)
        val width = MeasureSpec.getSize(widthMeasureSpec)
        val height = MeasureSpec.getSize(heightMeasureSpec)

        if (aspectRatioWidth == 0 || aspectRatioHeight == 0) {
            setMeasuredDimension(width, height)
        } else {
            // Fill available screen while preserving exact aspect ratio
            if (width < height * aspectRatioWidth / aspectRatioHeight) {
                setMeasuredDimension(width, width * aspectRatioHeight / aspectRatioWidth)
            } else {
                setMeasuredDimension(height * aspectRatioWidth / aspectRatioHeight, height)
            }
        }
    }
}
