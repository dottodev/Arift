package com.arift.injector.ui

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.os.Handler
import android.os.Looper
import android.view.MotionEvent
import android.view.View
import androidx.appcompat.content.res.AppCompatResources
import com.arift.injector.R
import kotlin.math.abs

/**
 * FloatingChipView — the discreet floating circle shown when the ARIFT
 * overlay is minimized (and the FIRST thing shown after injecting).
 * Drag it anywhere, tap it to open the full menu.
 *
 * Lives as a CHILD of the overlay's single window (see CheatOverlayService);
 * dragging delegates to the service, which moves the shared window.
 *
 * The app icon is rendered defensively: the launcher icon on API 26+ is an
 * adaptive-icon XML that BitmapFactory cannot decode (returns null). We
 * inflate it as a Drawable instead; if that ever fails we draw a plain
 * glyph so the chip still renders instead of crashing.
 */
class FloatingChipView(context: Context) : View(context) {

    companion object {
        private const val TAG = "ARIFT/Chip"
    }

    /** Invoked when the user taps the chip to open the menu. */
    var onTap: (() -> Unit)? = null

    /** Invoked while the user drags the chip (delegates window movement). */
    var onDrag: ((dx: Float, dy: Float) -> Unit)? = null

    private val density = resources.displayMetrics.density
    private val dp = { v: Float -> v * density }
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private var iconBitmap: Bitmap? = null

    private var lastTouchX = 0f
    private var lastTouchY = 0f
    private var isDragging = false

    // Same 100ms redraw loop as the menu — the VAE virtual display only
    // presents overlay surfaces that keep re-rendering.
    private val handler = Handler(Looper.getMainLooper())
    private val refreshRunnable = object : Runnable {
        override fun run() {
            if (isAttachedToWindow) {
                invalidate()
                handler.postDelayed(this, 100)
            }
        }
    }

    init {
        setBackgroundColor(Color.TRANSPARENT)
        isClickable = true
    }

    /** Lazy, never-throwing icon load. */
    private fun ensureIcon(): Bitmap? {
        iconBitmap?.let { return it }
        val bmp = try {
            val d = AppCompatResources.getDrawable(context, R.mipmap.ic_launcher) ?: return null
            val size = (76f * density).toInt()
            val b = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
            val c = Canvas(b)
            d.setBounds(0, 0, size, size)
            d.draw(c)
            b
        } catch (t: Throwable) {
            null
        }
        iconBitmap = bmp
        return bmp
    }

    // ------------------------------------------------------------------
    // Lifecycle (refresh loop runs only while the view is attached)
    // ------------------------------------------------------------------

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        handler.post(refreshRunnable)
    }

    override fun onDetachedFromWindow() {
        handler.removeCallbacks(refreshRunnable)
        super.onDetachedFromWindow()
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

        // Icon (or glyph fallback if the icon can't be rendered)
        val iconSize = 38f * density
        val offset = (size - iconSize) / 2f
        val icon = ensureIcon()
        if (icon != null) {
            canvas.drawBitmap(
                icon,
                null,
                RectF(offset, offset, offset + iconSize, offset + iconSize),
                paint
            )
        } else {
            paint.style = Paint.Style.FILL
            paint.color = Color.WHITE
            paint.textSize = dp(24f)
            paint.textAlign = Paint.Align.CENTER
            val baseline = center - (paint.descent() + paint.ascent()) / 2f
            canvas.drawText("A", center, baseline, paint)
            paint.textAlign = Paint.Align.LEFT
        }
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
                    onDrag?.invoke(dx, dy)
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
}