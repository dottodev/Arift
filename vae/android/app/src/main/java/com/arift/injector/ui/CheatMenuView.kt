package com.arift.injector.ui

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Shader
import android.graphics.Typeface
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.MotionEvent
import android.view.View
import com.arift.injector.AriftApplication
import com.arift.injector.core.InjectionManager
import kotlin.math.abs

/**
 * CheatMenuView — the full "ARIFT MENU" overlay, drawn 100% with Canvas.
 *
 *  - Dark mod-menu panel (340dp) with neon-yellow accents
 *  - Tabs: LOBBY / INGAME / MUSIC / EXIT
 *  - Pill toggles per feature
 *  - Drag anywhere on the panel (smooth incremental drag, clamped)
 *  - Minimize (—) button + title tap collapse to the floating chip
 *
 * Lives as a CHILD of the overlay's single window (see CheatOverlayService);
 * it never touches the WindowManager itself — dragging delegates to the
 * service, which moves the shared window.
 */
@SuppressLint("ViewConstructor")
class CheatMenuView(context: Context) : View(context) {

    companion object {
        private const val TAG = "ARIFT/Menu"

        private const val TAB_LOBBY = 0
        private const val TAB_INGAME = 1
        private const val TAB_MUSIC = 2
        private const val TAB_EXIT = 3

        private val LOBBY_FEATURES = listOf(
            CheatRow("RANK BOOSTER", InjectionManager.FEATURE_RANK_BOOSTER),
            CheatRow("VOID BAN", InjectionManager.FEATURE_VOID_BAN)
        )

        private val INGAME_FEATURES = listOf(
            CheatRow("ESP", InjectionManager.FEATURE_ESP),
            CheatRow("MAP HACK", InjectionManager.FEATURE_MAP_HACK),
            CheatRow("ENEMY LAG", InjectionManager.FEATURE_ENEMY_LAG),
            CheatRow("AUTO AIM", InjectionManager.FEATURE_AUTO_AIM),
            CheatRow("AUTO RETRI", InjectionManager.FEATURE_AUTO_RETRI),
            CheatRow("TANK DEFENSE", InjectionManager.FEATURE_TANK_DEFENSE),
            CheatRow("PHYSICAL DAMAGE", InjectionManager.FEATURE_PHYSICAL_DAMAGE)
        )

        private val MUSIC_FEATURES = listOf(
            CheatRow("MUSIC PLAYER", InjectionManager.FEATURE_MUSIC)
        )

        private data class CheatRow(val label: String, val feature: Int)
    }

    /** Invoked when the user minimizes the menu to the floating chip. */
    var onMinimize: (() -> Unit)? = null

    /** Invoked while the user drags the panel (delegates window movement). */
    var onDrag: ((dx: Float, dy: Float) -> Unit)? = null

    /** Invoked when the user hits EXIT (menu closes the overlay). */
    var onExit: (() -> Unit)? = null

    // ------------------------------------------------------------------
    // Palette
    // ------------------------------------------------------------------

    private val bgColor = Color.rgb(11, 12, 16)
    private val panelColor = Color.rgb(20, 22, 28)
    private val borderColor = Color.rgb(255, 210, 0)
    private val accentColor = Color.rgb(57, 255, 20)
    private val textColor = Color.rgb(242, 243, 247)
    private val dimTextColor = Color.rgb(138, 144, 160)
    private val rowDividerColor = Color.rgb(38, 43, 54)
    private val tabInactiveColor = Color.rgb(90, 90, 100)

    // ------------------------------------------------------------------
    // Metrics
    // ------------------------------------------------------------------

    private val density = resources.displayMetrics.density
    private val dp = { v: Float -> v * density }

    private val panelWidth = dp(340f)
    private val titleHeight = dp(50f)
    private val tabHeight = dp(42f)
    private val rowHeight = dp(50f)
    private val padding = dp(14f)

    private var activeTab = TAB_INGAME

    // ------------------------------------------------------------------
    // Drag state (incremental — no cumulative drift)
    // ------------------------------------------------------------------

