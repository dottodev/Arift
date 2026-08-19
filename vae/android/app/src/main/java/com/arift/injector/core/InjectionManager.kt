package com.arift.injector.core

import android.content.Context
import android.os.SystemClock
import android.util.Log
import com.arift.injector.vae.VaeSpawner
import java.io.File
import java.util.concurrent.Executors

/**
 * InjectionManager orchestrates the full injector lifecycle:
 *
 *   prepare()    -> extract native config, load bridge, warm caches
 *   launch()     -> boot V.A.E container, launch MLBB inside it,
 *                   wait for the process, attach, enable features
 *   autoAttach() -> scan the virtual space; if MLBB is already running,
 *                   attach to it automatically
 *   shutdown()   -> detach, kill container, release everything
 *
 * All heavy work is funneled through a single-threaded executor so state
 * transitions stay serialized.
 */
class InjectionManager(private val context: Context) {

    companion object {
        private const val TAG = "ARIFT/InjectionManager"

        const val FEATURE_ESP = 1
        const val FEATURE_MAP_HACK = 2
        const val FEATURE_AUTO_RETRI = 3
        const val FEATURE_AUTO_AIM = 4
        const val FEATURE_ENEMY_LAG = 5
        const val FEATURE_RANK_BOOSTER = 6
        const val FEATURE_TANK_DEFENSE = 7
        const val FEATURE_PHYSICAL_DAMAGE = 8
        const val FEATURE_VOID_BAN = 9
        const val FEATURE_MUSIC = 10
    }

    enum class State {
        IDLE, PREPARING, READY, LAUNCHING, ATTACHED, ACTIVE, SHUTTING_DOWN
    }

    private val executor = Executors.newSingleThreadExecutor { r ->
        Thread(r, "arift-injection").apply { isDaemon = true }
    }

    @Volatile
    var state: State = State.IDLE
        private set

    @Volatile
    var targetPid: Int = -1
        private set

    @Volatile
    var targetLibBase: Long = 0L
        private set

    /** When true, the background scanner attaches to MLBB the moment it appears. */
    @Volatile
    var autoAttachEnabled: Boolean = true
        private set

    private var spawner: VaeSpawner? = null

    fun prepare() {
        synchronized(this) {
            if (state != State.IDLE) return
            state = State.PREPARING
        }
        executor.execute {
            try {
                ensureConfigDir()
                NativeBridge.ensureLoadedOrThrow()
                state = State.READY
                Log.i(TAG, "Injection core ready: ${NativeBridge.nativeVersion()}")
            } catch (t: Throwable) {
                state = State.IDLE
                Log.e(TAG, "Prepare failed", t)
            }
        }
    }

    /** Block until the native core is READY (re-triggers prepare if needed). */
    fun ensureReady(timeoutMs: Long = 15_000): Boolean {
        if (state == State.READY) return true
        if (state == State.IDLE) prepare()
        val deadline = SystemClock.uptimeMillis() + timeoutMs
        while (SystemClock.uptimeMillis() < deadline) {
            val s = state
            if (s == State.READY) return true
            if (s != State.PREPARING) return false
            SystemClock.sleep(100)
        }
        return state == State.READY
    }

    fun isAttached(): Boolean = state == State.ATTACHED || state == State.ACTIVE

    /**
     * Full injection flow: boot the container, launch MLBB inside the
     * virtual space if it is not running yet, wait for its process,
     * resolve the library base and attach the core.
     */
    fun launch(onResult: (Result<Unit>) -> Unit = {}) {
        if (!ensureReady()) {
            onResult(Result.failure(IllegalStateException("Core not ready (state=$state)")))
            return
        }
        if (isAttached()) {
            onResult(Result.success(Unit))
            return
        }
        synchronized(this) {
            if (state != State.READY) {
                onResult(Result.failure(IllegalStateException("Busy (state=$state)")))
                return
            }
            state = State.LAUNCHING
            autoAttachEnabled = true
        }
        executor.execute {
            try {
                val s = VaeSpawner(context)
                spawner = s
                s.bootContainer()
                var proc = s.locateTargetOnce()
                if (proc == null) {
                    s.launchGame()
                    proc = s.locateTarget()
                }
                doAttach(proc.pid, s.resolveLibBase(proc.pid))
                onResult(Result.success(Unit))
            } catch (t: Throwable) {
                state = State.READY
                Log.e(TAG, "Launch failed", t)
                onResult(Result.failure(t))
            }
        }
    }

