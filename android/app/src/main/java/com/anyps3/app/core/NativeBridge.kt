package com.anyps3.core

import org.json.JSONObject

/**
 * Thin wrapper around the native core (libany_ps3.so).
 *
 * Every ISO-related call takes an open [android.os.ParcelFileDescriptor]
 * file descriptor — the core reads the image through random-access
 * seek/read on that descriptor. The ISO is NEVER copied into app storage
 * and NEVER fully loaded into RAM.
 */
object NativeBridge {
    init {
        System.loadLibrary("any_ps3")
    }

    /** Progress sink implemented on the Kotlin side (called on the calling thread). */
    fun interface Progress {
        fun onProgress(stageIndex: Int, fraction: Float, line: String)
    }

    private fun progressProxy(progress: Progress): Any = object {
        @Suppress("unused")
        fun onProgress(stageIndex: Int, fraction: Float, line: String) {
            progress.onProgress(stageIndex, fraction, line)
        }
    }

    fun engineVersion(): String = nativeEngineVersion()

    /** Inspect a PS3 ISO through an open fd. Returns JSONObject text. */
    fun inspectIsoFd(fd: Int): JSONObject = JSONObject(nativeInspectIsoFd(fd))

    /** Parse PARAM.SFO directly (folder import). */
    fun parseSfoFd(fd: Int): JSONObject = JSONObject(nativeParseSfoFd(fd))

    /** Copy PS3_GAME/<entry> out of the ISO into an open output fd (chunked). */
    fun copyIsoEntryToFd(isoFd: Int, entry: String, outFd: Int): JSONObject =
        JSONObject(nativeCopyIsoEntryToFd(isoFd, entry, outFd))

    /** Streamed SHA-256 of the whole image (outdated check / refresh). */
    fun computeIsoHash(fd: Int, progress: Progress): JSONObject =
        JSONObject(nativeComputeIsoHash(fd, progressProxy(progress)))

    /** Run the honest AOT pipeline; write manifest.json to [manifestFd]. */
    fun compileIsoFd(isoFd: Int, manifestFd: Int, gameIdHint: String,
                     progress: Progress): JSONObject =
        JSONObject(nativeCompileIsoFd(isoFd, manifestFd, gameIdHint, progressProxy(progress)))

    fun readManifestFd(fd: Int): JSONObject = JSONObject(nativeReadManifestFd(fd))

    fun checkOutdatedIsoFd(isoFd: Int, manifestFd: Int, progress: Progress): JSONObject =
        JSONObject(nativeCheckOutdatedIsoFd(isoFd, manifestFd, progressProxy(progress)))

    // ---- raw JNI ----
    private external fun nativeEngineVersion(): String
    private external fun nativeInspectIsoFd(fd: Int): String
    private external fun nativeParseSfoFd(fd: Int): String
    private external fun nativeCopyIsoEntryToFd(isoFd: Int, entry: String, outFd: Int): String
    private external fun nativeComputeIsoHash(fd: Int, progress: Any): String
    private external fun nativeCompileIsoFd(isoFd: Int, manifestFd: Int, gameIdHint: String,
                                            progress: Any): String
    private external fun nativeReadManifestFd(fd: Int): String
    private external fun nativeCheckOutdatedIsoFd(isoFd: Int, manifestFd: Int, progress: Any): String
}
