// Any_Ps3 — Android UI (Jetpack Compose), v0.6.0 redesign.
//
// Design goals (user feedback, 2026-10-25):
//   * READABILITY FIRST: explicit high-contrast dark palette (no black on
//     black — every text has an explicit, checked foreground color), font
//     sizes never below 12sp, generous line height in the log.
//   * Clean & simple: one primary action per screen (Von-Restorff effect),
//     big touch targets (Fitts's law), few visible choices (Hick's law),
//     16dp spacing grid, rounded 16dp cards, quiet grays + two accents.
//   * Immediate feedback for every action (status banner), no dead ends.
// Screens: FirstRun (legal consent) -> Library -> per-game Settings /
// AppSettings / full-screen LogViewer, plus overlays (import sheet,
// remove confirm, missing file, crash report).
package com.anyps3.app

import android.graphics.BitmapFactory
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.KeyboardArrowUp
import androidx.compose.material.icons.filled.List
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Share
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.withContext
import java.io.File

// ----------------------------------------------------------------------------
// State (lives in the Application so it survives rotation)
// ----------------------------------------------------------------------------

/** Structured user-feedback banner. */
data class Banner(val text: String, val kind: Int) { // 0 info, 1 ok, 2 error
    val isError get() = kind == 2
    val isOk get() = kind == 1
}

/** State holder for the whole app UI (app-scoped, rotation-safe). */
class AppUiState {
    var firstRunAccepted by mutableStateOf(false)
    var games by mutableStateOf(listOf<GameRow>())
    var search by mutableStateOf("")
    var banner by mutableStateOf<Banner?>(null)
    var importBusy by mutableStateOf(false)
    var runtimeLog by mutableStateOf("")
    var runningTitleId by mutableStateOf<String?>(null)
    var settingsFor by mutableStateOf<String?>(null)
    var showAppSettings by mutableStateOf(false)
    var showLogViewer by mutableStateOf(false)
    var showImportSheet by mutableStateOf(false)
    var removeFor by mutableStateOf<GameRow?>(null)
    var missingFileFor by mutableStateOf<String?>(null)
    var crashReport by mutableStateOf<String?>(null)
    var crashReportFile by mutableStateOf<java.io.File?>(null)
    var statusText by mutableStateOf("")      // engine status (native)
    var appVersion by mutableStateOf("")       // from package manager

    fun banner(text: String, kind: Int = 0) { this.banner = Banner(text, kind) }
    fun dismissBanner() { this.banner = null }

    // icon path -> decoded bitmap (ICON0.PNG of imported games)
    private val icons = mutableMapOf<String, androidx.compose.ui.graphics.ImageBitmap>()
    fun iconFor(path: String): androidx.compose.ui.graphics.ImageBitmap? {
        if (path.isBlank()) return null
        icons[path]?.let { return it }
        val f = File(path)
        if (!f.exists()) return null
        return try {
            val bmp = BitmapFactory.decodeFile(f.absolutePath) ?: return null
            val ib = bmp.asImageBitmap()
            icons[path] = ib
            ib
        } catch (e: Exception) {
            null
        }
    }

    data class GameRow(
        val id: String, val title: String, val appVer: String, val iconPath: String,
        val encState: Int = 0, val hasKey: Boolean = false)
}

// ----------------------------------------------------------------------------
// Theme: explicit high-contrast dark palette (nothing below ~7:1 for body
// text; secondary text >= 4.5:1 — checked against background #0B0E13)
// ----------------------------------------------------------------------------

private val CBackground = Color(0xFF0B0E13)
private val CSurface = Color(0xFF121721)
private val CSurfaceHigh = Color(0xFF1B2230)
private val COnBackground = Color(0xFFEFF3FA)
private val COnSurfaceVariant = Color(0xFFBAC6DA)

