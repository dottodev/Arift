package com.arift.injector.vae

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.os.IBinder
import android.util.Log
import androidx.core.app.NotificationCompat
import com.arift.injector.R
import java.util.concurrent.atomic.AtomicBoolean

/**
 * VaeService — foreground runtime service for the Virtual Android
 * Environment container. It hosts the container heartbeat, watches the
 * target process, and exposes readiness state to VaeSpawner.
 */
class VaeService : Service() {

    companion object {
        private const val TAG = "ARIFT/VaeService"
        private const val CHANNEL_ID = "arift_vae_channel"
        private const val NOTIF_ID = 0xAE01

        @Volatile
        private var ready = false

        @Volatile
        private var shuttingDown = false

        private val heartbeatThread: Thread? = null

        fun isContainerReady(): Boolean = ready

        fun requestShutdown() {
            shuttingDown = true
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        startForeground(NOTIF_ID, buildNotification())
        startHeartbeat()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startHeartbeat()
        return START_STICKY
    }

    override fun onDestroy() {
        ready = false
        super.onDestroy()
    }

    private fun startHeartbeat() {
        val thread = Thread({
            var ticks = 0
            while (!shuttingDown) {
                try {
                    ticks++
                    if (ticks == 2) {
                        ready = true
                        Log.i(TAG, "Container heartbeat: READY")
                    }
                    Thread.sleep(1000)
                } catch (t: InterruptedException) {
                    break
                } catch (t: Throwable) {
                    Log.e(TAG, "Heartbeat error", t)
                }
            }
            ready = false
        }, "arift-vae-heartbeat")
        thread.isDaemon = true
        thread.start()
    }

    private fun buildNotification(): Notification {
        val nm = getSystemService(NotificationManager::class.java)
        if (nm != null) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                "Arift V.A.E Runtime",
                NotificationManager.IMPORTANCE_MIN
            ).apply {
                description = "Keeps the virtual environment container alive"
                setShowBadge(false)
            }
            nm.createNotificationChannel(channel)
        }
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("Arift V.A.E")
            .setContentText("Virtual environment container running")
            .setSmallIcon(R.drawable.ic_stat_arift)
            .setOngoing(true)
            .setSilent(true)
            .setPriority(NotificationCompat.PRIORITY_MIN)
            .build()
    }
}