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
    external fun nativeStatus(): String

    // ---- import (NO copy for ISOs: the fd is read in place) ----
    /** ISO via SAF fd: "OK id|title" / "ENCRYPTED id|title[|key:path]" or "ERR ..." */
    external fun nativeImportGameFd(fd: Int, displayName: String, uri: String): String
    /** Folder flow (copied metadata files) / demo: path-based import. */
    external fun nativeImportGame(path: String): String // "OK id|title|cat" or "ERR ..."
    /** Re-import after the user re-selected a moved/lost file. */
    external fun nativeUpdateGameSource(id: String, fd: Int, displayName: String, uri: String): String

    // ---- disc keys (user-provided, app-private) ----
    external fun nativeSetGameKeyFd(id: String, fd: Int): String // "OK"/"ERR ..."
    external fun nativeSetGameKeyText(id: String, key32Hex: String): String // "OK"/"ERR ..."
    external fun nativeHasDiscKey(id: String): Boolean

    // ---- decrypted executable (user's own ELF/fSELF; app-private copy) ----
    external fun nativeSetDecryptedEbootFd(id: String, fd: Int): String // "OK"/"ERR ..."
    external fun nativeHasDecryptedEboot(id: String): Boolean
    external fun nativeDecryptedEbootName(id: String): String
    external fun nativeClearDecryptedEboot(id: String)

    // ---- library ----
    external fun nativeListGames(): String // "id|title|app_ver|icon|enc|haskey;..."
    external fun nativeRemoveGame(id: String): Boolean
    external fun nativeGetGameSourceUri(id: String): String
    external fun nativeEncState(id: String): Int // 0 none, 1 needs key, 2 key in use, -1 absent
    external fun nativeInstallDemo(helloElfPath: String): String // "OK id|title" or "ERR ..."

    // ---- boot/run ----
    external fun nativeBoot(id: String, callbacks: RuntimeCallbacks): Boolean
    external fun nativeBootFd(id: String, fd: Int, callbacks: RuntimeCallbacks): Boolean
    external fun nativeRun(): Int
    external fun nativeStop()

    // ---- settings (real features only) ----
    external fun nativeFirstRunAccepted(): Boolean
    external fun nativeAcceptFirstRun()
    external fun nativeSetLogLevel(level: Int) // 0 info+, 1 warn+, 2 debug, 3 errors only
    external fun nativeGetLogLevel(): Int
    external fun nativeGetGameSettings(id: String): String // "pad_cross=..;..." kv
    external fun nativeSetGameSettings(id: String, kv: String)
}