fun anyps3DarkScheme(): ColorScheme = darkColorScheme(
    background = CBackground,
    onBackground = COnBackground,
    surface = CSurface,
    onSurface = COnBackground,
    surfaceVariant = CSurfaceHigh,
    onSurfaceVariant = COnSurfaceVariant,
    surfaceContainer = CSurfaceHigh,
    surfaceContainerHigh = Color(0xFF202939),
    surfaceContainerHighest = Color(0xFF26303F),
    surfaceContainerLow = CSurface,
    surfaceContainerLowest = Color(0xFF0D1117),
    surfaceTint = Color(0xFF8FBAFF),
    primary = Color(0xFF9CC0FF),          // soft steel blue
    onPrimary = Color(0xFF0B1B33),
    primaryContainer = Color(0xFF22456E),
    onPrimaryContainer = Color(0xFFD6E6FF),
    secondary = Color(0xFFE3B877),         // brand gold
    onSecondary = Color(0xFF241A06),
    secondaryContainer = Color(0xFF3A2D14),
    onSecondaryContainer = Color(0xFFF4DDB3),
    tertiary = Color(0xFF85E0A6),          // success green
    onTertiary = Color(0xFF07271A),
    tertiaryContainer = Color(0xFF10391F),
    onTertiaryContainer = Color(0xFFC9F4D9),
    error = Color(0xFFFFB4AB),
    onError = Color(0xFF2B0705),
    errorContainer = Color(0xFF5C1A16),
    onErrorContainer = Color(0xFFFFDAD5),
    outline = Color(0xFF54677F),
    outlineVariant = Color(0xFF2C3849),
    inverseSurface = Color(0xFFE6EAF2),
    inverseOnSurface = Color(0xFF13161C),
    inversePrimary = Color(0xFF2A5E9E),
)

// Status colors (explicit pairs, all >= 4.5:1 on their chip background)
private val ChipReadyBg = Color(0xFF10291B)
private val ChipReadyFg = Color(0xFF8CE2AE)
private val ChipEncBg = Color(0xFF33270F)
private val ChipEncFg = Color(0xFFFFCB70)
private val ChipActiveBg = Color(0xFF0F2E38)
private val ChipActiveFg = Color(0xFF8BD8F0)

// ----------------------------------------------------------------------------
// Root
// ----------------------------------------------------------------------------

@Composable
fun AnyPs3App(
    vm: AppUiState,
    onRun: (String) -> Unit,
    onStop: () -> Unit,
    onSettings: (String) -> Unit,
    onPickKey: (String) -> Unit,
    onAttachKeyText: (String, String) -> Unit,
    onPickEboot: (String) -> Unit,
    onReselect: (String) -> Unit,
    onRemove: (String) -> Unit,
    onInstallDemo: () -> Unit,
    onImportIso: () -> Unit,
    onImportFolder: () -> Unit,
    onExportDiagnostics: (toDownloads: Boolean) -> Unit,
    onSaveText: (namePrefix: String, text: String) -> Unit,
    onShareText: (title: String, text: String) -> Unit,
    onCopyText: (text: String) -> Unit,
) {
    MaterialTheme(colorScheme = anyps3DarkScheme()) {
        Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
            when {
                !vm.firstRunAccepted -> FirstRunScreen(vm)

                vm.showLogViewer -> LogViewerScreen(
                    logText = vm.runtimeLog,
                    onBack = { vm.showLogViewer = false },
                    onSave = { onExportDiagnostics(true) },
                    onShare = { onExportDiagnostics(false) },
                    onCopy = { onCopyText(vm.runtimeLog) }
                )

                vm.showAppSettings -> AppSettingsScreen(vm, onExportDiagnostics) {
                    vm.showAppSettings = false
                }

                vm.settingsFor != null -> SettingsScreen(
                    id = vm.settingsFor!!, onPickKey = onPickKey,
                    onAttachKeyText = onAttachKeyText, onPickEboot = onPickEboot,
                    onClose = { vm.settingsFor = null }
                )

                vm.crashReport != null -> CrashReportScreen(
                    vm.crashReport!!,
                    onDismiss = { vm.crashReport = null },
                    onDelete = {
                        try { vm.crashReportFile?.delete() } catch (t: Throwable) { }
                        vm.crashReport = null
                    },
                    onCopy = { onCopyText(vm.crashReport ?: "") },
                    onSave = { onSaveText("Any_Ps3-crash", vm.crashReport ?: "") },
                    onShare = { onShareText("Any_Ps3 crash log", vm.crashReport ?: "") }
                )

                else -> {
                    LibraryScreen(vm, onRun, onStop, onSettings, onInstallDemo)
                    Overlays(vm, onReselect, onRemove, onImportIso, onImportFolder)
                }
            }
        }
    }
}

