package com.arift.injector.core

import android.util.Log
import com.arift.injector.AriftApplication

/**
 * JNI bridge into libarift_core.so — the native injection core.
 *
 * Every native method here is backed by the C++ bridge in
 * vae/native/src/jni_bridge.cpp. Keep names in sync with the native side.
 */
object NativeBridge {

    private const val LIB_NAME = "arift_core"
    private const val TAG = "ARIFT/Native"

    var loaded: Boolean = false
        private set

    @Volatile
    var attachedToTarget: Boolean = false
        private set

    init {
        load()
    }

    @Synchronized
    fun load(): Boolean {
        if (loaded) return true
        return try {
            System.loadLibrary(LIB_NAME)
            loaded = true
            Log.i(TAG, "libarift_core loaded (v${nativeVersion()})")
            true
        } catch (t: Throwable) {
            Log.e(TAG, "Failed to load libarift_core", t)
            false
        }
    }

    // ------------------------------------------------------------------
    // Core lifecycle
    // ------------------------------------------------------------------

    external fun nativeVersion(): String

    external fun nativeInit(configPath: String): Int

    external fun nativeShutdown(): Int

    external fun nativeStatus(): Int

    // ------------------------------------------------------------------
    // Target attachment (MLBB process inside V.A.E)
    // ------------------------------------------------------------------

    external fun nativeAttach(pid: Int, libBase: Long): Int

    external fun nativeDetach(): Int

    external fun nativeIsAttached(): Boolean

    external fun nativeTargetPid(): Int

    external fun nativeProbeMemory(pid: Int, libBase: Long): String

    external fun nativeRootInfo(): String

    // ------------------------------------------------------------------
    // Feature switches
    // ------------------------------------------------------------------

    external fun nativeSetFeature(feature: Int, enabled: Boolean): Int

    external fun nativeIsFeatureEnabled(feature: Int): Boolean

    external fun nativeFeaturesMask(): Long

    // ------------------------------------------------------------------
    // ESP
    // ------------------------------------------------------------------

    external fun nativeEspSetRenderMode(mode: Int): Int

    external fun nativeEspSetBoxes(enabled: Boolean): Int

    external fun nativeEspSetHealthBars(enabled: Boolean): Int

    external fun nativeEspSetNames(enabled: Boolean): Int

    external fun nativeEspSetCooldowns(enabled: Boolean): Int

    external fun nativeEspSetObjectives(enabled: Boolean): Int

    external fun nativeEspSetDistance(enabled: Boolean): Int

    external fun nativeEspRefreshNow(): Int

    // ------------------------------------------------------------------
    // Map hack
    // ------------------------------------------------------------------

    external fun nativeMapHackSetFogBypass(enabled: Boolean): Int

    external fun nativeMapHackSetMinimapOverride(enabled: Boolean): Int

    external fun nativeMapHackSetVisionRadius(radius: Float): Int

    // ------------------------------------------------------------------
    // Rank booster
    // ------------------------------------------------------------------

    external fun nativeRbSetEnabled(enabled: Boolean): Int

    external fun nativeRbSetAggression(level: Int): Int

    external fun nativeRbSetTargetRank(rank: Int): Int

    external fun nativeRbSnapshot(): String

    external fun nativeRbPump(): Int

    // ------------------------------------------------------------------
    // Diagnostics
    // ------------------------------------------------------------------

    external fun nativeLastError(): String

    external fun nativeDiagDump(): String

    fun attach(pid: Int, libBase: Long): Boolean {
        val rc = nativeAttach(pid, libBase)
        attachedToTarget = rc == 0
        return attachedToTarget
    }

    fun detach() {
        nativeDetach()
        attachedToTarget = false
    }

    fun isAlive(): Boolean = loaded && attachedToTarget

    fun ensureLoadedOrThrow() {
        check(loaded) {
            "Native core unavailable — reinstall or rebuild libarift_core.so"
        }
    }
}