package com.arift.injector.ui

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.view.View
import android.view.WindowManager
import com.arift.injector.R

/**
 * FloatingChipView — the discreet floating icon used when the ARIFT
 * MENU is minimized. Tap it to bring the full menu back.
 */
class FloatingChipView(context: Context) : View(context) {

    private val density = resources.displayMetrics.density
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val iconBitmap: Bitmap = BitmapFactory.decodeResource(resources, R.drawable.ic_overlay_button)

    private var windowManager: WindowManager? = null
    private var windowParams: WindowManager.LayoutParams? = null

    init {
        setBackgroundColor(Color.TRANSPARENT)
        isClickable = true
    }

    fun attach(params: WindowManager.LayoutParams, wm: WindowManager) {
        windowParams = params
        windowManager = wm
    }

    fun detach() {
        windowManager?.removeView(this)
        windowManager = null
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val size = (52f * density).toInt()
        setMeasuredDimension(size, size)
    }

    override fun onDraw(canvas: Canvas) {
        val size = (52f * density)
        val iconSize = (40f * density)
        val offset = (size - iconSize) / 2f

        canvas.drawBitmap(iconBitmap, null, RectF(offset, offset, offset + iconSize, offset + iconSize), paint)
    }
}
