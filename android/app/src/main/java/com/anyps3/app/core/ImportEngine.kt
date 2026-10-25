package com.anyps3.app.core

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import android.os.ParcelFileDescriptor
import androidx.documentfile.provider.DocumentFile
import org.json.JSONObject
import java.io.File

/**
 * No-copy import pipeline.
 *
 * HARD INVARIANTS:
 *  - ISOs are NEVER copied into app storage. Only the SAF URI + metadata
 *    (title, ID, size, cover thumbnail path) are stored.
 *  - Access goes through openFileDescriptor + native random-access reads.
 *  - Persistable URI permissions are taken for every imported item, so
 *    access survives app restarts and reboots.
 *  - Only a small cover THUMBNAIL (≤ 320 px) is cached on our side.
 */
class ImportEngine(private val context: Context, private val db: GameDb) {

    val coverDir: File = File(context.filesDir, "covers").also { it.mkdirs() }
    private val tmpDir: File = File(context.cacheDir, "import").also { it.mkdirs() }

    data class ImportResult(val ok: Boolean, val message: String)

    fun takePersistable(uri: Uri, rw: Boolean) {
        val flags = android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION or
                (if (rw) android.content.Intent.FLAG_GRANT_WRITE_URI_PERMISSION else 0)
        try {
            context.contentResolver.takePersistableUriPermission(uri, flags)
        } catch (e: SecurityException) {
            // Some providers do not issue persistable grants for single docs;
            // the tree grant covers folder-based imports.
        }
    }

    /** Import one ISO via SAF. */
    fun importIso(uri: Uri): ImportResult {
        takePersistable(uri, rw = false)
        try {
            context.contentResolver.openFileDescriptor(uri, "r")?.use { pfd ->
                val meta = NativeBridge.inspectIsoFd(pfd.fd)
                if (!meta.optBoolean("ok", false)) {
                    return ImportResult(false, meta.optString("error", "inspect failed"))
                }
                val gameId = meta.optString("title_id", "")
                if (gameId.isEmpty()) return ImportResult(false, "no TITLE_ID in PARAM.SFO")
                val existing = db.getGame(gameId)
                val cover = extractAndCacheCoverFromIso(pfd.fd, gameId)
                db.upsertGame(
                    GameRow(
                        gameId = gameId,
                        title = meta.optString("title", gameId),
                        displayName = existing?.displayName ?: "",
                        sourceUri = uri.toString(),
                        sourceKind = "iso",
                        sizeBytes = pfd.statSize,
                        coverPath = cover,
                        importedAt = System.currentTimeMillis(),
                        status = existing?.status ?: "not_compiled",
                        missing = false,
                        favorite = existing?.favorite ?: false,
                        notes = existing?.notes ?: "",
                    )
                )
                db.log("info", "imported ISO $gameId \"${meta.optString("title", "")}\"")
                return ImportResult(true, gameId)
            } ?: return ImportResult(false, "cannot open file (missing or moved?)")
        } catch (e: Exception) {
            return ImportResult(false, e.message ?: "import failed")
        }
    }

    /**
     * Import a game FOLDER (a directory that contains PS3_GAME/PARAM.SFO).
     * The tree URI is persisted; the source is read in place.
     */
    fun importFolder(uri: Uri): ImportResult {
        takePersistable(uri, rw = false)
        val root = DocumentFile.fromTreeUri(context, uri) ?: return ImportResult(false, "bad tree uri")
        val gameDir = root.findFile("PS3_GAME")
            ?: return ImportResult(false, "no PS3_GAME folder inside selection")
        val sfo = gameDir.findFile("PARAM.SFO")
            ?: return ImportResult(false, "no PS3_GAME/PARAM.SFO — not a game folder?")

        try {
            context.contentResolver.openFileDescriptor(sfo.uri, "r")?.use { pfd ->
                val meta = NativeBridge.parseSfoFd(pfd.fd)
                if (!meta.optBoolean("ok", false)) {
                    return ImportResult(false, meta.optString("error", "SFO parse failed"))
                }
                val gameId = meta.optString("title_id", "")
                if (gameId.isEmpty()) return ImportResult(false, "no TITLE_ID in PARAM.SFO")
                val existing = db.getGame(gameId)
                val cover = extractAndCacheCoverFromFolder(gameDir, gameId)
                var size = 0L
                var count = 0
                countFiles(gameDir).let { size = it.first; count = it.second }
                db.upsertGame(
                    GameRow(
                        gameId = gameId,
                        title = meta.optString("title", gameId),
                        displayName = existing?.displayName ?: "",
                        sourceUri = uri.toString(),
                        sourceKind = "folder",
                        sizeBytes = size,
                        coverPath = cover,
                        importedAt = System.currentTimeMillis(),
                        status = existing?.status ?: "not_compiled",
                        missing = false,
                        favorite = existing?.favorite ?: false,
                        notes = existing?.notes ?: "",
                    )
                )
                db.log("info", "imported folder $gameId \"${meta.optString("title", "")}\" ($count files)")
                return ImportResult(true, gameId)
            } ?: return ImportResult(false, "cannot open PARAM.SFO (missing or moved?)")
        } catch (e: Exception) {
            return ImportResult(false, e.message ?: "import failed")
        }
    }