/** Dialogs shown on top of the library. */
@Composable
private fun Overlays(vm: AppUiState, onReselect: (String) -> Unit,
                     onRemove: (String) -> Unit, onImportIso: () -> Unit,
                     onImportFolder: () -> Unit) {
    // Import sheet
    if (vm.showImportSheet) {
        AlertDialog(
            onDismissRequest = { vm.showImportSheet = false },
            title = { Text("Import game") },
            text = {
                Column {
                    Text(
                        "Both options use the Android file picker — no storage permission needed.",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                    Spacer(Modifier.height(12.dp))
                    ImportOption(
                        title = "ISO image",
                        subtitle = "Read in place — nothing is copied, the file must stay where it is") {
                        vm.showImportSheet = false; onImportIso()
                    }
                    Spacer(Modifier.height(8.dp))
                    ImportOption(
                        title = "Game folder",
                        subtitle = "Copies only PARAM.SFO / ICON0 / EBOOT into app storage") {
                        vm.showImportSheet = false; onImportFolder()
                    }
                }
            },
            confirmButton = {},
            dismissButton = {
                TextButton(onClick = { vm.showImportSheet = false }) { Text("Cancel") }
            }
        )
    }

    // Remove confirmation
    vm.removeFor?.let { g ->
        AlertDialog(
            onDismissRequest = { vm.removeFor = null },
            title = { Text("Remove \"${g.title}\"?") },
            text = {
                Text("The library entry, its settings and its stored disc key are deleted. " +
                        "The game file itself is not touched.")
            },
            confirmButton = {
                TextButton(onClick = {
                    vm.removeFor = null
                    onRemove(g.id)
                }) { Text("Remove", color = MaterialTheme.colorScheme.error, fontWeight = FontWeight.SemiBold) }
            },
            dismissButton = {
                TextButton(onClick = { vm.removeFor = null }) { Text("Keep") }
            }
        )
    }

    // Missing source file (expired SAF grant / moved file)
    vm.missingFileFor?.let { id ->
        AlertDialog(
            onDismissRequest = { vm.missingFileFor = null },
            title = { Text("File missing") },
            text = {
                Text("This game's file can no longer be opened (its access grant expired " +
                        "or the file moved). Reselect the file to re-link the game.")
            },
            confirmButton = {
                TextButton(onClick = {
                    vm.missingFileFor = null
                    onReselect(id)
                }) { Text("Reselect file") }
            },
            dismissButton = {
                TextButton(onClick = { vm.missingFileFor = null }) { Text("Not now") }
            }
        )
    }
}

@Composable
private fun ImportOption(title: String, subtitle: String, onClick: () -> Unit) {
    Surface(
        shape = RoundedCornerShape(12.dp),
        color = MaterialTheme.colorScheme.surfaceContainerHigh,
        modifier = Modifier.fillMaxWidth().clickable(onClick = onClick)
    ) {
        Column(Modifier.padding(14.dp)) {
            Text(title, style = MaterialTheme.typography.titleSmall)
            Spacer(Modifier.height(2.dp))
            Text(subtitle, style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

// ----------------------------------------------------------------------------
// First run (legal consent)
// ----------------------------------------------------------------------------

@Composable
fun FirstRunScreen(vm: AppUiState) {
    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(24.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Spacer(Modifier.height(24.dp))
        Image(
            painter = painterResource(R.drawable.brand_logo),
            contentDescription = "Any_Ps3",
            modifier = Modifier.size(104.dp).clip(RoundedCornerShape(24.dp))
        )
        Spacer(Modifier.height(20.dp))
        Text("Welcome to Any_Ps3", style = MaterialTheme.typography.headlineMedium)
        Spacer(Modifier.height(8.dp))
        Text(
            "Any_Ps3 runs PS3 software by translating it ahead of time. " +
                    "It contains no Sony firmware, no keys and no game content.",
            style = MaterialTheme.typography.bodyLarge,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            lineHeight = 23.sp,
            modifier = Modifier.padding(horizontal = 4.dp)
        )
        Spacer(Modifier.height(20.dp))
        listOf(
            "I own the games I import and my dumps are legal.",
            "I will not import content I do not own.",
            "Encrypted retail executables cannot be decrypted (no keys are shipped); " +
                    "homebrew and debug executables are supported.",
            "\'Play\' shows the real boot log — full game execution is still in development."
        ).forEach { text ->
            Row(Modifier.fillMaxWidth().padding(vertical = 6.dp),
                verticalAlignment = Alignment.Top) {
                Icon(Icons.Filled.Check, contentDescription = null,
                    tint = MaterialTheme.colorScheme.tertiary,
                    modifier = Modifier.size(20.dp).padding(top = 2.dp))
                Spacer(Modifier.width(12.dp))
                Text(text, style = MaterialTheme.typography.bodyLarge, lineHeight = 23.sp)
            }
        }
        Spacer(Modifier.height(28.dp))
        Button(
            onClick = {
                NativeBridge.nativeAcceptFirstRun()
                vm.firstRunAccepted = NativeBridge.nativeFirstRunAccepted()
            },
            modifier = Modifier.fillMaxWidth().height(54.dp),
            shape = RoundedCornerShape(14.dp)
        ) {
            Text("I agree — continue", style = MaterialTheme.typography.titleMedium)
        }
        Spacer(Modifier.height(24.dp))
    }
}

// ----------------------------------------------------------------------------
// Library
// ----------------------------------------------------------------------------

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun LibraryScreen(vm: AppUiState, onRun: (String) -> Unit,
                  onStop: () -> Unit, onSettings: (String) -> Unit,
                  onInstallDemo: () -> Unit) {
    val filtered = remember(vm.games, vm.search) {
        val q = vm.search.trim().lowercase()
        if (q.isEmpty()) vm.games
        else vm.games.filter {
            it.title.lowercase().contains(q) || it.id.lowercase().contains(q)
        }
    }
    val count = vm.games.size

    Scaffold(
        containerColor = MaterialTheme.colorScheme.background,
        topBar = {
            TopAppBar(
                title = {
                    Column {
                        Text("Any_Ps3", style = MaterialTheme.typography.titleLarge)
                        Text(
                            if (count == 1) "1 game" else "$count games",
                            style = MaterialTheme.typography.labelMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                },
                actions = {
                    IconButton(onClick = { vm.showAppSettings = true }) {
                        Icon(Icons.Filled.Settings, contentDescription = "App settings")
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.background,
                    titleContentColor = MaterialTheme.colorScheme.onBackground,
                    actionIconContentColor = MaterialTheme.colorScheme.onBackground
                )
            )
        },
        floatingActionButton = {
            ExtendedFloatingActionButton(
                onClick = { vm.showImportSheet = true },
                icon = { Icon(Icons.Filled.Add, contentDescription = null) },
                text = { Text("Import game") },
                containerColor = MaterialTheme.colorScheme.primaryContainer,
                contentColor = MaterialTheme.colorScheme.onPrimaryContainer
            )
        }
    ) { pad ->
        Column(Modifier.padding(pad).fillMaxSize().padding(horizontal = 16.dp)) {
            // Search
            OutlinedTextField(
                value = vm.search,
                onValueChange = { vm.search = it },
                modifier = Modifier.fillMaxWidth(),
                placeholder = { Text("Search games") },
                leadingIcon = { Icon(Icons.Filled.Search, contentDescription = null) },
                trailingIcon = {
                    if (vm.search.isNotEmpty()) {
                        IconButton(onClick = { vm.search = "" }) {
                            Icon(Icons.Filled.Close, contentDescription = "Clear search")
                        }
                    }
                },
                singleLine = true,
                shape = RoundedCornerShape(14.dp)
            )

            // Feedback banner
            vm.banner?.let { b ->
                Spacer(Modifier.height(8.dp))
                Surface(
                    shape = RoundedCornerShape(12.dp),
                    color = when {
                        b.isError -> MaterialTheme.colorScheme.errorContainer
                        b.isOk -> MaterialTheme.colorScheme.tertiaryContainer
                        else -> MaterialTheme.colorScheme.surfaceContainerHigh
                    }
                ) {
                    Row(Modifier.fillMaxWidth().padding(start = 14.dp), verticalAlignment = Alignment.CenterVertically) {
                        Icon(
                            if (b.isError) Icons.Filled.Warning else Icons.Filled.Info,
                            contentDescription = null,
                            tint = when {
                                b.isError -> MaterialTheme.colorScheme.onErrorContainer
                                b.isOk -> MaterialTheme.colorScheme.onTertiaryContainer
                                else -> MaterialTheme.colorScheme.onSurfaceVariant
                            },
                            modifier = Modifier.size(18.dp)
                        )
                        Spacer(Modifier.width(10.dp))
                        Text(
                            b.text,
                            modifier = Modifier.weight(1f).padding(vertical = 10.dp),
                            style = MaterialTheme.typography.bodyMedium,
                            color = when {
                                b.isError -> MaterialTheme.colorScheme.onErrorContainer
                                b.isOk -> MaterialTheme.colorScheme.onTertiaryContainer
                                else -> MaterialTheme.colorScheme.onSurface
                            }
                        )
                        IconButton(onClick = { vm.dismissBanner() }) {
                            Icon(Icons.Filled.Close, contentDescription = "Dismiss",
                                tint = MaterialTheme.colorScheme.onSurfaceVariant,
                                modifier = Modifier.size(16.dp))
                        }
                    }
                }
            }

            if (vm.importBusy) {
                Spacer(Modifier.height(8.dp))
                LinearProgressIndicator(Modifier.fillMaxWidth())
            }

            Spacer(Modifier.height(8.dp))

            // Games list / empty state
            LazyColumn(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                if (count == 0) {
                    item { EmptyState(onImport = { vm.showImportSheet = true }, onInstallDemo = onInstallDemo) }
                } else if (filtered.isEmpty()) {
                    item {
                        Text(
                            "No game matches \"${vm.search}\".",
                            style = MaterialTheme.typography.bodyLarge,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.padding(vertical = 24.dp)
                        )
                    }
                } else {
                    items(filtered, key = { it.id }) { g ->
                        GameCard(g, vm, onRun, onStop, onSettings)
                        Spacer(Modifier.height(0.dp))
                    }
                }
                item { Spacer(Modifier.height(72.dp)) } // room for the FAB
            }

            // Log peek bar -> full log viewer
            Surface(
                shape = RoundedCornerShape(14.dp),
                color = MaterialTheme.colorScheme.surface,
                border = androidx.compose.foundation.BorderStroke(1.dp, MaterialTheme.colorScheme.outlineVariant),
                modifier = Modifier.fillMaxWidth().padding(bottom = 10.dp).clickable { vm.showLogViewer = true }
            ) {
                Row(Modifier.padding(horizontal = 14.dp, vertical = 10.dp),
                    verticalAlignment = Alignment.CenterVertically) {
                    Icon(Icons.Filled.List, contentDescription = null,
                        tint = MaterialTheme.colorScheme.secondary)
                    Spacer(Modifier.width(12.dp))
                    Column(Modifier.weight(1f)) {
                        Text("Log & boot output", style = MaterialTheme.typography.labelLarge)
                        val lastLines = vm.runtimeLog.split('\n').filter { it.isNotBlank() }
                        Text(
                            lastLines.lastOrNull() ?: "nothing logged yet — tap to open the log viewer",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            maxLines = 1, overflow = TextOverflow.Ellipsis
                        )
                    }
                    Icon(Icons.Filled.KeyboardArrowUp, contentDescription = "Open log viewer",
                        tint = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
        }
    }
}

@Composable
private fun EmptyState(onImport: () -> Unit, onInstallDemo: () -> Unit) {
    Column(
        Modifier.fillMaxWidth().padding(top = 32.dp, bottom = 16.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Image(
            painter = painterResource(R.drawable.brand_logo),
            contentDescription = null,
            modifier = Modifier.size(128.dp).clip(RoundedCornerShape(28.dp))
        )
        Spacer(Modifier.height(20.dp))
        Text("Your library is empty", style = MaterialTheme.typography.titleLarge)
        Spacer(Modifier.height(8.dp))
        Text(
            "Import a PS3 ISO image or a folder dump of a game you own.\n" +
                    "Everything stays on your device — nothing is uploaded.",
            style = MaterialTheme.typography.bodyLarge,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            lineHeight = 23.sp,
            textAlign = androidx.compose.ui.text.style.TextAlign.Center
        )
        Spacer(Modifier.height(20.dp))
        Button(
            onClick = onImport,
            modifier = Modifier.fillMaxWidth().height(52.dp),
            shape = RoundedCornerShape(14.dp)
        ) { Text("Import game", style = MaterialTheme.typography.titleMedium) }
        Spacer(Modifier.height(10.dp))
        OutlinedButton(
            onClick = onInstallDemo,
            modifier = Modifier.fillMaxWidth().height(52.dp),
            shape = RoundedCornerShape(14.dp)
        ) {
            Text("Install built-in demo", style = MaterialTheme.typography.titleMedium)
        }
        Spacer(Modifier.height(6.dp))
        Text(
            "The demo is a tiny homebrew ELF (no Sony content) that exercises the real import + boot pipeline.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = androidx.compose.ui.text.style.TextAlign.Center,
            lineHeight = 18.sp,
            modifier = Modifier.padding(horizontal = 12.dp)
        )
    }
}

// ----------------------------------------------------------------------------
// Game card
// ----------------------------------------------------------------------------

@Composable
fun GameCard(g: AppUiState.GameRow, vm: AppUiState, onRun: (String) -> Unit,
             onStop: () -> Unit, onSettings: (String) -> Unit) {
    ElevatedCard(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(16.dp),
        colors = CardDefaults.elevatedCardColors(
            containerColor = MaterialTheme.colorScheme.surfaceContainer)
    ) {
        Row(Modifier.padding(12.dp), verticalAlignment = Alignment.CenterVertically) {
            // Cover icon
            val bmp = vm.iconFor(g.iconPath)
            if (bmp != null) {
                Image(
                    bitmap = bmp, contentDescription = g.title,
                    modifier = Modifier.size(64.dp).clip(RoundedCornerShape(10.dp)),
                    contentScale = ContentScale.Crop
                )
            } else {
                Box(
                    modifier = Modifier.size(64.dp).clip(RoundedCornerShape(10.dp))
                        .background(Brush.linearGradient(listOf(Color(0xFF233042), Color(0xFF131A26)))),
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        g.id.take(4), style = MaterialTheme.typography.labelLarge,
                        color = Color(0xFF9FB4D4), fontFamily = FontFamily.Monospace
                    )
                }
            }
            Spacer(Modifier.width(12.dp))
            Column(Modifier.weight(1f)) {
                Text(
                    g.title, style = MaterialTheme.typography.titleMedium,
                    maxLines = 2, overflow = TextOverflow.Ellipsis
                )
                Spacer(Modifier.height(2.dp))
                Text(
                    "${g.id}  •  app ${g.appVer}",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Spacer(Modifier.height(7.dp))
                StatusChip(g.encState, g.hasKey)
            }
            Spacer(Modifier.width(8.dp))
            // Actions
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                val running = vm.runningTitleId == g.id
                if (running) {
                    Button(
                        onClick = onStop, shape = RoundedCornerShape(14.dp),
                        modifier = Modifier.height(44.dp),
                        colors = ButtonDefaults.buttonColors(
                            containerColor = MaterialTheme.colorScheme.errorContainer,
                            contentColor = MaterialTheme.colorScheme.onErrorContainer)
                    ) { Text("Stop") }
                } else {
                    Button(
                        onClick = { onRun(g.id) },
                        enabled = vm.runningTitleId == null,
                        shape = CircleShape,
                        modifier = Modifier.size(48.dp),
                        contentPadding = PaddingValues(0.dp)
                    ) {
                        Icon(Icons.Filled.PlayArrow, contentDescription = "Play",
                            modifier = Modifier.size(26.dp))
                    }
                }
                Spacer(Modifier.height(6.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    TextButton(
                        onClick = { onSettings(g.id) },
                        contentPadding = PaddingValues(horizontal = 10.dp),
                        modifier = Modifier.height(36.dp)
                    ) { Text("Settings", fontSize = 13.sp) }
                    IconButton(
                        onClick = { vm.removeFor = g },
                        modifier = Modifier.size(36.dp)
                    ) {
                        Icon(Icons.Filled.Delete, contentDescription = "Remove",
                            tint = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.size(18.dp))
                    }
                }
            }
        }
    }
}

@Composable
fun StatusChip(encState: Int, hasKey: Boolean) {
    val (bg, fg, label) = when (encState) {
        1 -> Triple(ChipEncBg, ChipEncFg,
            if (hasKey) "Disc key stored — press Play to apply"
            else "Encrypted — disc key needed")
        2 -> Triple(ChipActiveBg, ChipActiveFg, "Encrypted (decryption active) — ready")
        else -> Triple(ChipReadyBg, ChipReadyFg, "Ready to compile")
    }
    Surface(color = bg, shape = RoundedCornerShape(50)) {
        Row(
            Modifier.padding(horizontal = 10.dp, vertical = 4.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Box(Modifier.size(6.dp).clip(CircleShape).background(fg))
            Spacer(Modifier.width(7.dp))
            Text(
                label, color = fg, fontSize = 12.sp,
                fontWeight = FontWeight.Medium, lineHeight = 16.sp
            )
        }
    }
}

// ----------------------------------------------------------------------------
// App settings
// ----------------------------------------------------------------------------

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AppSettingsScreen(vm: AppUiState, onExportDiagnostics: (Boolean) -> Unit, onClose: () -> Unit) {
    var level by remember { mutableStateOf(NativeBridge.nativeGetLogLevel()) }
    val context = androidx.compose.ui.platform.LocalContext.current
    var storage by remember { mutableStateOf<Pair<Long, Long>?>(null) }

    // Real storage stats, computed off the main thread.
    LaunchedEffect(Unit) {
        storage = withContext(kotlinx.coroutines.Dispatchers.IO) {
            Diagnostics.storageStats(context)
        }
    }

    Scaffold(
        containerColor = MaterialTheme.colorScheme.background,
        topBar = {
            TopAppBar(
                title = { Text("Settings") },
                navigationIcon = {
                    IconButton(onClick = onClose) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.background,
                    titleContentColor = MaterialTheme.colorScheme.onBackground,
                    navigationIconContentColor = MaterialTheme.colorScheme.onBackground
                )
            )
        }
    ) { pad ->
        Column(
            Modifier.padding(pad).fillMaxSize().verticalScroll(rememberScrollState())
                .padding(horizontal = 20.dp)
        ) {
            SectionHeader("Logging")
            Text(
                "How much the emulator writes to the log. Debug slows big boots down a little but shows everything.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                lineHeight = 21.sp
            )
            Spacer(Modifier.height(10.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                listOf("Info+", "Warn+", "Debug", "Errors").forEachIndexed { i, label ->
                    FilterChip(
                        selected = level == i,
                        onClick = {
                            level = i
                            try { NativeBridge.nativeSetLogLevel(i) } catch (t: Throwable) { }
                        },
                        label = { Text(label, fontSize = 13.sp) }
                    )
                }
            }

            SectionHeader("Diagnostics")
            Text(
                "The report contains app version, device/CPU/OS, GPU/Vulkan capability, storage use, per-game status and the last 500 log lines.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                lineHeight = 21.sp
            )
            Spacer(Modifier.height(12.dp))
            Button(
                onClick = { onExportDiagnostics(true) },
                modifier = Modifier.fillMaxWidth().height(52.dp),
                shape = RoundedCornerShape(14.dp)
            ) {
                Icon(Icons.Filled.Check, contentDescription = null)
                Spacer(Modifier.width(8.dp))
                Text("Save log report to Downloads", style = MaterialTheme.typography.titleSmall)
            }
            Spacer(Modifier.height(8.dp))
            OutlinedButton(
                onClick = { onExportDiagnostics(false) },
                modifier = Modifier.fillMaxWidth().height(52.dp),
                shape = RoundedCornerShape(14.dp)
            ) {
                Icon(Icons.Filled.Share, contentDescription = null)
                Spacer(Modifier.width(8.dp))
                Text("Share diagnostics report", style = MaterialTheme.typography.titleSmall)
            }
            Spacer(Modifier.height(8.dp))
            OutlinedButton(
                onClick = { vm.showAppSettings = false; vm.showLogViewer = true },
                modifier = Modifier.fillMaxWidth().height(52.dp),
                shape = RoundedCornerShape(14.dp)
            ) {
                Icon(Icons.Filled.List, contentDescription = null)
                Spacer(Modifier.width(8.dp))
                Text("Open log viewer", style = MaterialTheme.typography.titleSmall)
            }

            SectionHeader("Storage")
            Text(
                storage?.let { (lib, cache) ->
                    "Library data (imported metadata + keys): ${Diagnostics.formatBytes(lib)}\n" +
                            "Log files & cache: ${Diagnostics.formatBytes(cache)}"
                } ?: "measuring…",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                lineHeight = 21.sp
            )
            Spacer(Modifier.height(8.dp))
            TextButton(onClick = {
                try {
                    val n = Diagnostics.clearCachedReports(context)
                    vm.banner(
                        if (n > 0) "Deleted $n cached log file(s)" else "No cached log files",
                        if (n > 0) 1 else 0)
                    storage = Diagnostics.storageStats(context)
                } catch (t: Throwable) {
                    vm.banner("Could not clear cache: ${t.message}", 2)
                }
            }) { Text("Delete old exported log files") }

            SectionHeader("About")
            Text("Any_Ps3 ${vm.appVersion}", style = MaterialTheme.typography.titleSmall)
            Spacer(Modifier.height(4.dp))
            Text(
                vm.statusText,
                style = MaterialTheme.typography.bodySmall,
                fontFamily = FontFamily.Monospace,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                lineHeight = 18.sp
            )
            Spacer(Modifier.height(12.dp))
            listOf(
                Triple(true, "Import & library:", "ISO read in place (no copy), folder dumps, encrypted ISOs with your disc key, decrypted EBOOTs"),
                Triple(true, "Executing:", "built-in demo ELF — real PPU interpreter + AOT pipeline, module HLE bring-up"),
                Triple(false, "In development:", "full game boot, graphics (RSX to Vulkan), SPU, audio, controls")
            ).forEach { (ok, head, tail) ->
                Row(Modifier.padding(vertical = 4.dp), verticalAlignment = Alignment.Top) {
                    Icon(
                        if (ok) Icons.Filled.Check else Icons.Filled.Info,
                        contentDescription = null,
                        tint = if (ok) MaterialTheme.colorScheme.tertiary
                        else MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.size(18.dp).padding(top = 3.dp)
                    )
                    Spacer(Modifier.width(10.dp))
                    Text(
                        buildString { append(head).append(' ').append(tail) },
                        style = MaterialTheme.typography.bodyMedium,
                        lineHeight = 21.sp
                    )
                }
            }
            Spacer(Modifier.height(28.dp))
        }
    }
}

@Composable
private fun SectionHeader(title: String) {
    Spacer(Modifier.height(22.dp))
    Text(
        title.uppercase(),
        style = MaterialTheme.typography.labelLarge,
        color = MaterialTheme.colorScheme.secondary,
        fontWeight = FontWeight.SemiBold
    )
    Spacer(Modifier.height(8.dp))
}

// ----------------------------------------------------------------------------
// Crash report (previous session)
// ----------------------------------------------------------------------------

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun CrashReportScreen(report: String, onDismiss: () -> Unit, onDelete: () -> Unit,
                      onCopy: () -> Unit, onSave: () -> Unit, onShare: () -> Unit) {
    Scaffold(
        containerColor = MaterialTheme.colorScheme.background,
        topBar = {
            TopAppBar(
                title = { Text("Previous session crashed") },
                navigationIcon = {
                    IconButton(onClick = onDismiss) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Keep for later")
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.background,
                    titleContentColor = MaterialTheme.colorScheme.onBackground,
                    navigationIconContentColor = MaterialTheme.colorScheme.onBackground
                )
            )
        }
    ) { pad ->
        Column(Modifier.padding(pad).fillMaxSize().padding(horizontal = 20.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(Icons.Filled.Warning, contentDescription = null,
                    tint = MaterialTheme.colorScheme.error, modifier = Modifier.size(20.dp))
                Spacer(Modifier.width(10.dp))
                Text(
                    "Any_Ps3 closed unexpectedly. The full report is below — " +
                            "send it with the Save or Share button so the bug can be fixed.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    lineHeight = 21.sp,
                    modifier = Modifier.weight(1f)
                )
            }
            Spacer(Modifier.height(12.dp))
            Surface(
                shape = RoundedCornerShape(12.dp),
                color = MaterialTheme.colorScheme.surfaceContainerLowest,
                border = androidx.compose.foundation.BorderStroke(1.dp, MaterialTheme.colorScheme.outlineVariant)
            ) {
                androidx.compose.foundation.text.selection.SelectionContainer {
                    Text(
                        report,
                        modifier = Modifier.fillMaxWidth().verticalScroll(rememberScrollState())
                            .padding(12.dp).heightIn(max = 420.dp),
                        fontFamily = FontFamily.Monospace,
                        fontSize = 12.sp, lineHeight = 17.sp
                    )
                }
            }
            Spacer(Modifier.height(14.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                Button(
                    onClick = onSave,
                    modifier = Modifier.weight(1f).height(50.dp),
                    shape = RoundedCornerShape(14.dp)
                ) { Text("Save to Downloads", maxLines = 1) }
                OutlinedButton(
                    onClick = onShare,
                    modifier = Modifier.weight(1f).height(50.dp),
                    shape = RoundedCornerShape(14.dp)
                ) { Text("Share", maxLines = 1) }
            }
            Spacer(Modifier.height(8.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                TextButton(onClick = onCopy, modifier = Modifier.weight(1f)) { Text("Copy report") }
                TextButton(onClick = onDelete, modifier = Modifier.weight(1f)) {
                    Text("Delete report", color = MaterialTheme.colorScheme.error)
                }
            }
            Text(
                "Closing this screen keeps the report — it will be shown again at the next start.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                lineHeight = 18.sp
            )
            Spacer(Modifier.height(20.dp))
        }
    }
}
