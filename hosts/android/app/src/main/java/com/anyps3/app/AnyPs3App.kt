// Any_Ps3 — Application class.
//  * installs the global uncaught-exception handler: a crash writes
//    files/anyps3/crash_last.txt and then re-delegates to the platform
//    handler so the system still shows the normal crash flow; MainActivity
//    shows + exports the report (in-app, crash-proof) on the next start.
//  * holds the app-scoped UI state (v0.6.0) so the library, log and all
//    status survive device rotation and other activity re-creations.
package com.anyps3.app

import android.app.Application
import java.io.File

class AnyPs3App : Application() {
    val ui = AppUiState()

    override fun onCreate() {
        super.onCreate()
        val previous = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, throwable ->
            try {
                val dir = File(filesDir, "anyps3")
                dir.mkdirs()
                val f = File(dir, "crash_last.txt")
                f.writeText(buildString {
                    append("Any_Ps3 crash report (Android host)\n")
                    append("thread: ").append(thread.name).append('\n')
                    append("exception: ").append(throwable.javaClass.name).append('\n')
                    append("message: ").append(throwable.message ?: "(none)").append('\n')
                    append("\nstack trace:\n")
                    append(android.util.Log.getStackTraceString(throwable))
                    append('\n')
                })
            } catch (t: Throwable) {
                // Never let the crash-log writer itself crash the handler.
            }
            previous?.uncaughtException(thread, throwable)
        }
    }
}