    /** Scan a whole folder tree for games: *.iso children + game sub-folders. */
    fun scanFolder(uri: Uri, onEach: (ImportResult) -> Unit): Int {
        val root = DocumentFile.fromTreeUri(context, uri) ?: return 0
        var imported = 0
        for (child in root.listFiles()) {
            val name = child.name ?: continue
            if (child.isFile && name.endsWith(".iso", true)) {
                val r = importIso(child.uri)
                if (r.ok) imported++
                onEach(r)
            } else if (child.isDirectory) {
                val hasGame = try {
                    child.findFile("PS3_GAME")?.findFile("PARAM.SFO") != null
                } catch (e: Exception) { false }
                if (hasGame) {
                    val r = importFolder(child.uri)
                    if (r.ok) imported++
                    onEach(r)
                }
            }
        }
        db.log("info", "folder scan: $imported game(s) found")
        return imported
    }

    /**
     * Re-check availability of all imported sources (pull-to-refresh).
     * Missing files are flagged — UI shows a clear "re-select" state.
     */
    fun refreshAvailability(): Int {
        var missingCount = 0
        for (g in db.listGames()) {
            val available = try {
                val uri = Uri.parse(g.sourceUri)
                if (g.sourceKind == "iso") {
                    context.contentResolver.openFileDescriptor(uri, "r")?.use { true } ?: false
                } else {
                    val tree = DocumentFile.fromTreeUri(context, uri)
                    tree != null && tree.findFile("PS3_GAME")?.findFile("PARAM.SFO") != null
                }
            } catch (e: Exception) { false }
            if (available == g.missing) { // state changed
                db.updateFields(g.gameId, "missing" to !available)
                if (!available) {
                    missingCount++
                    db.log("warn", "source missing for ${g.gameId}")
                }
            }
        }
        return missingCount
    }

    private fun countFiles(dir: DocumentFile): Pair<Long, Int> {
        var size = 0L
        var count = 0
        for (c in dir.listFiles()) {
            if (c.isFile) { size += c.length(); count++ }
            else if (c.isDirectory) {
                val sub = countFiles(c)
                size += sub.first; count += sub.second
            }
        }
        return Pair(size, count)
    }

    private fun extractAndCacheCoverFromIso(isoFd: Int, gameId: String): String? {
        // Native: stream PS3_GAME/ICON0.PNG out of the ISO into a temp file
        // (random access, chunked; the image is never fully in RAM), then
        // downscale to a small thumbnail and delete the temp file.
        val tmp = File(tmpDir, "icon_$gameId.png")
        try {
            ParcelFileDescriptor.open(tmp, ParcelFileDescriptor.MODE_WRITE_ONLY or
                ParcelFileDescriptor.MODE_TRUNCATE or ParcelFileDescriptor.MODE_CREATE).use { out ->
                val r = NativeBridge.copyIsoEntryToFd(isoFd, "ICON0.PNG", out.fd)
                if (!r.optBoolean("ok", false)) return null
            }
            return downscaleToThumbnail(tmp, gameId)
        } catch (e: Exception) {
            return null
        } finally {
            tmp.delete()
        }
    }

    private fun extractAndCacheCoverFromFolder(gameDir: DocumentFile, gameId: String): String? {
        val icon = gameDir.findFile("ICON0.PNG") ?: return null
        return try {
            context.contentResolver.openFileDescriptor(icon.uri, "r")?.use { pfd ->
                downscaleToThumbnail(pfd.fileDescriptor, null, gameId)
            }
        } catch (e: Exception) { null }
    }

    private fun downscaleToThumbnail(source: File, gameId: String): String? =
        downscaleToThumbnail(-1, source, gameId)

    private fun downscaleToThumbnail(fdOrFile: Any, unused: Any?, gameId: String): String? {
        try {
            val opts = BitmapFactory.Options().apply { inJustDecodeBounds = true }
            if (fdOrFile is Int && fdOrFile >= 0) BitmapFactory.decodeFileDescriptor(fdOrFile, null, opts)
            else if (fdOrFile is File) BitmapFactory.decodeFile(fdOrFile.absolutePath, opts)
            if (opts.outWidth <= 0) return null
            var sample = 1
            while (opts.outWidth / (sample * 2) >= 320) sample *= 2
            val realOpts = BitmapFactory.Options().apply { inSampleSize = sample }
            val bmp: Bitmap? = if (fdOrFile is Int && fdOrFile >= 0)
                BitmapFactory.decodeFileDescriptor(fdOrFile, null, realOpts)
            else if (fdOrFile is File) BitmapFactory.decodeFile(fdOrFile.absolutePath, realOpts)
            else null
            bmp ?: return null
            val out = File(coverDir, "$gameId.png")
            out.outputStream().use { fos ->
                bmp.compress(Bitmap.CompressFormat.PNG, 90, fos)
            }
            if (bmp.width > 320) { // final safety downscale
                val scaled = Bitmap.createScaledBitmap(bmp, 320, bmp.height * 320 / bmp.width, true)
                out.outputStream().use { fos -> scaled.compress(Bitmap.CompressFormat.PNG, 90, fos) }
            }
            bmp.recycle()
            return out.absolutePath
        } catch (e: Exception) {
            return null
        }
    }

    /** Delete a game's cache folder content inside the user's tree (manifest). */
    fun deleteCache(gameId: String, cacheTree: Uri?): Boolean {
        val tree = cacheTree ?: return false
        return try {
            val root = DocumentFile.fromTreeUri(context, tree) ?: return false
            val dir = root.findFile(gameId) ?: return true // nothing cached
            dir.listFiles().forEach { it.delete() }
            dir.delete()
            db.log("info", "cache deleted for $gameId")
            db.updateFields(gameId, "status" to "not_compiled")
            true
        } catch (e: Exception) { false }
    }
}
