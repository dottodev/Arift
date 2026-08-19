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
 * above every window inside the V.A.E. The menu is movable and
 * collapsible to a small floating chip.
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
        if (menuView == null) {
            showMenu()
        } else {
            menuView?.makeVisible()
        }
        visible = true
        return START_STICKY
    }

    private fun showMenu() {
        val view = CheatMenuView(this)
        val params = WindowManager.LayoutParams(
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.WRAP_CONTENT,
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
            PixelFormat.TRANSLUCENT
        ).apply {
            gravity = Gravity.TOP or Gravity.START
            x = 24
            y = 160
        }
        try {
            windowManager.addView(view, params)
            menuView = view
            menuView?.attach(params, windowManager)
            Log.i(TAG, "ARIFT MENU drawn")
        } catch (t: Throwable) {
            Log.e(TAG, "Failed to draw overlay", t)
        }
    }

    override fun onDestroy() {
        try {
            menuView?.detach()
            chipView?.detach()
        } catch (t: Throwable) {
            Log.e(TAG, "Cleanup error", t)
        }
        menuView = null
        chipView = null
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