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
 * Every interesting transition is recorded in a small event ring buffer
 * so the app UI can show live proof of life ("MLBB FOUND", "Attached").
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

    /** Last MLBB process seen in the virtual space (even before attach). */
    @Volatile
    var lastFoundPid: Int = -1
        private set

    /** Last injection-verification summary (dedupe key for the event log). */
    @Volatile
    var lastVerify: String = ""
        private set

    /** Last environment info (root/SELinux) — dedupe key for the event log. */
    @Volatile
    private var lastEnvInfo: String = ""

    /** When true, the background scanner attaches to MLBB the moment it appears. */
    @Volatile
    var autoAttachEnabled: Boolean = true
        private set

    private val events = ArrayDeque<String>()

    private var spawner: VaeSpawner? = null

    // ------------------------------------------------------------------
    // Event log (shown live in the app status card)
    // ------------------------------------------------------------------

    fun recentEvents(): List<String> = synchronized(events) { events.toList() }

    fun addEvent(msg: String) {
        synchronized(events) {
            events.addLast(msg)
            while (events.size > 8) events.removeFirst()
        }
        Log.i(TAG, msg)
    }

    // ------------------------------------------------------------------
    // Core lifecycle
    // ------------------------------------------------------------------

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
                addEvent("Core ready: ${NativeBridge.nativeVersion()}")
            } catch (t: Throwable) {
                state = State.IDLE
                addEvent("Core load failed: ${t.message}")
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
                addEvent("Booting V.A.E container\u2026")
                s.bootContainer()
                var proc = s.locateTargetOnce()
                if (proc == null) {
                    addEvent("Launching MLBB inside V.A.E\u2026")
                    s.launchGame()
                    proc = s.locateTarget()
                }
                lastFoundPid = proc.pid
                addEvent("MLBB found: pid=${proc.pid} (${proc.name})")
                doAttach(proc.pid, s.resolveLibBase(proc.pid))
                addEvent("Injected: pid=${proc.pid}")
                onResult(Result.success(Unit))
            } catch (t: Throwable) {
                state = State.READY
                addEvent("Inject failed: ${t.message}")
                Log.e(TAG, "Launch failed", t)
                onResult(Result.failure(t))
            }
        }
    }

    /**
     * Report whether MLBB is visible in the virtual space right now.
     * Records an event the first time a new pid is seen. Does not attach.
     */
    fun reportTargetPresence(): Boolean {
        val info = findTargetOnce()
        if (info != null && info.pid != lastFoundPid) {
            lastFoundPid = info.pid
            addEvent("MLBB found: pid=${info.pid} (${info.name})")
        }
        return info != null
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
        val info = findTargetOnce() ?: return false
        if (info.pid != lastFoundPid) {
            lastFoundPid = info.pid
            addEvent("MLBB found: pid=${info.pid} (${info.name})")
        }
        synchronized(this) {
            if (state != State.READY) return false
            state = State.LAUNCHING
        }
        executor.execute {
            try {
                val sp = VaeSpawner(context)
                spawner = sp
                doAttach(info.pid, sp.resolveLibBase(info.pid))
                addEvent("Attached: pid=${info.pid} (${info.name})")
            } catch (t: Throwable) {
                state = State.READY
                addEvent("Attach failed: ${t.message}")
                Log.e(TAG, "Auto-attach failed", t)
            }
        }
        return true
    }

    /**
     * One-shot on-device verification: finds MLBB in the virtual space,
     * reports attach state, counts the readable memory regions, resolves
     * the game library base and the feature mask. Records an event only
     * when the result actually changes (no log spam).
     */
    fun verifyInjection(): String {
        val pm = ProcessManager()
        val info = pm.findTarget()
        if (info == null) {
            if (lastVerify != "no-target") {
                lastVerify = "no-target"
                addEvent("Verify: MLBB not found")
            }
            return "MLBB not found"
        }
        if (info.pid != lastFoundPid) {
            lastFoundPid = info.pid
            addEvent("MLBB found: pid=${info.pid} (${info.name})")
        }
        val attached = NativeBridge.loaded && NativeBridge.isAlive()
        val maps = pm.readFile("/proc/${info.pid}/maps") ?: ""
        val regions = maps.lineSequence().count { it.isNotBlank() }
        var lib = "none"
        var base = 0L
        for (g in ProcessManager.GAME_LIBS) {
            var found = false
            for (line in maps.lineSequence()) {
                if (line.contains(g) && line.contains("r-xp")) {
                    lib = g
                    base = line.substringBefore('-').trim().toLongOrNull(16) ?: 0L
                    found = true
                    break
                }
            }
            if (found) break
        }
        val mask = if (attached) NativeBridge.nativeFeaturesMask() else 0L
        val summary = buildString {
            append("MLBB pid=${info.pid}")
            append(" | attach=${if (attached) "OK" else "no"}")
            append(" | regions=$regions")
            if (lib != "none") append(" | $lib @ 0x${base.toString(16)}")
            append(" | mask=0x${mask.toString(16)}")
        }
        if (summary != lastVerify) {
            lastVerify = summary
            addEvent("Verify: $summary")
        }
        return summary
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
                lastFoundPid = -1
                addEvent("Stopped")
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

    private fun findTargetOnce(): ProcessManager.ProcInfo? {
        return try {
            ProcessManager().findTarget()
        } catch (t: Throwable) {
            null
        }
    }

    private fun doAttach(pid: Int, base: Long) {
        synchronized(pidLock) {
            targetPid = pid
            targetLibBase = base
        }
        val pm = ProcessManager()
        val probe = NativeBridge.nativeProbeMemory(pid, base)
        val env = NativeBridge.nativeRootInfo()
        if (env != lastEnvInfo) {
            lastEnvInfo = env
            addEvent("Env: $env")
        }
        val gameName = pm.readFile("/proc/$pid/cmdline")
            ?.substringBefore('\u0000')?.trim().orEmpty()
        val info = pm.readProcInfo(pid)
        if (info != null && info.uid != android.os.Process.myUid()) {
            addEvent("UID mismatch: game=${info.uid} vs us=${android.os.Process.myUid()}")
        }
        if (probe.startsWith("ok")) {
            addEvent("Memory: $probe")
        } else {
            addEvent("Memory blocked: $probe — killing anti-cheat\u2026")
            val killed = pm.killGameSubprocesses(
                pid, gameName.ifEmpty { ProcessManager.GAME_PROCESS_NAMES.first() })
            if (killed.isNotEmpty()) addEvent("Anti-cheat killed: ${killed.joinToString()}")
            addEvent("Memory retry: ${NativeBridge.nativeProbeMemory(pid, base)}")
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