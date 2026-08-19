package com.arift.injector.ui

import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.util.Log
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.Lifecycle
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
            var tick = 0
            while (isActive) {
                // Presence scan first so the UI reports "MLBB FOUND" even
                // before the auto-attach kicks in.
                injectionManager.reportTargetPresence()
                val state = injectionManager.state
                if (state == InjectionManager.State.READY ||
                    state == InjectionManager.State.IDLE
                ) {
                    injectionManager.autoAttachIfPresent()
                }
                // On-device injection verification every ~7.5s while
                // attached — proves the core can see the game's memory.
                if (injectionManager.isAttached() && tick % 3 == 0) {
                    injectionManager.verifyInjection()
                }
                tick++
                updateStatus()
                // Self-heal: attached but the menu overlay is not up?
                // Bring it up again. Only attempt while this activity is
                // actually foreground — background starts are blocked by
                // Android and would just burn attempts.
                if (injectionManager.isAttached() && !CheatOverlayService.visible &&
                    lifecycle.currentState.isAtLeast(Lifecycle.State.STARTED)
                ) {
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
        // Start the overlay FIRST — while we are still in the foreground.
        // Once the game launches below us the app goes to the background
        // and Android (targetSdk 34) blocks foreground-service starts,
        // which is why the menu never appeared after injection.
        startOverlayService()
        if (injectionManager.isAttached()) {
            Toast.makeText(this, "Already attached — menu opened", Toast.LENGTH_SHORT).show()
            return
        }
        lifecycleScope.launch {
            injectionManager.launch { result ->
                runOnUiThread {
                    result.fold(
                        onSuccess = {
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
        val target = when {
            im.isAttached() || im.targetPid > 0 -> "MLBB FOUND pid ${im.targetPid}"
            im.lastFoundPid > 0 -> "MLBB FOUND pid ${im.lastFoundPid}"
            else -> "MLBB not found"
        }
        val sb = StringBuilder("CORE   $core\nTARGET $target\nSTATE  ${im.state}")
        im.recentEvents().takeLast(5).forEach { sb.append("\n\u2022 ").append(it) }
        binding.txtStatus.text = sb.toString()
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