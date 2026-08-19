package com.arift.injector

import android.app.Application
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
        injectionManager.prepare()
    }

    override fun onTerminate() {
        injectionManager.shutdown()
        super.onTerminate()
    }
}