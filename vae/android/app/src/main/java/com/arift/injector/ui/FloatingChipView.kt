package com.arift.injector.ui

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.util.Log
import android.view.MotionEvent
import android.view.View
import android.view.WindowManager
import com.arift.injector.R
import kotlin.math.abs

/**
 * FloatingChipView — the discreet floating circle used when the ARIFT
 * MENU is minimized. Drag it anywhere, tap it to restore the full menu.
 */
class FloatingChipView(context: Context) : View(context) {

    companion object {
        private const val TAG = "ARIFT/Chip"
    }

    /** Invoked when the user taps the chip to restore the menu. */
    var onTap: (() -> Unit)? = null

    private val density = resources.displayMetrics.density
    private val dp = { v: Float -> v * density }
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val iconBitmap: Bitmap = BitmapFactory.decodeResource(resources, R.mipmap.ic_launcher)

    private var windowManager: WindowManager? = null
    private var windowParams: WindowManager.LayoutParams? = null

    private var lastTouchX = 0f
    private var lastTouchY = 0f
    private var isDragging = false

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
        val size = (56f * density).toInt()
        setMeasuredDimension(size, size)
    }

    override fun onDraw(canvas: Canvas) {
        val size = 56f * density
        val center = size / 2f

        // Body
        paint.style = Paint.Style.FILL
        paint.color = Color.rgb(20, 22, 28)
        canvas.drawCircle(center, center, size / 2f, paint)

        // Neon ring
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = dp(2f)
        paint.color = Color.rgb(255, 210, 0)
        canvas.drawCircle(center, center, size / 2f - dp(1f), paint)

        // Icon
        val iconSize = 38f * density
        val offset = (size - iconSize) / 2f
        canvas.drawBitmap(
            iconBitmap,
            null,
            RectF(offset, offset, offset + iconSize, offset + iconSize),
            paint
        )
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                lastTouchX = event.x
                lastTouchY = event.y
                isDragging = false
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                val dx = event.x - lastTouchX
                val dy = event.y - lastTouchY
                if (!isDragging && (abs(dx) > dp(4f) || abs(dy) > dp(4f))) {
                    isDragging = true
                }
                if (isDragging) {
                    moveWindow(dx, dy)
                    lastTouchX = event.x
                    lastTouchY = event.y
                }
                return true
            }
            MotionEvent.ACTION_UP -> {
                if (!isDragging) {
                    onTap?.invoke()
                }
                isDragging = false
                return true
            }
        }
        return super.onTouchEvent(event)
    }

    private fun moveWindow(dx: Float, dy: Float) {
        val p = windowParams ?: return
        val wm = windowManager ?: return
        val sw = resources.displayMetrics.widthPixels
        val sh = resources.displayMetrics.heightPixels
        val size = (56f * density).toInt()
        p.x = (p.x + dx).toInt().coerceIn(0, sw - size)
        p.y = (p.y + dy).toInt().coerceIn(0, sh - size)
        try {
            wm.updateViewLayout(this, p)
        } catch (t: Throwable) {
            Log.e(TAG, "Drag update failed", t)
        }
    }
}