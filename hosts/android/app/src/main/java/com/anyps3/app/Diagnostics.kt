// Any_Ps3 — diagnostics + log export helpers (Android).
//
// This module exists because the share-sheet-only export path crashed for
// users on Android 13 (see v0.6.0 changelog). Design rules applied here:
//   1. PRIMARY export path = MediaStore insert into the public Downloads
//      folder (works on Android 10+ with NO permissions and NO share sheet;
//      the user gets a real file they can open with any file manager).
//   2. Share sheet is only a SECONDARY path.
//   3. Every entry point catches Throwable (not just Exception) so that
//      Error-class failures (UnsatisfiedLinkError, NoClassDefFoundError,
//      OutOfMemoryError, ...) can never kill the app from an export tap.
//   4. All file I/O runs on the caller-provided IO dispatcher; callers show
//      the returned status in the UI banner.
package com.anyps3.app

import android.content.ContentValues
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import androidx.core.content.FileProvider
import java.io.File
import java.io.IOException

object Diagnostics {

    /** Human-visible app version (from the package manager). */
    fun appVersion(context: Context): String = try {
        @Suppress("DEPRECATION")
        context.packageManager.getPackageInfo(context.packageName, 0).versionName ?: "?"
    } catch (t: Throwable) {
        "?"
    }

    /** Device / OS / CPU block for reports. Never throws. */
    fun deviceBlock(): String = try {
        val sb = StringBuilder()
        sb.append("device: ").append(Build.MANUFACTURER).append(' ')
            .append(Build.MODEL).append(" (").append(Build.DEVICE).append(")\n")
        if (Build.VERSION.SDK_INT >= 31) {
            sb.append("SoC: ").append(Build.SOC_MANUFACTURER).append(' ')
                .append(Build.SOC_MODEL).append('\n')
        }
        sb.append("CPU ABIs: ").append(Build.SUPPORTED_ABIS.joinToString(", ")).append('\n')
        sb.append("OS: Android ").append(Build.VERSION.RELEASE)
            .append(" (API ").append(Build.VERSION.SDK_INT).append(")\n")
        sb.toString()
    } catch (t: Throwable) {
        "device: unknown (${t.javaClass.simpleName})\n"
    }

    fun gpuLine(context: Context): String = try {
        val am = context.getSystemService(Context.ACTIVITY_SERVICE) as android.app.ActivityManager
        val info = am.deviceConfigurationInfo
        "OpenGL ES: ${info.glEsVersion} (required 0x${Integer.toHexString(info.reqGlEsVersion)})"
    } catch (t: Throwable) {
        "OpenGL ES: unknown (${t.javaClass.simpleName})"
    }

    fun vulkanLine(context: Context): String = try {
        val reported = context.packageManager.hasSystemFeature(
            android.content.pm.PackageManager.FEATURE_VULKAN_HARDWARE_VERSION)
        "Vulkan hardware feature: ${if (reported) "reported by this device" else "NOT reported by this device"}"
    } catch (t: Throwable) {
        "Vulkan hardware feature: unknown (${t.javaClass.simpleName})"
    }

    /** Real storage usage: app-private library dir + cache dir, in bytes. */
    fun storageStats(context: Context): Pair<Long, Long> = try {
        val lib = dirSize(File(context.filesDir, "anyps3"))
        val cache = dirSize(context.cacheDir)
        lib to cache
    } catch (t: Throwable) {
        0L to 0L
    }

    private fun dirSize(dir: File): Long {
        var size = 0L
        val files = dir.listFiles() ?: return 0L
        for (f in files) size += if (f.isDirectory) dirSize(f) else f.length()
        return size
    }

    /** Delete only files this module created in cacheDir (diagnostics/crash exports). */
    fun clearCachedReports(context: Context): Int = try {
        var n = 0
        val files = context.cacheDir.listFiles() ?: return 0
        for (f in files) {
            val name = f.name
            if ((name.startsWith("anyps3-diagnostics-") || name.startsWith("anyps3-crash-export")) &&
                name.endsWith(".txt") && f.isFile) {
                if (f.delete()) n++
            }
        }
        n
    } catch (t: Throwable) {
        0
    }

    fun formatBytes(bytes: Long): String = when {
        bytes >= 1 shl 20 -> String.format(java.util.Locale.US, "%.1f MB", bytes / 1048576.0)
        bytes >= 1 shl 10 -> String.format(java.util.Locale.US, "%.1f KB", bytes / 1024.0)
        else -> "$bytes B"
    }

