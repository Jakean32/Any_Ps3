// Any_Ps3 — Android activity: SAF game import (NO ISO copy — fd random access),
// Compose UI host, emulation thread, diagnostics export + crash handling.
//
// v0.6.0 changes:
//  * UI state is app-scoped (survives rotation) — see AnyPs3App.
//  * Diagnostics export is CRASH-PROOF: every path catches Throwable (the
//    old `catch (Exception)` could not stop Error-class failures, and the
//    old share-sheet-only path crashed for a user on Android 13).
//  * PRIMARY export = MediaStore save into the public Downloads folder
//    (a real file, no share sheet involved). Share sheet stays secondary.
package com.anyps3.app

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.provider.DocumentsContract
import android.view.KeyEvent
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.documentfile.provider.DocumentFile
import kotlinx.coroutines.*
import java.io.File

class MainActivity : ComponentActivity() {
    private val app get() = application as AnyPs3App
    private val ui get() = app.ui
    private val scope = MainScope()
    private var bootCallbacks = object : NativeBridge.RuntimeCallbacks() {
        override fun onLog(line: String) {
            scope.launch(Dispatchers.Main) { ui.runtimeLog += line + "\n" }
        }
        override fun onProgress(percent: Int, note: String) {
            scope.launch(Dispatchers.Main) { ui.runtimeLog += "[$percent%] $note\n" }
        }
        override fun onState(state: Int) {}
    }

