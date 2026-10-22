// Any_Ps3 — Android UI (Jetpack Compose).
// Screens: FirstRun (legal consent) -> Library (list/search) -> Settings / Import.
package com.anyps3.app

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/** State holder for the whole app UI. */
class AppUiState {
    var firstRunAccepted by mutableStateOf(false)
    var games by mutableStateOf(listOf<GameRow>())
    var search by mutableStateOf("")
    var importStatus by mutableStateOf<String?>(null)
    var importBusy by mutableStateOf(false)
    var runtimeLog by mutableStateOf("")
    var runningTitleId by mutableStateOf<String?>(null)
    var settingsFor by mutableStateOf<String?>(null)

    data class GameRow(val id: String, val title: String, val appVer: String, val iconPath: String)
}

@Composable
fun AnyPs3App(vm: AppUiState, onAddGame: () -> Unit, onRun: (String) -> Unit,
              onStop: () -> Unit, onSettings: (String) -> Unit,
              onRemove: (String) -> Unit) {
    MaterialTheme(colorScheme = darkColorScheme()) {
        Surface(Modifier.fillMaxSize(), color = Color(0xFF101014)) {
            if (!vm.firstRunAccepted) {
                FirstRunScreen(vm)
            } else {
                LibraryScreen(vm, onAddGame, onRun, onStop, onSettings, onRemove)
                vm.settingsFor?.let { id ->
                    // simple modal: draw settings on top until closed
                    Box(Modifier.fillMaxSize().padding(24.dp)) {
                        Surface(
                            color = Color(0xFF181B22),
                            modifier = Modifier.fillMaxWidth()) {
                            SettingsScreen(id) { vm.settingsFor = null }
                        }
                    }
                }
            }
        }
    }
}

@Composable
fun FirstRunScreen(vm: AppUiState) {
    Column(Modifier.padding(24.dp)) {
        Text("Welcome to Any_Ps3", style = MaterialTheme.typography.headlineLarge)
        Spacer(Modifier.height(16.dp))
        Text(
            "Any_Ps3 runs PS3 software by translating it ahead-of-time. " +
                    "It contains no Sony firmware, no keys and no game content.",
            style = MaterialTheme.typography.bodyMedium)
        Spacer(Modifier.height(12.dp))
        listOf(
            "I own the games I import and my dumps are legal.",
            "I will not import content I do not own.",
            "Encrypted retail executables cannot be loaded (no keys are shipped); " +
                    "homebrew/debug executables are supported."
        ).forEach {
            Row(Modifier.padding(vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
                Text("•  ")
                Text(it)
            }
        }
        Spacer(Modifier.height(20.dp))
        Button(onClick = {
            NativeBridge.nativeAcceptFirstRun()
            vm.firstRunAccepted = NativeBridge.nativeFirstRunAccepted()
        }) { Text("I agree — continue") }
    }
}

@Composable
fun LibraryScreen(vm: AppUiState, onAddGame: () -> Unit, onRun: (String) -> Unit,
                  onStop: () -> Unit, onSettings: (String) -> Unit, onRemove: (String) -> Unit) {
    Column(Modifier.padding(16.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("Any_Ps3 library", style = MaterialTheme.typography.titleLarge)
            Spacer(Modifier.weight(1f))
            Button(onClick = onAddGame, enabled = !vm.importBusy) { Text("Add game") }
        }
        OutlinedTextField(
            value = vm.search, onValueChange = { vm.search = it },
            label = { Text("search") }, modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp))
        vm.importStatus?.let {
            Text(it, color = MaterialTheme.colorScheme.secondary, fontSize = 13.sp)
            Spacer(Modifier.height(8.dp))
        }
        LazyColumn {
            val q = vm.search.lowercase()
            items(vm.games.filter { it.title.lowercase().contains(q) || it.id.lowercase().contains(q) }) { g ->
                GameCard(g, vm, onRun, onStop, onSettings, onRemove)
                HorizontalDivider()
            }
        }
        if (vm.runtimeLog.isNotEmpty()) {
            Text("Runtime log", style = MaterialTheme.typography.labelLarge)
            Box(Modifier.fillMaxWidth().padding(8.dp)) {
                Text(
                    vm.runtimeLog.takeLast(8000),
                    fontFamily = FontFamily.Monospace, fontSize = 10.sp)
            }
        }
    }
}

@Composable
fun GameCard(g: AppUiState.GameRow, vm: AppUiState, onRun: (String) -> Unit,
             onStop: () -> Unit, onSettings: (String) -> Unit, onRemove: (String) -> Unit) {
    Row(Modifier.fillMaxWidth().padding(vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically) {
        // icon: loaded from g.iconPath by coil/glide in a full build; a color block keeps
        // this file dependency-free while remaining honest about what is implemented.
        Surface(
            color = Color(0xFF20242E), modifier = Modifier.size(72.dp)) {
            Box(contentAlignment = Alignment.Center) { Text(g.id.take(4)) }
        }
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            Text(g.title, style = MaterialTheme.typography.titleMedium)
            Text("${g.id}  app ${g.appVer}", fontSize = 12.sp)
        }
        Column(horizontalAlignment = Alignment.End) {
            if (vm.runningTitleId == g.id) {
                Button(onClick = onStop) { Text("Stop") }
            } else {
                Button(onClick = { onRun(g.id) }, enabled = vm.runningTitleId == null) { Text("Run") }
            }
            Row {
                TextButton(onClick = { onSettings(g.id) }) { Text("Settings") }
                TextButton(onClick = { onRemove(g.id) }) { Text("Remove") }
            }
        }
    }
}
