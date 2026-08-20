package com.arift.injector.ui

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.graphics.Color
import android.graphics.PixelFormat
import android.os.Build
import android.os.IBinder
import android.provider.Settings
import android.util.Log
import android.view.Gravity
import android.view.WindowManager
import android.widget.FrameLayout
import androidx.core.app.NotificationCompat
import com.arift.injector.AriftApplication
import com.arift.injector.R

/**
 * CheatOverlayService — draws the ARIFT overlay as ONE stable window.
 *
 * The window is added once and never removed until shutdown; the menu and
 * the floating chip are swapped as CHILDREN inside it. Swapping children
 * (unlike swapping windows) is safe while a touch is in progress, so
 * minimizing no longer crashes and dragging no longer glitches.
 *
 * The overlay starts in chip form (the discreet floating icon); tapping
 * the chip opens the full menu, minimizing the menu returns to the chip.
 */
class CheatOverlayService : Service() {

    companion object {
        private const val TAG = "ARIFT/Overlay"
        private const val CHANNEL_ID = "arift_overlay_channel"
        private const val NOTIF_ID = 0xAE02

        @Volatile
        var visible: Boolean = false
            private set
    }

    private lateinit var windowManager: WindowManager
    private var overlayRoot: FrameLayout? = null
    private var menuView: CheatMenuView? = null
    private var chipView: FloatingChipView? = null

    private var lastMenuX = 24
    private var lastMenuY = 160

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        windowManager = getSystemService(Context.WINDOW_SERVICE) as WindowManager
        try {
            startForeground(NOTIF_ID, buildNotification())
        } catch (t: Throwable) {
            // Some virtual spaces reject foreground notifications; the
            // overlay window itself still works without one.
            Log.w(TAG, "startForeground blocked (${t.message}) — continuing")
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M && !Settings.canDrawOverlays(this)) {
            Log.w(TAG, "Overlay permission missing")
            stopSelf()
            return START_NOT_STICKY
        }
        if (overlayRoot == null) {
            // Floating icon FIRST — the menu only opens when tapped.
            showChip()
        }
        visible = overlayRoot != null
        return START_STICKY
    }

    private fun baseParams(): WindowManager.LayoutParams =
        WindowManager.LayoutParams(
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
            PixelFormat.TRANSLUCENT
        ).apply {
            gravity = Gravity.TOP or Gravity.START
        }

    /** Creates the single overlay window on first use; returns null on failure. */
    private fun ensureRoot(): FrameLayout? {
        overlayRoot?.let { return it }
        val root = FrameLayout(this)
        root.setBackgroundColor(Color.TRANSPARENT)
        val params = baseParams().apply {
            x = lastMenuX
            y = lastMenuY
        }
        try {
            windowManager.addView(root, params)
            overlayRoot = root
            report("window up at $lastMenuX,$lastMenuY")
            Log.i(TAG, "Overlay window created at $lastMenuX,$lastMenuY")
        } catch (t: Throwable) {
            report("window add failed: ${t.message}")
            Log.e(TAG, "Failed to create overlay window", t)
            return null
        }
        return root
    }

    /** Shared drag target: children drag the single root window. */
    fun onOverlayDrag(dx: Float, dy: Float) {
        val root = overlayRoot ?: return
        val p = root.layoutParams as? WindowManager.LayoutParams ?: return
        val sw = resources.displayMetrics.widthPixels
        val sh = resources.displayMetrics.heightPixels
        val w = root.width.coerceAtLeast(1)
        val h = root.height.coerceAtLeast(1)
        // Menu may hide behind the screen edge; the chip stays fully visible.
        val small = w < (200f * resources.displayMetrics.density).toInt()
        val minX = if (small) 0 else -(w * 2 / 3)
        val maxX = if (small) (sw - w).coerceAtLeast(0) else sw - w / 3
        p.x = (p.x + dx).toInt().coerceIn(minX, maxX)
        p.y = (p.y + dy).toInt().coerceIn(0, (sh - h).coerceAtLeast(0))
        try {
            windowManager.updateViewLayout(root, p)
        } catch (t: Throwable) {
            Log.e(TAG, "Drag update failed", t)
        }
    }

    private fun showChip() {
        if (chipView != null) return
        val root = ensureRoot() ?: return
        val chip = FloatingChipView(this).apply {
            onTap = { showMenu() }
            onDrag = { dx, dy -> onOverlayDrag(dx, dy) }
        }
        root.removeAllViews()
        root.addView(chip)
        chipView = chip
        menuView = null
        report("chip shown")
        Log.i(TAG, "Chip shown")
    }

    private fun showMenu() {
        if (menuView != null) return
        val root = ensureRoot() ?: return
        val menu = CheatMenuView(this).apply {
            onMinimize = { showChip() }
            onDrag = { dx, dy -> onOverlayDrag(dx, dy) }
            onExit = {
                report("overlay closed")
                removeOverlay()
            }
        }
        root.removeAllViews()
        root.addView(menu)
        menuView = menu
        chipView = null
        report("menu shown")
        Log.i(TAG, "Menu shown")
    }

    private fun removeOverlay() {
        overlayRoot?.let {
            try {
                windowManager.removeView(it)
            } catch (t: Throwable) {
                Log.e(TAG, "Overlay remove error", t)
            }
        }
        overlayRoot = null
        menuView = null
        chipView = null
        visible = false
    }

    override fun onDestroy() {
        removeOverlay()
        super.onDestroy()
    }

    /** Mirrors overlay lifecycle events onto the status card's event log. */
    private fun report(msg: String) {
        AriftApplication.instance.injectionManager.addEvent("Overlay: $msg")
    }

    private fun buildNotification(): Notification {
        val nm = getSystemService(NotificationManager::class.java)
        if (nm != null) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                "Arift Menu",
                NotificationManager.IMPORTANCE_MIN
            ).apply { setShowBadge(false) }
            nm.createNotificationChannel(channel)
        }
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("ARIFT MENU")
            .setContentText("Menu active")
            .setSmallIcon(R.drawable.ic_stat_arift)
            .setOngoing(true)
            .setSilent(true)
            .setPriority(NotificationCompat.PRIORITY_MIN)
            .build()
    }
}