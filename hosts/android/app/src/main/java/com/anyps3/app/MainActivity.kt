// Any_Ps3 — Android activity: SAF game import, Compose UI host, emulation thread.
package com.anyps3.app

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.provider.DocumentsContract
import android.view.KeyEvent
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.*
import androidx.documentfile.provider.DocumentFile
import kotlinx.coroutines.*
import java.io.File

class MainActivity : ComponentActivity() {
    private val ui = AppUiState()
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
    ) { uri: Uri? -> uri?.let { scope.launch(Dispatchers.IO) { importIso(it) } } }

    private val pickFolder = registerForActivityResult(
        ActivityResultContracts.OpenDocumentTree()
    ) { uri: Uri? -> uri?.let { scope.launch(Dispatchers.IO) { importFolderTree(it) } } }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val configDir = File(filesDir, "anyps3").absolutePath
        File(configDir).mkdirs()
        NativeBridge.nativeInit(configDir)
        ui.firstRunAccepted = NativeBridge.nativeFirstRunAccepted()
        refreshLibrary()

        setContent {
            AnyPs3App(
                vm = ui,
                onAddGame = { showImportSheet() },
                onRun = { id -> runGame(id) },
                onStop = { NativeBridge.nativeStop() },
                onSettings = { id -> openSettings(id) },
                onRemove = { id ->
                    NativeBridge.nativeRemoveGame(id)
                    refreshLibrary()
                }
            )
        }
    }

    private fun showImportSheet() {
        // Both options use Storage Access Framework as required by modern Android.
        val items = arrayOf("ISO image (copied to app storage)", "Game folder (metadata preview)")
        val builder = android.app.AlertDialog.Builder(this)
        builder.setTitle("Import game")
            .setItems(items) { _, which ->
                if (which == 0) pickIso.launch(arrayOf("*/*")) else pickFolder.launch(null)
            }
            .show()
    }

    private fun importIso(uri: Uri) {
        ui.importBusy = true
        ui.importStatus = "Copying ISO into app storage..."
        try {
            val name = queryDisplayName(uri) ?: "game.iso"
            val dir = File(filesDir, "imports")
            dir.mkdirs()
            val out = File(dir, name)
            contentResolver.openInputStream(uri)!!.use { input ->
                out.outputStream().use { output -> input.copyTo(output) }
            }
            val res = NativeBridge.nativeImportGame(out.absolutePath)
            ui.importStatus = if (res.startsWith("OK")) "Imported: " + res.substring(3) else res
        } catch (e: Exception) {
            ui.importStatus = "Import failed: ${e.message}"
        } finally {
            ui.importBusy = false
            refreshLibrary()
        }
    }

    /**
     * Folder dumps via SAF: the tree is walked with DocumentFile; metadata (PARAM.SFO,
     * ICON0.PNG) and the executable are copied so the library entry and boot checks work.
     * Streaming a full multi-GB USRDIR straight from SAF into the runtime is tracked as
     * roadmap work (see PHASE_REPORTS.md); ISO import is the complete path today.
     */
    private fun importFolderTree(uri: Uri) {
        ui.importBusy = true
        ui.importStatus = "Scanning folder (SAF)..."
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
            ui.importStatus = if (res.startsWith("OK")) {
                "Imported (metadata+executable): " + res.substring(3)
            } else res
        } catch (e: Exception) {
            ui.importStatus = "Folder import failed: ${e.message}"
        } finally {
            ui.importBusy = false
            refreshLibrary()
        }
    }

    private fun queryDisplayName(uri: Uri): String? {
        var name: String? = null
        contentResolver.query(uri, null, null, null, null)?.use { c ->
            if (c.moveToFirst()) {
                val idx = c.getColumnIndex(DocumentsContract.Document.COLUMN_DISPLAY_NAME)
                if (idx >= 0) name = c.getString(idx)
            }
        }
        return name
    }

    private fun refreshLibrary() {
        val raw = NativeBridge.nativeListGames()
        ui.games = raw.split(";").filter { it.isNotBlank() }.map { entry ->
            val f = entry.split("|")
            AppUiState.GameRow(
                id = f.getOrNull(0) ?: "",
                title = f.getOrNull(1) ?: "",
                appVer = f.getOrNull(2) ?: "",
                iconPath = f.getOrNull(3) ?: ""
            )
        }
    }

    private fun runGame(id: String) {
        ui.runningTitleId = id
        scope.launch(Dispatchers.Default) {
            val ok = NativeBridge.nativeBoot(id, bootCallbacks)
            if (ok) {
                val code = NativeBridge.nativeRun()
                withContext(Dispatchers.Main) {
                    ui.runtimeLog += "\nguest exited with code $code\n"
                    ui.runningTitleId = null
                }
            } else {
                withContext(Dispatchers.Main) { ui.runningTitleId = null }
            }
        }
    }

    private fun openSettings(id: String) {
        ui.settingsFor = id
        // Per-game settings UI is rendered by AppUi/SettingsScreen when settingsFor != null.
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
