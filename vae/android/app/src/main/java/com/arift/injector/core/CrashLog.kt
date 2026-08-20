package com.arift.injector.core

import android.content.Context
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * CrashLog — the app's built-in log system. Uncaught exceptions are
 * recorded to a file inside the app's own storage, and the most recent
 * crash is re-surfaced on the status card on the next launch so the
 * user never has to plug into adb to see what went wrong.
 */
object CrashLog {

    private fun file(context: Context): File = File(context.filesDir, "crash_log.txt")

    /** Appends a crash to the log (newest first, capped). */
    fun append(context: Context, thread: Thread, throwable: Throwable) {
        try {
            val ts = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(Date())
            val sb = StringBuilder()
            sb.append("=== ").append(ts).append(" | ").append(thread.name).append(" ===\n")
            sb.append(throwable.toString()).append('\n')
            for (ste in throwable.stackTrace.take(12)) {
                sb.append("  at ").append(ste).append('\n')
            }
            val f = file(context)
            val previous = if (f.exists()) f.readText().takeLast(6000) else ""
            f.writeText(sb.toString() + "\n" + previous)
        } catch (_: Throwable) {
            // Logging must never crash anything itself.
        }
    }

    /** First line of the most recent crash (the "=== timestamp | thread ===" header). */
    fun lastCrashLine(context: Context): String? = try {
        file(context).readText()
            .lineSequence()
            .firstOrNull { it.startsWith("===") }
    } catch (_: Throwable) {
        null
    }

    /** The exception line of the most recent crash, e.g. "java.lang.NullPointerException: ...". */
    fun lastCrashError(context: Context): String? {
        val text = try {
            file(context).readText()
        } catch (_: Throwable) {
            return null
        }
        val lines = text.lineSequence().toList()
        for (i in lines.indices) {
            if (lines[i].startsWith("===")) {
                return lines.getOrNull(i + 1)?.take(220)
            }
        }
        return null
    }
}