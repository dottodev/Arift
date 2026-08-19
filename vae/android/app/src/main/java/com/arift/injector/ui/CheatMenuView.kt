package com.arift.injector.ui

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Typeface
import android.os.Handler
import android.os.Looper
import android.util.AttributeSet
import android.util.Log
import android.view.GestureDetector
import android.view.MotionEvent
import android.view.View
import android.view.WindowManager
import com.arift.injector.AriftApplication
import com.arift.injector.core.InjectionManager
import kotlin.math.abs

/**
 * CheatMenuView — the full "ARIFT MENU" overlay, drawn 100% with Canvas
 * for a razor-sharp, self-contained rendering pipeline.
 *
 *  - Dark panel with neon-yellow rounded border
 *  - Title "ARIFT MENU" + "ARIFT MENU — V.A.E OVERLAY"
 *  - Tabs: LOBBY / INGAME / MUSIC / EXIT
 *  - White circular toggles per feature
 */
@SuppressLint("ViewConstructor")
class CheatMenuView(context: Context) : View(context) {

    companion object {
        private const val TAG = "ARIFT/Menu"

        private val TAB_LOBBY = 0
        private val TAB_INGAME = 1
        private val TAB_MUSIC = 2
        private val TAB_EXIT = 3

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

    // ------------------------------------------------------------------
    // Palette
    // ------------------------------------------------------------------

    private val bgColor = Color.rgb(12, 12, 14)
    private val panelColor = Color.rgb(22, 22, 26)
    private val borderColor = Color.rgb(255, 210, 0)
    private val accentColor = Color.rgb(57, 255, 20)
    private val textColor = Color.WHITE
    private val dimTextColor = Color.rgb(160, 160, 170)
    private val toggleOffColor = Color.WHITE
    private val toggleOnColor = accentColor
    private val tabInactiveColor = Color.rgb(90, 90, 100)

    // ------------------------------------------------------------------
    // Metrics
    // ------------------------------------------------------------------

    private val density = resources.displayMetrics.density
    private val dp = { v: Float -> v * density }

    private val panelWidth = dp(300f)
    private val titleHeight = dp(42f)
    private val brandHeight = dp(22f)
    private val tabHeight = dp(44f)
    private val rowHeight = dp(52f)
    private val padding = dp(14f)

    private var activeTab = TAB_INGAME
    private var collapseOffset = 0f

    // ------------------------------------------------------------------
    // Drag state
    // ------------------------------------------------------------------

    private var dragStartX = 0f
    private var dragStartY = 0f
    private var windowParams: WindowManager.LayoutParams? = null
    private var windowManager: WindowManager? = null
    private var isDragging = false

    private val gestureDetector = GestureDetector(
        context,
        object : GestureDetector.SimpleOnGestureListener() {
            override fun onSingleTapConfirmed(e: MotionEvent): Boolean {
                handleTap(e.x, e.y)
                return true
            }

            override fun onDown(e: MotionEvent): Boolean = true
        }
    )

    private val handler = Handler(Looper.getMainLooper())
    private val refreshRunnable = object : Runnable {
        override fun run() {
            if (attached) {
                invalidate()
                handler.postDelayed(this, 100)
            }
        }
    }

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val panelRect = RectF()
    private val roundRect = RectF()

    private var attached = false

    init {
        setBackgroundColor(Color.TRANSPARENT)
        isClickable = true
    }

    // ------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------

    fun attach(params: WindowManager.LayoutParams, wm: WindowManager) {
        windowParams = params
        windowManager = wm
        attached = true
        handler.post(refreshRunnable)
    }

    fun detach() {
        attached = false
        handler.removeCallbacks(refreshRunnable)
        windowManager?.removeView(this)
        windowManager = null
    }

    fun makeVisible() {
        attached = true
        handler.post(refreshRunnable)
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
        val contentHeight = titleHeight + brandHeight + tabHeight + rows * rowHeight + padding * 2
        val collapsedHeight = titleHeight + brandHeight + tabHeight + padding * 2
        val total = (contentHeight * (1f - collapseOffset) + collapsedHeight * collapseOffset).toInt()
        setMeasuredDimension(panelWidth.toInt(), total)
    }

    // ------------------------------------------------------------------
    // Drawing
    // ------------------------------------------------------------------

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)

        val rows = rowsForTab(activeTab)
        val contentHeight = titleHeight + brandHeight + tabHeight + rows.size * rowHeight + padding * 2
        val collapsedHeight = titleHeight + brandHeight + tabHeight + padding * 2
        val panelHeight = contentHeight * (1f - collapseOffset) + collapsedHeight * collapseOffset
        val visibleRows = rows.size * (1f - collapseOffset)

        panelRect.set(0f, 0f, panelWidth, panelHeight)

        // Panel body
        paint.style = Paint.Style.FILL
        paint.color = bgColor
        roundRect.set(0f, 0f, panelWidth, panelHeight)
        canvas.drawRoundRect(roundRect, dp(14f), dp(14f), paint)

