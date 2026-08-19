package com.arift.injector.core

import android.content.Context
import android.util.Log
import com.arift.injector.AriftApplication
import com.arift.injector.vae.VaeSpawner
import java.io.File
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

/**
 * InjectionManager orchestrates the full injector lifecycle:
 *
 *   prepare()  -> extract native config, load bridge, warm caches
 *   launch()   -> boot V.A.E container + spawn target, attach, enable features
 *   shutdown() -> detach, kill container, release everything
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

    private val _state = AtomicBoolean(false)
    private val pidLock = Any()

    @Volatile
    var state: State = State.IDLE
        private set

    @Volatile
    var targetPid: Int = -1
        private set

    @Volatile
    var targetLibBase: Long = 0L
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

    fun launch(onResult: (Result<Unit>) -> Unit = {}) {
        if (state != State.READY) {
            onResult(Result.failure(IllegalStateException("Core not ready (state=$state)")))
            return
        }
        state = State.LAUNCHING
        executor.execute {
            try {
                val s = VaeSpawner(context)
                spawner = s
                s.bootContainer()
                val proc = s.locateTarget()
                val pid = proc.pid
                val base = s.resolveLibBase(pid)
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
                onResult(Result.success(Unit))
            } catch (t: Throwable) {
                state = State.READY
                onResult(Result.failure(t))
            }
        }
    }

    fun shutdown() {
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