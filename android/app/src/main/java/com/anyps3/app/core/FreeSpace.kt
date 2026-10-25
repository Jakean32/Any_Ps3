package com.anyps3.app.core

import android.content.Context
import android.net.Uri
import android.os.StatFs
import android.provider.DocumentsContract

/**
 * Best-effort free-space resolution for a SAF tree URI.
 * For external-storage provider URIs (the overwhelmingly common case —
 * primary volume / SD cards) the document ID maps to a real filesystem
 * path, where StatFs gives honest numbers. Other providers return null
 * and the UI shows "unknown" rather than inventing a value.
 */
object FreeSpace {

    data class Result(val freeBytes: Long?, val resolvedPath: String?)

    fun forTree(context: Context, treeUri: Uri): Result {
        return try {
            val docId = DocumentsContract.getTreeDocumentId(treeUri)
            val authority = treeUri.authority ?: return Result(null, null)
            if (authority != "com.android.externalstorage.documents") return Result(null, null)
            val volume = docId.substringBefore(':')
            val rest = docId.substringAfter(':', "")
            val basePath = when (volume) {
                "primary" -> "/storage/emulated/0"
                else -> "/storage/$volume"
            }
            val path = if (rest.isEmpty()) basePath else "$basePath/$rest"
            val stat = StatFs(path)
            Result(stat.availableBytes, path)
        } catch (e: Exception) {
            Result(null, null)
        }
    }

    fun format(bytes: Long?): String {
        if (bytes == null) return "unknown"
        val gb = bytes / (1024.0 * 1024 * 1024)
        return if (gb >= 10) String.format("%.0f GB", gb) else String.format("%.1f GB", gb)
    }

    fun isLow(bytes: Long?, thresholdGb: Int): Boolean {
        if (bytes == null) return false // unknown -> no warning (honest)
        return bytes < thresholdGb.toLong() * 1024 * 1024 * 1024
    }
}