        // Neon border
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = dp(2.5f)
        paint.color = borderColor
        canvas.drawRoundRect(roundRect, dp(14f), dp(14f), paint)

        var y = padding

        // Title
        paint.style = Paint.Style.FILL
        paint.color = textColor
        paint.typeface = Typeface.create(Typeface.DEFAULT, Typeface.BOLD)
        paint.textSize = dp(20f)
        val titleText = "ARIFT MENU"
        canvas.drawText(titleText, padding, y + dp(20f), paint)
        y += titleHeight

        // Branding
        paint.typeface = Typeface.create(Typeface.DEFAULT, Typeface.NORMAL)
        paint.textSize = dp(11f)
        paint.color = dimTextColor
        canvas.drawText("ARIFT MENU — V.A.E OVERLAY", padding, y + dp(14f), paint)
        y += brandHeight

        // Tabs
        val tabWidth = (panelWidth - padding * 2) / 4f
        for (i in 0 until 4) {
            val left = padding + i * tabWidth
            val right = left + tabWidth
            val selected = i == activeTab
            paint.style = Paint.Style.FILL
            paint.color = if (selected) Color.rgb(35, 35, 42) else Color.TRANSPARENT
            roundRect.set(left, y, right, y + tabHeight - dp(4f))
            canvas.drawRoundRect(roundRect, dp(8f), dp(8f), paint)
            paint.color = if (selected) borderColor else tabInactiveColor
            paint.textSize = dp(13f)
            paint.typeface = Typeface.create(Typeface.DEFAULT, if (selected) Typeface.BOLD else Typeface.NORMAL)
            val label = when (i) {
                TAB_LOBBY -> "LOBBY"
                TAB_INGAME -> "INGAME"
                TAB_MUSIC -> "MUSIC"
                else -> "EXIT"
            }
            val tw = paint.measureText(label)
            canvas.drawText(label, left + (tabWidth - tw) / 2f, y + dp(24f), paint)
        }
        y += tabHeight

        // Feature rows
        for (i in 0 until visibleRows.toInt()) {
            val row = rows[i]
            val rowTop = y + i * rowHeight
            val rowBottom = rowTop + rowHeight

            // Row divider
            paint.style = Paint.Style.STROKE
            paint.strokeWidth = dp(1f)
            paint.color = Color.rgb(40, 40, 46)
            canvas.drawLine(padding, rowTop, panelWidth - padding, rowTop, paint)

            // Label
            paint.style = Paint.Style.FILL
            paint.color = textColor
            paint.textSize = dp(15f)
            paint.typeface = Typeface.create(Typeface.DEFAULT, Typeface.BOLD)
            canvas.drawText(row.label, padding, rowTop + dp(28f), paint)

            // White circular toggle
            val enabled = AriftApplication.instance.injectionManager.isFeatureEnabled(row.feature)
            val cx = panelWidth - padding - dp(18f)
            val cy = rowTop + dp(26f)
            val r = dp(14f)

            paint.style = Paint.Style.STROKE
            paint.strokeWidth = dp(2f)
            paint.color = if (enabled) toggleOnColor else Color.rgb(120, 120, 130)
            canvas.drawCircle(cx, cy, r, paint)

            paint.style = Paint.Style.FILL
            paint.color = if (enabled) toggleOnColor else toggleOffColor
            canvas.drawCircle(cx, cy, r - dp(5f), paint)
        }
    }

    // ------------------------------------------------------------------
    // Interaction
    // ------------------------------------------------------------------

    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                dragStartX = event.x
                dragStartY = event.y
                isDragging = false
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                val dx = event.x - dragStartX
                val dy = event.y - dragStartY
                if (abs(dx) > dp(8f) || abs(dy) > dp(8f)) {
                    isDragging = true
                    windowParams?.let { p ->
                        p.x += dx.toInt()
                        p.y += dy.toInt()
                        windowManager?.updateViewLayout(this, p)
                    }
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
        return gestureDetector.onTouchEvent(event)
    }

    private fun handleTap(x: Float, y: Float) {
        val yTop = padding + titleHeight + brandHeight
        val tabWidth = (panelWidth - padding * 2) / 4f
        if (y in yTop..(yTop + tabHeight)) {
            val tab = ((x - padding) / tabWidth).toInt().coerceIn(0, 3)
            if (tab == TAB_EXIT) {
                AriftApplication.instance.injectionManager.shutdown()
                detach()
                return
            }
            activeTab = tab
            requestLayout()
            invalidate()
            return
        }

        val rows = rowsForTab(activeTab)
        val rowsTop = yTop + tabHeight
        if (collapseOffset < 1f) {
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

        // Title area tap toggles collapse
        if (y in padding..(padding + titleHeight)) {
            collapseOffset = if (collapseOffset < 1f) 1f else 0f
            requestLayout()
            invalidate()
        }
    }
}