    /** Full diagnostics report (version, system, games, log tail). Never throws. */
    fun buildReportText(
        context: Context,
        version: String,
        statusText: String,
        gamesBlock: String,
        logText: String
    ): String = try {
        val sb = StringBuilder()
        sb.append("Any_Ps3 ").append(version).append(" — diagnostics report\n")
        sb.append("======================================\n\n")
        sb.append("[System]\n").append(deviceBlock())
        sb.append(gpuLine(context)).append('\n')
        sb.append(vulkanLine(context)).append("\n\n")
        val (lib, cache) = storageStats(context)
        sb.append("[Storage]\n")
        sb.append("config: ").append(File(context.filesDir, "anyps3").absolutePath).append('\n')
        sb.append("cache: ").append(context.cacheDir.absolutePath).append('\n')
        sb.append("library data: ").append(formatBytes(lib))
            .append(", logs/cache: ").append(formatBytes(cache)).append("\n\n")
        sb.append("[Engine]\n").append(statusText).append("\n\n")
        sb.append("[Games]\n").append(gamesBlock).append('\n')
        sb.append("[Log — last 500 lines]\n")
        val lines = logText.split('\n')
        val tail = if (lines.size > 500) lines.takeLast(500) else lines
        sb.append(tail.joinToString("\n")).append('\n')
        sb.toString()
    } catch (t: Throwable) {
        "report build failed: ${t.javaClass.name}: ${t.message}"
    }

    /**
     * PRIMARY export path: save [text] as a real file the user can find,
     * in Download/Any_Ps3/ via MediaStore (Android 10+). No permissions
     * needed because we create the file ourselves.
     * @return human-readable location string
     * @throws IOException on failure (caller must catch Throwable)
     */
    fun saveToDownloads(context: Context, fileName: String, text: String): String {
        if (Build.VERSION.SDK_INT >= 29) {
            val resolver = context.contentResolver
            val values = ContentValues().apply {
                put(MediaStore.MediaColumns.DISPLAY_NAME, fileName)
                put(MediaStore.MediaColumns.MIME_TYPE, "text/plain")
                put(MediaStore.MediaColumns.RELATIVE_PATH,
                    Environment.DIRECTORY_DOWNLOADS + "/Any_Ps3")
                put(MediaStore.MediaColumns.IS_PENDING, 1)
            }
            val uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)
                ?: throw IOException("MediaStore insert failed")
            try {
                resolver.openOutputStream(uri)?.use { out ->
                    out.write(text.toByteArray(Charsets.UTF_8))
                    out.flush()
                } ?: throw IOException("could not open output stream")
            } catch (t: Throwable) {
                // best-effort cleanup of the pending row
                try { resolver.delete(uri, null, null) } catch (_: Throwable) {}
                throw t
            }
            val done = ContentValues().apply { put(MediaStore.MediaColumns.IS_PENDING, 0) }
            try {
                resolver.update(uri, done, null, null)
            } catch (_: Throwable) {
                // Some OEMs don't allow the pending->done update; the file is
                // usually still written. Not fatal.
            }
            return "Downloads/Any_Ps3/$fileName"
        } else {
            // Android 9 and below: app-specific external documents dir (no
            // permission needed) — still a real file on shared storage.
            val dir = context.getExternalFilesDir(Environment.DIRECTORY_DOCUMENTS)
                ?: File(context.filesDir, "exports").also { it.mkdirs() }
            dir.mkdirs()
            val f = File(dir, fileName)
            f.writeText(text)
            return f.absolutePath
        }
    }

    /** Write into the app cache dir (share-sheet staging). */
    fun writeCacheFile(context: Context, fileName: String, text: String): File {
        val f = File(context.cacheDir, fileName)
        f.writeText(text)
        return f
    }

    /**
     * SECONDARY export path: share a cache file through the system share
     * sheet. Grant flags are set on the chooser AND the inner intent for
     * maximum compatibility.
     */
    fun shareFile(context: Context, file: File, chooserTitle: String) {
        val uri = FileProvider.getUriForFile(context, "${context.packageName}.fileprovider", file)
        val send = Intent(Intent.ACTION_SEND).apply {
            type = "text/plain"
            putExtra(Intent.EXTRA_STREAM, uri)
            putExtra(Intent.EXTRA_TITLE, chooserTitle)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        val chooser = Intent.createChooser(send, chooserTitle).apply {
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        context.startActivity(chooser)
    }
}
