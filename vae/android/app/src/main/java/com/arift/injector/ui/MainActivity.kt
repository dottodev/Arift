package com.arift.injector.ui

import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.util.Log
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.arift.injector.databinding.ActivityMainBinding
import com.arift.injector.AriftApplication
import com.arift.injector.core.InjectionManager
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/**
 * MainActivity — control center. Runs a background scanner that watches
 * the virtual space for MLBB and attaches the core the moment the game
 * appears. "INJECT CORE" launches the game inside the V.A.E (if needed),
 * injects, and opens the ARIFT MENU overlay with its floating chip.
 */
class MainActivity : AppCompatActivity() {

    private companion object {
        private const val TAG = "ARIFT/Main"
    }

    private lateinit var binding: ActivityMainBinding
    private val injectionManager get() = AriftApplication.instance.injectionManager
    private var scanJob: Job? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        binding.btnInject.setOnClickListener { onInjectClicked() }
        binding.btnStop.setOnClickListener { onStopClicked() }

        // One-time overlay permission request up front so INJECT CORE
        // never gets blocked mid-flow.
        requestOverlayPermissionIfNeeded()
        startScanner()
    }

    private fun startScanner() {
        scanJob?.cancel()
        scanJob = lifecycleScope.launch {
            while (isActive) {
                updateStatus()
                val state = injectionManager.state
                if (state == InjectionManager.State.READY ||
                    state == InjectionManager.State.IDLE
                ) {
                    injectionManager.autoAttachIfPresent()
                }
                // Self-heal: attached but the menu overlay is not up?
                // Bring it up again (virtual spaces can eat the first
                // foreground-service start).
                if (injectionManager.isAttached() && !CheatOverlayService.visible) {
                    startOverlayService()
                }
                delay(2500)
            }
        }
    }

    private fun onInjectClicked() {
        if (!canDrawOverlays()) {
            requestOverlayPermission()
            return
        }
        if (injectionManager.isAttached()) {
            startOverlayService()
            Toast.makeText(this, "Already attached — menu opened", Toast.LENGTH_SHORT).show()
            return
        }
        lifecycleScope.launch {
            injectionManager.launch { result ->
                runOnUiThread {
                    result.fold(
                        onSuccess = {
                            startOverlayService()
                            Toast.makeText(
                                this@MainActivity,
                                "Injected (pid=${injectionManager.targetPid})",
                                Toast.LENGTH_SHORT
                            ).show()
                        },
                        onFailure = {
                            Toast.makeText(
                                this@MainActivity,
                                "Inject failed: ${it.message}",
                                Toast.LENGTH_LONG
                            ).show()
                        }
                    )
                }
            }
        }
    }

    private fun onStopClicked() {
        injectionManager.shutdown()
        stopService(Intent(this, CheatOverlayService::class.java))
        Toast.makeText(this, "Stopped", Toast.LENGTH_SHORT).show()
    }

    private fun updateStatus() {
        val im = injectionManager
        val core = when (im.state) {
            InjectionManager.State.IDLE, InjectionManager.State.PREPARING -> "LOADING\u2026"
            InjectionManager.State.READY -> "READY"
            InjectionManager.State.LAUNCHING -> "LAUNCHING\u2026"
            InjectionManager.State.ATTACHED, InjectionManager.State.ACTIVE -> "ATTACHED"
            InjectionManager.State.SHUTTING_DOWN -> "STOPPING\u2026"
        }
        val target = if (im.targetPid > 0) "MLBB pid ${im.targetPid}" else "MLBB not found"
        binding.txtStatus.text = "CORE   $core\nTARGET $target\nSTATE  ${im.state}"
    }

    private fun canDrawOverlays(): Boolean {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            Settings.canDrawOverlays(this)
        } else true
    }

    private fun requestOverlayPermissionIfNeeded() {
        if (!canDrawOverlays()) requestOverlayPermission()
    }

    private fun requestOverlayPermission() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            startActivity(
                Intent(
                    Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                    Uri.parse("package:$packageName")
                )
            )
        }
    }

    private fun startOverlayService() {
        val intent = Intent(this, CheatOverlayService::class.java)
        try {
            startForegroundService(intent)
        } catch (t: Throwable) {
            // Virtual spaces often block FGS starts — fall back to a
            // plain service start so the overlay still comes up.
            Log.w(TAG, "FGS start blocked (${t.message}), falling back to startService")
            try {
                startService(intent)
            } catch (t2: Throwable) {
                Log.e(TAG, "Overlay service start failed", t2)
            }
        }
    }
}