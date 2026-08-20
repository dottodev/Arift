package com.arift.injector

import android.app.Application
import com.arift.injector.core.CrashLog
import com.arift.injector.core.InjectionManager

class AriftApplication : Application() {

    companion object {
        const val TAG = "ARIFT"
        lateinit var instance: AriftApplication
            private set
    }

    val injectionManager: InjectionManager by lazy { InjectionManager(this) }

    override fun onCreate() {
        super.onCreate()
        instance = this
        installCrashLog()
        injectionManager.prepare()
        // Re-surface the last crash on the status card so failures are
        // visible on-device without any PC tooling.
        CrashLog.lastCrashLine(this)?.let {
            injectionManager.addEvent("Last crash: $it")
        }
    }

    private fun installCrashLog() {
        val previous = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, throwable ->
            try {
                CrashLog.append(this, thread, throwable)
                injectionManager.addEvent("Crash: ${throwable.javaClass.simpleName}")
            } catch (_: Throwable) {
                // Never throw from the crash handler itself.
            } finally {
                previous?.uncaughtException(thread, throwable)
            }
        }
    }

    override fun onTerminate() {
        injectionManager.shutdown()
        super.onTerminate()
    }
}