    /**
     * Scan the virtual space once for MLBB. If it is already running,
     * attach to it immediately. Returns true when the scan found and
     * queued an attachment.
     */
    fun autoAttachIfPresent(): Boolean {
        if (!autoAttachEnabled) return false
        if (isAttached()) return true
        val s = state
        if (s != State.READY && s != State.IDLE) return false
        val info = try {
            ProcessManager().findTarget()
        } catch (t: Throwable) {
            null
        } ?: return false
        synchronized(this) {
            if (state != State.READY) return false
            state = State.LAUNCHING
        }
        executor.execute {
            try {
                val sp = VaeSpawner(context)
                spawner = sp
                doAttach(info.pid, sp.resolveLibBase(info.pid))
                Log.i(TAG, "Auto-attached to ${info.name} pid=${info.pid}")
            } catch (t: Throwable) {
                state = State.READY
                Log.e(TAG, "Auto-attach failed", t)
            }
        }
        return true
    }

    fun shutdown() {
        autoAttachEnabled = false
        state = State.SHUTTING_DOWN
        executor.execute {
            try {
                if (NativeBridge.isAlive()) NativeBridge.detach()
                spawner?.teardownContainer()
                spawner = null
                synchronized(pidLock) {
                    targetPid = -1
                    targetLibBase = 0L
                }
            } catch (t: Throwable) {
                Log.e(TAG, "Shutdown error", t)
            } finally {
                state = State.IDLE
            }
        }
    }

    fun setFeature(feature: Int, enabled: Boolean): Boolean {
        if (!NativeBridge.loaded) return false
        return NativeBridge.nativeSetFeature(feature, enabled) == 0
    }

    fun isFeatureEnabled(feature: Int): Boolean {
        if (!NativeBridge.loaded) return false
        return NativeBridge.nativeIsFeatureEnabled(feature)
    }

    fun activeFeatureMask(): Long {
        if (!NativeBridge.loaded) return 0L
        return NativeBridge.nativeFeaturesMask()
    }

    fun diagSnapshot(): String {
        if (!NativeBridge.loaded) return "native not loaded"
        return NativeBridge.nativeDiagDump()
    }

    fun espSetRenderMode(mode: Int) { NativeBridge.nativeEspSetRenderMode(mode) }
    fun espSetBoxes(v: Boolean) { NativeBridge.nativeEspSetBoxes(v) }
    fun espSetHealthBars(v: Boolean) { NativeBridge.nativeEspSetHealthBars(v) }
    fun espSetNames(v: Boolean) { NativeBridge.nativeEspSetNames(v) }
    fun espSetCooldowns(v: Boolean) { NativeBridge.nativeEspSetCooldowns(v) }
    fun espSetObjectives(v: Boolean) { NativeBridge.nativeEspSetObjectives(v) }
    fun espSetDistance(v: Boolean) { NativeBridge.nativeEspSetDistance(v) }

    fun mapHackSetFogBypass(v: Boolean) { NativeBridge.nativeMapHackSetFogBypass(v) }
    fun mapHackSetMinimapOverride(v: Boolean) { NativeBridge.nativeMapHackSetMinimapOverride(v) }
    fun mapHackSetVisionRadius(r: Float) { NativeBridge.nativeMapHackSetVisionRadius(r) }

    fun rankBoosterSetEnabled(v: Boolean) { NativeBridge.nativeRbSetEnabled(v) }
    fun rankBoosterSetAggression(l: Int) { NativeBridge.nativeRbSetAggression(l) }
    fun rankBoosterSetTargetRank(r: Int) { NativeBridge.nativeRbSetTargetRank(r) }
    fun rankBoosterSnapshot(): String = NativeBridge.nativeRbSnapshot()

    // ------------------------------------------------------------------

    private val pidLock = Any()

    private fun doAttach(pid: Int, base: Long) {
        synchronized(pidLock) {
            targetPid = pid
            targetLibBase = base
        }
        val ok = NativeBridge.attach(pid, base)
        if (!ok) {
            throw IllegalStateException("Attach failed: ${NativeBridge.nativeLastError()}")
        }
        state = State.ATTACHED
        applyStoredFeatureMask()
        state = State.ACTIVE
        Log.i(TAG, "Attached to pid=$pid base=0x${base.toString(16)}")
    }

    private fun ensureConfigDir() {
        val dir = File(context.filesDir, "arift")
        if (!dir.exists()) dir.mkdirs()
        NativeBridge.nativeInit(dir.absolutePath)
    }

    private fun applyStoredFeatureMask() {
        val prefs = context.getSharedPreferences("arift_features", Context.MODE_PRIVATE)
        for (feature in 1..10) {
            val enabled = prefs.getBoolean("feature_$feature", false)
            if (enabled) NativeBridge.nativeSetFeature(feature, true)
        }
    }

    fun persistFeature(feature: Int, enabled: Boolean) {
        context.getSharedPreferences("arift_features", Context.MODE_PRIVATE)
            .edit()
            .putBoolean("feature_$feature", enabled)
            .apply()
    }
}