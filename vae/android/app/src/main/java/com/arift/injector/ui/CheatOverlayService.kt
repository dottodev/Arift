package com.arift.injector.ui

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.graphics.PixelFormat
import android.os.Build
import android.os.IBinder
import android.provider.Settings
import android.util.Log
import android.view.Gravity
import android.view.WindowManager
import androidx.core.app.NotificationCompat
import com.arift.injector.R

/**
 * CheatOverlayService — draws the "ARIFT MENU" as a floating overlay
 * above every window inside the V.A.E. The menu is movable and can be
 * minimized to a small floating chip; tapping the chip restores it.
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
    private var menuView: CheatMenuView? = null
    private var chipView: FloatingChipView? = null

    private var lastMenuX = 24
    private var lastMenuY = 160

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        windowManager = getSystemService(Context.WINDOW_SERVICE) as WindowManager
        startForeground(NOTIF_ID, buildNotification())
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M && !Settings.canDrawOverlays(this)) {
            Log.w(TAG, "Overlay permission missing")
            stopSelf()
            return START_NOT_STICKY
        }
        if (menuView == null && chipView == null) {
            showMenu()
        } else {
            menuView?.makeVisible()
        }
        visible = true
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

    private fun showMenu() {
        if (menuView != null) return
        removeChip()
        val view = CheatMenuView(this)
        view.onMinimize = { showChip() }
        val params = baseParams().apply {
            x = lastMenuX
            y = lastMenuY
        }
        try {
            windowManager.addView(view, params)
            menuView = view
            menuView?.attach(params, windowManager)
            Log.i(TAG, "ARIFT MENU drawn at $lastMenuX,$lastMenuY")
        } catch (t: Throwable) {
            Log.e(TAG, "Failed to draw overlay", t)
        }
    }

    private fun showChip() {
        if (chipView != null) return
        removeMenu()
        val chip = FloatingChipView(this)
        chip.onTap = { showMenu() }
        val params = baseParams().apply {
            x = lastMenuX
            y = lastMenuY
        }
        try {
            windowManager.addView(chip, params)
            chipView = chip
            chipView?.attach(params, windowManager)
            Log.i(TAG, "ARIFT MENU minimized to chip")
        } catch (t: Throwable) {
            Log.e(TAG, "Failed to draw chip", t)
        }
    }

    private fun removeMenu() {
        menuView?.let {
            try {
                it.detach()
            } catch (t: Throwable) {
                Log.e(TAG, "Menu remove error", t)
            }
        }
        menuView = null
    }

    private fun removeChip() {
        chipView?.let {
            try {
                it.detach()
            } catch (t: Throwable) {
                Log.e(TAG, "Chip remove error", t)
            }
        }
        chipView = null
    }

    override fun onDestroy() {
        try {
            removeMenu()
            removeChip()
        } catch (t: Throwable) {
            Log.e(TAG, "Cleanup error", t)
        }
        visible = false
        super.onDestroy()
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