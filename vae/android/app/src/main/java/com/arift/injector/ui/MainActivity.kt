package com.arift.injector.ui

import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.arift.injector.databinding.ActivityMainBinding
import com.arift.injector.AriftApplication
import kotlinx.coroutines.launch

/**
 * MainActivity — minimal launcher. Requests overlay permission, then
 * hands off to CheatOverlayService which draws the ARIFT MENU.
 */
class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private val injectionManager get() = AriftApplication.instance.injectionManager

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        binding.btnLaunchOverlay.setOnClickListener {
            if (canDrawOverlays()) {
                startOverlayService()
            } else {
                requestOverlayPermission()
            }
        }

        binding.btnLaunchInjector.setOnClickListener {
            if (!canDrawOverlays()) {
                requestOverlayPermission()
                return@setOnClickListener
            }
            lifecycleScope.launch {
                injectionManager.launch { result ->
                    runOnUiThread {
                        result.fold(
                            onSuccess = {
                                Toast.makeText(this@MainActivity, "Injected (pid=${injectionManager.targetPid})", Toast.LENGTH_SHORT).show()
                            },
                            onFailure = {
                                Toast.makeText(this@MainActivity, "Inject failed: ${it.message}", Toast.LENGTH_LONG).show()
                            }
                        )
                    }
                }
            }
        }

        binding.btnStop.setOnClickListener {
            injectionManager.shutdown()
            stopService(Intent(this, CheatOverlayService::class.java))
            Toast.makeText(this, "Stopped", Toast.LENGTH_SHORT).show()
        }

        binding.btnStatus.setOnClickListener {
            val diag = injectionManager.diagSnapshot()
            binding.txtStatus.text = diag
        }
    }

    override fun onResume() {
        super.onResume()
        binding.txtStatus.text = "Core: ${if (injectionManager.isFeatureEnabled(9)) "fortified" else "loaded"} | State: ${injectionManager.state}"
    }

    private fun canDrawOverlays(): Boolean {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            Settings.canDrawOverlays(this)
        } else true
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
        startForegroundService(Intent(this, CheatOverlayService::class.java))
        Toast.makeText(this, "ARIFT MENU deployed", Toast.LENGTH_SHORT).show()
    }
}