    private var lastTouchX = 0f
    private var lastTouchY = 0f
    private var isDragging = false

    private val handler = Handler(Looper.getMainLooper())
    private val refreshRunnable = object : Runnable {
        override fun run() {
            if (isAttachedToWindow) {
                invalidate()
                handler.postDelayed(this, 100)
            }
        }
    }

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val panelRect = RectF()
    private val roundRect = RectF()

    init {
        setBackgroundColor(Color.TRANSPARENT)
        isClickable = true
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

    // ------------------------------------------------------------------
    // Sizing
    // ------------------------------------------------------------------

    private fun rowsForTab(tab: Int): List<CheatRow> = when (tab) {
        TAB_LOBBY -> LOBBY_FEATURES
        TAB_MUSIC -> MUSIC_FEATURES
        else -> INGAME_FEATURES
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val rows = rowsForTab(activeTab).size
        val total = (titleHeight + tabHeight + rows * rowHeight + padding * 2).toInt()
        setMeasuredDimension(panelWidth.toInt(), total)
    }

    // ------------------------------------------------------------------
    // Drawing
    // ------------------------------------------------------------------

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)

        val rows = rowsForTab(activeTab)
        val panelHeight = titleHeight + tabHeight + rows.size * rowHeight + padding * 2

        panelRect.set(0f, 0f, panelWidth, panelHeight)

        // Panel body
        paint.style = Paint.Style.FILL
        paint.shader = null
        paint.color = panelColor
        roundRect.set(0f, 0f, panelWidth, panelHeight)
        canvas.drawRoundRect(roundRect, dp(16f), dp(16f), paint)

        // Neon border
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = dp(2f)
        paint.color = borderColor
        canvas.drawRoundRect(roundRect, dp(16f), dp(16f), paint)

        // Title strip
        paint.style = Paint.Style.FILL
        paint.shader = LinearGradient(
            0f, 0f, panelWidth, 0f,
            intArrayOf(Color.rgb(30, 33, 42), Color.rgb(16, 18, 24)),
            null, Shader.TileMode.CLAMP
        )
        roundRect.set(0f, 0f, panelWidth, titleHeight)
        canvas.drawRoundRect(roundRect, dp(16f), dp(16f), paint)
        paint.shader = null

        // Title text
        paint.color = textColor
        paint.typeface = Typeface.create(Typeface.DEFAULT, Typeface.BOLD)
        paint.textSize = dp(18f)
        canvas.drawText("ARIFT", padding, dp(21f), paint)

        paint.typeface = Typeface.create(Typeface.DEFAULT, Typeface.NORMAL)
        paint.textSize = dp(9f)
        paint.color = dimTextColor
        canvas.drawText("V.A.E OVERLAY", padding, dp(37f), paint)

        // Accent underline
        paint.style = Paint.Style.FILL
        paint.color = borderColor
        roundRect.set(padding, titleHeight - dp(3f), padding + dp(30f), titleHeight - dp(1f))
        canvas.drawRoundRect(roundRect, dp(1f), dp(1f), paint)

        // Minimize button
        val minCx = panelWidth - padding - dp(18f)
        val minCy = titleHeight / 2f
        val minR = dp(12f)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = dp(2f)
        paint.color = borderColor
        canvas.drawCircle(minCx, minCy, minR, paint)
        paint.style = Paint.Style.FILL
        paint.strokeWidth = dp(2f)
        canvas.drawLine(minCx - dp(6f), minCy, minCx + dp(6f), minCy, paint)

