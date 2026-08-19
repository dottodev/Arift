package com.arift.injector.vae

import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.SystemClock
import android.util.Log
import com.arift.injector.core.ProcessManager

/**
 * VaeSpawner — bootstraps the Virtual Android Environment container,
 * launches the target (MLBB) inside it, and hands the PID + library base
 * to the injection core.
 *
 * In a production V.A.E this class would drive the container runtime
 * (F1 VM / VirtualApp-style host). Here it performs the equivalent
 * container-orchestration flow: verify environment, start the VAE
 * runtime service, wait for the target process, resolve its base.
 */
class VaeSpawner(private val context: Context) {

    companion object {
        private const val TAG = "ARIFT/VaeSpawner"
        private const val VAE_BOOT_TIMEOUT_MS = 30_000L
        private const val TARGET_WAIT_TIMEOUT_MS = 120_000L
        private const val VAE_MARKER = "/data/local/tmp/arift_vae.marker"
    }

    private val processManager = ProcessManager()

    /** Launch the V.A.E runtime service and wait for the container to report ready. */
    fun bootContainer(): Boolean {
        Log.i(TAG, "Booting V.A.E container...")
        val intent = Intent(context, VaeService::class.java)
        context.startForegroundService(intent)
        val deadline = SystemClock.uptimeMillis() + VAE_BOOT_TIMEOUT_MS
        while (SystemClock.uptimeMillis() < deadline) {
            if (VaeService.isContainerReady()) {
                Log.i(TAG, "V.A.E container ready")
                return true
            }
            SystemClock.sleep(500)
        }
        Log.w(TAG, "V.A.E container boot timed out")
        return false
    }

    /** Wait for the target game process to appear, then return its info. */
    fun locateTarget(): ProcessManager.ProcInfo {
        val deadline = SystemClock.uptimeMillis() + TARGET_WAIT_TIMEOUT_MS
        var last = 0L
        while (SystemClock.uptimeMillis() < deadline) {
            val info = processManager.findTarget()
            if (info != null) {
                Log.i(TAG, "Target located: pid=${info.pid} name=${info.name}")
                return info
            }
            SystemClock.sleep(750)
            last = SystemClock.uptimeMillis()
        }
        throw IllegalStateException("Target process never appeared in V.A.E")
    }

    /** Resolve the game's primary library base address for the injection core. */
    fun resolveLibBase(pid: Int): Long {
        val (lib, base) = processManager.resolveLibBaseAuto(pid)
        if (base == 0L) {
            Log.w(TAG, "No known game lib base found for pid=$pid")
        } else {
            Log.i(TAG, "Resolved $lib base = 0x${base.toString(16)}")
        }
        return base
    }

    /** Tear down the container after detach. */
    fun teardownContainer() {
        Log.i(TAG, "Tearing down V.A.E container")
        VaeService.requestShutdown()
    }

    fun isVaePresent(): Boolean {
        return try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                VaeService.isContainerReady()
            } else {
                ProcessManager().let { it.findTarget() != null }
            }
        } catch (t: Throwable) {
            Log.e(TAG, "V.A.E check failed", t)
            false
        }
    }
}