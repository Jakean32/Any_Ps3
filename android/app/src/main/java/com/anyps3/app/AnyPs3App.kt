package com.anyps3.app

import android.app.Application
import com.anyps3.app.core.GameDb
import com.anyps3.app.core.Settings

class AnyPs3App : Application() {
    lateinit var db: GameDb
        private set
    lateinit var settings: Settings
        private set

    override fun onCreate() {
        super.onCreate()
        db = GameDb(this)
        settings = Settings(this)
    }
}