        // Tabs
        var y = titleHeight
        val tabWidth = (panelWidth - padding * 2) / 4f
        for (i in 0 until 4) {
            val left = padding + i * tabWidth + dp(3f)
            val right = left + tabWidth - dp(6f)
            val selected = i == activeTab
            paint.style = Paint.Style.FILL
            paint.color = if (selected) borderColor else Color.rgb(26, 29, 38)
            roundRect.set(left, y, right, y + tabHeight - dp(6f))
            canvas.drawRoundRect(roundRect, dp(9f), dp(9f), paint)
            paint.color = if (selected) Color.rgb(11, 12, 16) else tabInactiveColor
            paint.textSize = dp(12f)
            paint.typeface = Typeface.create(Typeface.DEFAULT, Typeface.BOLD)
            val label = when (i) {
                TAB_LOBBY -> "LOBBY"
                TAB_INGAME -> "INGAME"
                TAB_MUSIC -> "MUSIC"
                else -> "EXIT"
            }
            val tw = paint.measureText(label)
            canvas.drawText(label, left + (tabWidth - tw) / 2f, y + dp(25f), paint)
        }
        y += tabHeight

        // Feature rows
        for (i in rows.indices) {
            val row = rows[i]
            val rowTop = y + i * rowHeight

            // Row divider
            paint.style = Paint.Style.STROKE
            paint.strokeWidth = dp(1f)
            paint.color = rowDividerColor
            canvas.drawLine(padding, rowTop, panelWidth - padding, rowTop, paint)

            // Label
            paint.style = Paint.Style.FILL
            paint.color = textColor
            paint.textSize = dp(14f)
            paint.typeface = Typeface.create(Typeface.DEFAULT, Typeface.BOLD)
            canvas.drawText(row.label, padding, rowTop + dp(31f), paint)

            // Pill toggle
            val enabled = AriftApplication.instance.injectionManager.isFeatureEnabled(row.feature)
            val pillLeft = panelWidth - padding - dp(52f)
            val pillTop = rowTop + (rowHeight - dp(24f)) / 2f
            val pillW = dp(44f)
            val pillH = dp(24f)

            paint.style = Paint.Style.FILL
            paint.color = if (enabled) accentColor else Color.rgb(38, 43, 54)
            roundRect.set(pillLeft, pillTop, pillLeft + pillW, pillTop + pillH)
            canvas.drawRoundRect(roundRect, dp(12f), dp(12f), paint)

            val knobR = dp(9f)
            val knobCx = if (enabled) pillLeft + pillW - dp(12f) else pillLeft + dp(12f)
            paint.color = Color.WHITE
            canvas.drawCircle(knobCx, pillTop + pillH / 2f, knobR, paint)
        }
    }

    // ------------------------------------------------------------------
    // Interaction
    // ------------------------------------------------------------------

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
                    handleTap(event.x, event.y)
                }
                isDragging = false
                return true
            }
        }
        return super.onTouchEvent(event)
    }

    private fun handleTap(x: Float, y: Float) {
        // Minimize button
        val minCx = panelWidth - padding - dp(18f)
        val minCy = titleHeight / 2f
        if (x in (minCx - dp(20f))..(minCx + dp(20f)) &&
            y in (minCy - dp(20f))..(minCy + dp(20f))
        ) {
            onMinimize?.invoke()
            return
        }

        // Title area tap also minimizes
        if (y in 0f..titleHeight) {
            onMinimize?.invoke()
            return
        }

        // Tabs
        val tabWidth = (panelWidth - padding * 2) / 4f
        if (y in titleHeight..(titleHeight + tabHeight)) {
            val tab = ((x - padding) / tabWidth).toInt().coerceIn(0, 3)
            if (tab == TAB_EXIT) {
                AriftApplication.instance.injectionManager.shutdown()
                onExit?.invoke()
                return
            }
            activeTab = tab
            requestLayout()
            invalidate()
            return
        }

        // Feature rows
        val rows = rowsForTab(activeTab)
        val rowsTop = titleHeight + tabHeight
        val rowIndex = ((y - rowsTop) / rowHeight).toInt()
        if (rowIndex in rows.indices) {
            val row = rows[rowIndex]
            val manager = AriftApplication.instance.injectionManager
            val newState = !manager.isFeatureEnabled(row.feature)
            manager.setFeature(row.feature, newState)
            manager.persistFeature(row.feature, newState)
            Log.i(TAG, "${row.label} -> $newState")
            invalidate()
        }
    }
}