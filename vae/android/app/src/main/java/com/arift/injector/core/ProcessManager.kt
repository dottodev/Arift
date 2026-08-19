package com.arift.injector.core

import android.os.SystemClock
import android.util.Log
import java.io.File
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * ProcessManager — reads /proc for the target process inside the V.A.E,
 * resolves the base address of the game's primary native library
 * (libil2cpp.so / libunity.so) and exposes helpers for the native core.
 */
class ProcessManager {

    companion object {
        private const val TAG = "ARIFT/ProcessManager"

        val GAME_PROCESS_NAMES = listOf(
            "com.mobile.legends",
            "com.mobilelegends",
            "com.moonton.mobile.legends"
        )

        val GAME_LIBS = listOf(
            "libil2cpp.so",
            "libunity.so",
            "libgame.so",
            "libMlbb.so"
        )
    }

    data class ProcInfo(
        val pid: Int,
        val name: String,
        val uid: Int,
        val startTime: Long
    )

    /** Locate the MLBB process among running processes. */
    fun findTarget(): ProcInfo? {
        val now = SystemClock.uptimeMillis()
        val procDir = File("/proc")
        val candidates = procDir.listFiles { f -> f.name.toIntOrNull() != null } ?: return null
        for (dir in candidates) {
            val info = readProcInfo(dir.name.toInt()) ?: continue
            if (info.name in GAME_PROCESS_NAMES) {
                Log.i(TAG, "Target found: ${info.name} pid=${info.pid}")
                return info
            }
        }
        Log.w(TAG, "Target not found after ${now}ms scan")
        return null
    }

    fun readProcInfo(pid: Int): ProcInfo? {
        return try {
            val name = readLine("/proc/$pid/cmdline") ?: return null
            val status = readFile("/proc/$pid/status")
            val uid = Regex("Uid:\\s+(\\d+)").find(status ?: "")?.groupValues?.get(1)?.toIntOrNull() ?: -1
            val start = Regex("starttime:\\s+(\\d+)").find(status ?: "")?.groupValues?.get(1)?.toLongOrNull() ?: 0L
            ProcInfo(pid, name, uid, start)
        } catch (t: Throwable) {
            null
        }
    }

    /** Resolve the base address of a native library in the target maps. */
    fun resolveLibBase(pid: Int, libName: String = GAME_LIBS.first()): Long {
        val maps = readFile("/proc/$pid/maps") ?: return 0L
        val wanted = libName.let { if (it.endsWith(".so")) it else "$it.so" }
        for (line in maps.lineSequence()) {
            if (!line.contains(wanted)) continue
            val addrHex = line.substringBefore('-').trim()
            val base = addrHex.toLongOrNull(16) ?: continue
            if (line.contains("r-xp")) return base
        }
        return 0L
    }

    fun resolveLibBaseAuto(pid: Int): Pair<String, Long> {
        val maps = readFile("/proc/$pid/maps") ?: return "" to 0L
        for (lib in GAME_LIBS) {
            for (line in maps.lineSequence()) {
                if (!line.contains(lib) || !line.contains("r-xp")) continue
                val base = line.substringBefore('-').trim().toLongOrNull(16) ?: continue
                return lib to base
            }
        }
        return "" to 0L
    }

    /** Read target memory (used only by native core; kept here for parity tests). */
    fun readMemory(pid: Int, address: Long, size: Int): ByteArray? {
        return try {
            val memFile = File("/proc/$pid/mem")
            if (!memFile.canRead()) return null
            RandomAccessFile(memFile, "r").use { raf ->
                raf.seek(address)
                val buf = ByteArray(size)
                raf.readFully(buf)
                buf
            }
        } catch (t: Throwable) {
            null
        }
    }

    fun readMemoryLong(pid: Int, address: Long): Long? {
        val bytes = readMemory(pid, address, 8) ?: return null
        return ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).long
    }

    fun isAlive(pid: Int): Boolean {
        return try {
            readLine("/proc/$pid/cmdline") != null
        } catch (t: Throwable) {
            false
        }
    }

    fun enumerateThreads(pid: Int): List<Int> {
        val dir = File("/proc/$pid/task") ?: return emptyList()
        return dir.listFiles { f -> f.name.toIntOrNull() != null }
            ?.map { it.name.toInt() }
            ?: emptyList()
    }

    // ------------------------------------------------------------------

    private fun readLine(path: String): String? {
        return try {
            File(path).readText().trim().split('\u0000').firstOrNull()?.trim()
        } catch (t: Throwable) {
            null
        }
    }

    private fun readFile(path: String): String? {
        return try {
            File(path).readText()
        } catch (t: Throwable) {
            null
        }
    }
}