    // SAF pickers -----------------------------------------------------------------
    private val pickIso = registerForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri: Uri? -> uri?.let { scope.launch(Dispatchers.IO) { importIsoNoCopy(it) } } }

    private val pickFolder = registerForActivityResult(
        ActivityResultContracts.OpenDocumentTree()
    ) { uri: Uri? -> uri?.let { scope.launch(Dispatchers.IO) { importFolderTree(it) } } }

    // Re-select flow (persisted URI grant lost / file moved / rebooted device):
    private var reselectForId: String? = null
    private val pickReselect = registerForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri: Uri? ->
        val id = reselectForId
        if (uri != null && id != null) {
            scope.launch(Dispatchers.IO) { reselectSource(id, uri) }
        }
        reselectForId = null
    }

    // Per-game disc key picker (user's own .dkey/.key file)
    private var keyForId: String? = null
    private val pickKey = registerForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri: Uri? ->
        val id = keyForId
        if (uri != null && id != null) {
            scope.launch(Dispatchers.IO) { setDiscKey(id, uri) }
        }
        keyForId = null
    }

    // SAF picker for the user's own DECRYPTED executable (encrypted retail
    // EBOOTs cannot be decrypted by Any_Ps3 — no keys shipped by design).
    private var ebootForId: String? = null
    private val pickEboot = registerForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri: Uri? ->
        val id = ebootForId
        if (uri != null && id != null) {
            scope.launch(Dispatchers.IO) { setDecryptedEboot(id, uri) }
        }
        ebootForId = null
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val configDir = File(filesDir, "anyps3").absolutePath
        File(configDir).mkdirs()
        NativeBridge.nativeInit(configDir)
        ui.firstRunAccepted = NativeBridge.nativeFirstRunAccepted()
        ui.statusText = NativeBridge.nativeStatus()
        ui.appVersion = Diagnostics.appVersion(this)
        ui.runtimeLog = "Any_Ps3 ${ui.appVersion} — Android host\n"
        refreshLibrary()

        setContent {
            AnyPs3App(
                vm = ui,
                onRun = { id -> runGame(id) },
                onStop = { NativeBridge.nativeStop() },
                onSettings = { id -> ui.settingsFor = id },
                onPickKey = { id -> pickKeyFor(id) },
                onPickEboot = { id -> pickEbootFor(id) },
                onAttachKeyText = { id, text -> attachKeyText(id, text) },
                onReselect = { id -> reselectForId = id; pickReselect.launch(arrayOf("*/*")) },
                onRemove = { id ->
                    NativeBridge.nativeRemoveGame(id)
                    refreshLibrary()
                },
                onInstallDemo = { installDemo() },
                onImportIso = { pickIso.launch(arrayOf("*/*")) },
                onImportFolder = { pickFolder.launch(null) },
                onExportDiagnostics = { toDownloads -> exportDiagnostics(toDownloads) },
                onSaveText = { prefix, text -> saveTextToDownloads(prefix, text) },
                onShareText = { title, text -> shareText(title, text) },
                onCopyText = { text -> copyToClipboard(text) }
            )
        }

        // Previous session left a crash log -> show the in-app report screen.
        showCrashReportIfAny()
    }

    // ---- import -----------------------------------------------------------------

    /**
     * NO-COPY ISO import (TASK requirement): take a persistable read grant so
     * the URI survives reboots, open the document through SAF and hand the raw
     * ParcelFileDescriptor to the core, which reads it with random access.
     * The ISO is never copied and never fully loaded; only metadata
     * (title/id/icon + manifest) is stored. Encrypted images import with
     * state "Encrypted — disc key needed" and a per-game key can be attached
     * in the settings.
     */
    private fun importIsoNoCopy(uri: Uri) {
        ui.importBusy = true
        ui.banner("Reading ISO metadata in place (no copy)…")
        try {
            contentResolver.takePersistableUriPermission(
                uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
            val name = queryDisplayName(uri) ?: "game.iso"
            val res = contentResolver.openFileDescriptor(uri, "r")!!.use { pfd ->
                NativeBridge.nativeImportGameFd(pfd.fd, name, uri.toString())
            }
            ui.banner(describeImportResult(res), if (res.startsWith("OK")) 1 else 2)
        } catch (e: SecurityException) {
            ui.banner("Import failed: no read permission for the selected file", 2)
        } catch (t: Throwable) {
            ui.banner("Import failed: ${t.message}", 2)
        } finally {
            ui.importBusy = false
            refreshLibrary()
        }
    }

    private fun describeImportResult(res: String): String {
        return if (res.startsWith("OK")) {
            "Imported: " + res.substring(3)
        } else if (res.startsWith("ENCRYPTED")) {
            "Imported (ENCRYPTED — attach a disc key in this game's settings): " +
                res.substring("ENCRYPTED ".length)
        } else res
    }

    /**
     * Folder dumps via SAF: the tree is walked with DocumentFile; metadata
     * (PARAM.SFO, ICON0.PNG) and the executable are copied so the library
     * entry and boot checks work. ISO import is the fully no-copy path.
     */
    private fun importFolderTree(uri: Uri) {
        ui.importBusy = true
        ui.banner("Scanning folder (SAF)…")
        try {
            contentResolver.takePersistableUriPermission(
                uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
            val root = DocumentFile.fromTreeUri(this, uri)
                ?: throw IllegalStateException("cannot open tree")
            val ps3Game = root.findFile("PS3_GAME") ?: root
            val sfoDoc = ps3Game.findFile("PARAM.SFO")
                ?: throw IllegalStateException("PS3_GAME/PARAM.SFO not found — not a PS3 game folder")
            val target = File(filesDir, "imports/${uri.lastPathSegment ?: "folder_game"}")
            target.mkdirs()
            listOfNotNull(
                sfoDoc,
                ps3Game.findFile("ICON0.PNG"),
                ps3Game.findFile("EBOOT.BIN")
            ).forEach { doc ->
                val out = File(target, doc.name!!)
                if (!out.exists()) {
                    contentResolver.openInputStream(doc.uri)!!.use { input ->
                        out.outputStream().use { output -> input.copyTo(output) }
                    }
                }
            }
            val res = NativeBridge.nativeImportGame(target.absolutePath)
            ui.banner(
                if (res.startsWith("OK")) "Imported (metadata+executable copied): " + res.substring(3)
                else res,
                if (res.startsWith("OK")) 1 else 2)
        } catch (t: Throwable) {
            ui.banner("Folder import failed: ${t.message}", 2)
        } finally {
            ui.importBusy = false
            refreshLibrary()
        }
    }

    // Re-select: the persisted URI grant expired or the file moved — point the
    // existing library entry at the newly picked document.
    private fun reselectSource(id: String, uri: Uri) {
        ui.importBusy = true
        ui.banner("Re-linking game to the new file…")
        try {
            contentResolver.takePersistableUriPermission(
                uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
            val name = queryDisplayName(uri) ?: "game.iso"
            val res = contentResolver.openFileDescriptor(uri, "r")!!.use { pfd ->
                NativeBridge.nativeUpdateGameSource(id, pfd.fd, name, uri.toString())
            }
            ui.banner(describeImportResult(res), if (res.startsWith("OK")) 1 else 2)
        } catch (t: Throwable) {
            ui.banner("Re-select failed: ${t.message}", 2)
        } finally {
            ui.importBusy = false
            refreshLibrary()
        }
    }

    // Attach a user-owned disc key (file copied byte-for-byte is ≤ 64 bytes).
    // After the key is accepted, the ISO is re-read through the SAF grant so
    // the entry shows its real title/id/cover and becomes "Ready to compile".
    private fun setDiscKey(id: String, uri: Uri) {
        ui.importBusy = true
        ui.banner("Storing disc key…")
        try {
            val res = contentResolver.openFileDescriptor(uri, "r")!!.use { pfd ->
                NativeBridge.nativeSetGameKeyFd(id, pfd.fd)
            }
            if (res == "OK") {
                rereadWithKey(id, "Disc key stored — re-reading the ISO in place…")
            } else {
                ui.banner(res, 2)
                ui.importBusy = false
            }
        } catch (t: Throwable) {
            ui.banner("Could not read key: ${t.message}", 2)
            ui.importBusy = false
        }
    }

    // Typed 32-hex disc key (same flow as the file picker).
    private fun attachKeyText(id: String, text: String) {
        ui.importBusy = true
        val res = try {
            NativeBridge.nativeSetGameKeyText(id, text.trim())
        } catch (t: Throwable) {
            "ERR JNI: ${t.message}"
        }
        if (res == "OK") {
            rereadWithKey(id, "Disc key stored — re-reading the ISO in place…")
        } else {
            ui.banner(res, 2)
            ui.importBusy = false
        }
    }

    // Re-open the game's source URI and re-read metadata with the attached key.
    // Needs the file to still be reachable; a lost grant surfaces the reselect
    // state instead of a failure.
    private fun rereadWithKey(id: String, status: String) {
        ui.banner(status)
        val sourceUri = NativeBridge.nativeGetGameSourceUri(id)
        try {
            if (sourceUri.isBlank()) throw IllegalStateException("game has no SAF source")
            val name = queryDisplayName(Uri.parse(sourceUri)) ?: "game.iso"
            val res = contentResolver.openFileDescriptor(Uri.parse(sourceUri), "r")!!.use { pfd ->
                NativeBridge.nativeUpdateGameSource(id, pfd.fd, name, sourceUri)
            }
            ui.banner(
                if (res.startsWith("OK") || res.contains("|unlocked")) {
                    val idPart = res.removePrefix("OK ").substringBefore('|')
                    "Key accepted — $idPart unlocked and ready"
                } else res,
                if (res.startsWith("OK") || res.contains("|unlocked")) 1 else 2)
        } catch (e: SecurityException) {
            ui.banner("Key stored, but the ISO cannot be opened anymore — " +
                    "use Play or re-select to re-link the file, then the key applies.", 2)
        } catch (t: Throwable) {
            ui.banner("Key stored, but re-read failed: ${t.message}", 2)
        } finally {
            ui.importBusy = false
            refreshLibrary()
        }
    }

    private fun pickKeyFor(id: String) {
        keyForId = id
        pickKey.launch(arrayOf("*/*"))
    }

    private fun pickEbootFor(id: String) {
        ebootForId = id
        pickEboot.launch(arrayOf("*/*"))
    }

    // Store the picked decrypted executable (app-private copy) and validate
    // it through the same loader Play will use.
    private fun setDecryptedEboot(id: String, uri: Uri) {
        ui.importBusy = true
        ui.banner("Storing decrypted executable…")
        try {
            val res = contentResolver.openFileDescriptor(uri, "r")!!.use { pfd ->
                NativeBridge.nativeSetDecryptedEbootFd(id, pfd.fd)
            }
            ui.banner(
                if (res == "OK") "Decrypted executable stored — Play boots it instead of the encrypted EBOOT.BIN"
                else res,
                if (res == "OK") 1 else 2)
        } catch (t: Throwable) {
            ui.banner("Could not read the executable: ${t.message}", 2)
        } finally {
            ui.importBusy = false
            refreshLibrary()
        }
    }

    private fun queryDisplayName(uri: Uri): String? {
        var name: String? = null
        try {
            contentResolver.query(uri, null, null, null, null)?.use { c ->
                if (c.moveToFirst()) {
                    val idx = c.getColumnIndex(DocumentsContract.Document.COLUMN_DISPLAY_NAME)
                    if (idx >= 0) name = c.getString(idx)
                }
            }
        } catch (t: Throwable) {
            return null
        }
        return name
    }

    private fun refreshLibrary() {
        try {
            val raw = NativeBridge.nativeListGames()
            ui.games = raw.split(";").filter { it.isNotBlank() }.map { entry ->
                val f = entry.split("|")
                AppUiState.GameRow(
                    id = f.getOrNull(0) ?: "",
                    title = f.getOrNull(1) ?: "",
                    appVer = f.getOrNull(2) ?: "",
                    iconPath = f.getOrNull(3) ?: "",
                    encState = f.getOrNull(4)?.toIntOrNull() ?: 0,
                    hasKey = f.getOrNull(5) == "1"
                )
            }
        } catch (t: Throwable) {
            // Never crash the UI thread on a library refresh.
        }
    }

    // Built-in demo: copy the bundled sample (assets/hello.elf, GNU-assembled
    // PPC64 test code, no Sony content) into app storage and install it as a
    // regular library entry through the core import pipeline.
    private fun installDemo() {
        try {
            val dir = File(filesDir, "samples")
            dir.mkdirs()
            val out = File(dir, "hello.elf")
            if (!out.exists() || out.length() == 0L) {
                assets.open("hello.elf").use { input ->
                    out.outputStream().use { output -> input.copyTo(output) }
                }
            }
            val res = NativeBridge.nativeInstallDemo(out.absolutePath)
            ui.banner(
                if (res.startsWith("OK")) "Demo installed — press Play on it to run the real boot pipeline"
                else res,
                if (res.startsWith("OK")) 1 else 2)
        } catch (t: Throwable) {
            ui.banner("Demo install failed: ${t.message}", 2)
        }
        refreshLibrary()
    }

    private fun runGame(id: String) {
        ui.runningTitleId = id
        ui.runtimeLog += "\n--- boot $id ---\n"
        scope.launch(Dispatchers.Default) {
            val sourceUri = NativeBridge.nativeGetGameSourceUri(id)
            if (sourceUri.isNotBlank()) {
                // SAF-backed ISO: open the persisted URI and boot through the fd
                // (no copy). A SecurityException/FileNotFoundException means the
                // grant expired or the file moved — surface the reselect state.
                try {
                    contentResolver.openFileDescriptor(Uri.parse(sourceUri), "r")!!.use { pfd ->
                        val ok = NativeBridge.nativeBootFd(id, pfd.fd, bootCallbacks)
                        if (ok) finishRun(NativeBridge.nativeRun())
                        else withContext(Dispatchers.Main) { ui.runningTitleId = null }
                    }
                } catch (t: Throwable) {
                    withContext(Dispatchers.Main) {
                        ui.runtimeLog += "\nFile missing — please reselect the game " +
                                "file (its access grant expired or the file moved).\n"
                        ui.missingFileFor = id
                        ui.runningTitleId = null
                    }
                }
            } else {
                try {
                    val ok = NativeBridge.nativeBoot(id, bootCallbacks)
                    if (ok) finishRun(NativeBridge.nativeRun())
                    else withContext(Dispatchers.Main) { ui.runningTitleId = null }
                } catch (t: Throwable) {
                    withContext(Dispatchers.Main) {
                        ui.runtimeLog += "\nboot failed: ${t.message}\n"
                        ui.runningTitleId = null
                    }
                }
            }
        }
    }

    private suspend fun finishRun(code: Int) {
        withContext(Dispatchers.Main) {
            ui.runtimeLog += "\nguest exited with code $code\n"
            ui.runningTitleId = null
        }
    }

    // ---- diagnostics export (v0.6.0: crash-proof, Downloads-first) --------------

    private fun gamesBlockForReport(): String {
        val sb = StringBuilder()
        try {
            val raw = NativeBridge.nativeListGames()
            if (raw.isBlank()) {
                sb.append("library is empty\n")
            } else {
                for (entry in raw.split(";").filter { it.isNotBlank() }) {
                    val f = entry.split("|")
                    val id = f.getOrNull(0) ?: "?"
                    val title = f.getOrNull(1) ?: "?"
                    val enc = when (f.getOrNull(4)?.toIntOrNull()) {
                        1 -> if (f.getOrNull(5) == "1") "Encrypted — disc key stored"
                             else "Encrypted — disc key needed"
                        2 -> "Encrypted (decryption active) — ready"
                        else -> "Ready to compile"
                    }
                    sb.append("- ").append(title).append(" [").append(id).append("] — ")
                        .append(enc).append('\n')
                }
            }
        } catch (t: Throwable) {
            sb.append("games: unavailable (").append(t.javaClass.name).append(")\n")
        }
        return sb.toString()
    }

    /**
     * Build the diagnostics report and hand it to the user.
     * @param toDownloads true = MediaStore save into Download/Any_Ps3 (primary,
     *        no share sheet); false = share sheet (secondary path).
     * Every failure is caught (Throwable) and reported in the banner — this
     * code path can no longer crash the app.
     */
    private fun exportDiagnostics(toDownloads: Boolean) {
        scope.launch(Dispatchers.Main) {
            try {
                val report = withContext(Dispatchers.IO) {
                    Diagnostics.buildReportText(
                        this@MainActivity, ui.appVersion, ui.statusText,
                        gamesBlockForReport(), ui.runtimeLog)
                }
                if (toDownloads) {
                    val name = "Any_Ps3-log-${timestamp()}.txt"
                    val where = withContext(Dispatchers.IO) {
                        Diagnostics.saveToDownloads(this@MainActivity, name, report)
                    }
                    ui.banner("Report saved: $where", 1)
                    Toast.makeText(this@MainActivity, "Saved: $where", Toast.LENGTH_LONG).show()
                } else {
                    val file = withContext(Dispatchers.IO) {
                        Diagnostics.writeCacheFile(
                            this@MainActivity, "anyps3-diagnostics-${timestamp()}.txt", report)
                    }
                    Diagnostics.shareFile(this@MainActivity, file, "Any_Ps3 diagnostics report")
                    ui.banner("Share sheet opened — pick an app to send the report", 0)
                }
            } catch (t: Throwable) {
                val msg = "Export failed: ${t.javaClass.simpleName}: ${t.message}"
                ui.banner(msg, 2)
                Toast.makeText(this@MainActivity, msg, Toast.LENGTH_LONG).show()
            }
        }
    }

    /** Generic save (used by the crash-report screen). Crash-proof. */
    private fun saveTextToDownloads(prefix: String, text: String) {
        scope.launch(Dispatchers.Main) {
            try {
                val name = "$prefix-${timestamp()}.txt"
                val where = withContext(Dispatchers.IO) {
                    Diagnostics.saveToDownloads(this@MainActivity, name, text)
                }
                ui.banner("Saved: $where", 1)
                Toast.makeText(this@MainActivity, "Saved: $where", Toast.LENGTH_LONG).show()
            } catch (t: Throwable) {
                val msg = "Save failed: ${t.javaClass.simpleName}: ${t.message}"
                ui.banner(msg, 2)
                Toast.makeText(this@MainActivity, msg, Toast.LENGTH_LONG).show()
            }
        }
    }

    /** Generic share (used by the crash-report screen). Crash-proof. */
    private fun shareText(title: String, text: String) {
        scope.launch(Dispatchers.Main) {
            try {
                val file = withContext(Dispatchers.IO) {
                    Diagnostics.writeCacheFile(
                        this@MainActivity, "anyps3-share-${timestamp()}.txt", text)
                }
                Diagnostics.shareFile(this@MainActivity, file, title)
            } catch (t: Throwable) {
                val msg = "Share failed: ${t.javaClass.simpleName}: ${t.message}"
                ui.banner(msg, 2)
                Toast.makeText(this@MainActivity, msg, Toast.LENGTH_LONG).show()
            }
        }
    }

    private fun copyToClipboard(text: String) {
        try {
            val cm = getSystemService(CLIPBOARD_SERVICE) as android.content.ClipboardManager
            cm.setPrimaryClip(
                android.content.ClipData.newPlainText("Any_Ps3 log", text))
            ui.banner("Copied to clipboard", 1)
        } catch (t: Throwable) {
            ui.banner("Copy failed: ${t.message}", 2)
        }
    }

    private fun timestamp(): String = try {
        java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.US)
            .format(java.util.Date())
    } catch (t: Throwable) {
        java.lang.System.currentTimeMillis().toString()
    }

    /** Show the previous session's crash log (written by AnyPs3App's
     *  uncaught-exception handler) as an in-app report screen. */
    private fun showCrashReportIfAny() {
        try {
            val crashFile = File(File(filesDir, "anyps3"), "crash_last.txt")
            if (!crashFile.exists()) return
            val text = crashFile.readText()
            if (text.isBlank()) return
            ui.crashReport = text
            // Deleting happens on "Keep for later" close: we keep the file so
            // the report survives app restarts until the user is done with it.
            // Deleting is done from the report screen's dismiss action.
            ui.crashReportFile = crashFile
        } catch (t: Throwable) {
            // Never crash at startup because of a broken crash log.
        }
    }

    // Physical gamepad buttons are forwarded to the input module (Phase 6).
    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        PadInput.onAndroidKey(keyCode, event)
        return super.onKeyDown(keyCode, event)
    }

    override fun onDestroy() {
        NativeBridge.nativeStop()
        scope.cancel()
        super.onDestroy()
    }
}
