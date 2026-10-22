// Any_Ps3 — native bridge surface. The `external` declarations must exactly match
// the JNI exports in app/src/main/cpp/anyps3_jni.cpp.
package com.anyps3.app

object NativeBridge {
    init {
        System.loadLibrary("anyps3")
    }

    /** Runtime callbacks, invoked from the emulation thread. */
    abstract class RuntimeCallbacks {
        open fun onLog(line: String) {}
        open fun onProgress(percent: Int, note: String) {}
        open fun onState(state: Int) {} // 0 stopped, 1 booting, 2 running, 3 exiting
    }

    external fun nativeInit(configDir: String)
    external fun nativeFirstRunAccepted(): Boolean
    external fun nativeAcceptFirstRun()
    external fun nativeImportGame(path: String): String // "OK id|title|cat" or "ERR ..."
    external fun nativeListGames(): String // "id|title|app_ver|icon_path;..."
    external fun nativeRemoveGame(id: String): Boolean
    external fun nativeBoot(id: String, callbacks: RuntimeCallbacks): Boolean
    external fun nativeRun(): Int
    external fun nativeStop()
    external fun nativeGetGameSettings(id: String): String // "scale|frame_limit|shader|backend"
    external fun nativeSetGameSettings(id: String, kv: String) // "key=value;key=value"
}
