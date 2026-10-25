// Any_Ps3 — per-game settings screen (Android), v0.6.0 redesign.
//
// Real features only (unchanged set, better presentation):
//  * disc key for encrypted ISOs: .dkey/.key file picker OR typed 32-hex;
//    after attach, MainActivity re-reads the ISO through the SAF grant and
//    the entry becomes "Ready to compile"
//  * decrypted executable for encrypted retail EBOOTs (user's own ELF/fSELF)
//  * controller mapping (fed live by PadInput into the input module)
//
// v0.6.0 bug fix: the old version wrote snapshot state during composition
// (`pad = pad + (btn to code)` inside the render loop), which is illegal in
// Compose and can loop recomposition forever. The map is now local state,
// written only from input callbacks and saved on Save.
package com.anyps3.app

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Info
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsScreen(id: String, onPickKey: (String) -> Unit,
                   onAttachKeyText: (String, String) -> Unit,
                   onPickEboot: (String) -> Unit, onClose: () -> Unit) {
    var loaded by remember { mutableStateOf(false) }
    var hasKey by remember { mutableStateOf(false) }
    var encState by remember { mutableStateOf(0) }
    var hasEboot by remember { mutableStateOf(false) }
    var ebootName by remember { mutableStateOf("") }
    var typedKey by remember { mutableStateOf("") }

    // Local, editable copy of the controller mapping — written ONLY from
    // input callbacks (never during composition), saved on "Save".
    val pad = remember { mutableStateMapOf<String, Int>() }

    LaunchedEffect(id) {
        hasKey = NativeBridge.nativeHasDiscKey(id)
        encState = NativeBridge.nativeEncState(id)
        hasEboot = NativeBridge.nativeHasDecryptedEboot(id)
        ebootName = NativeBridge.nativeDecryptedEbootName(id)
        val kv = NativeBridge.nativeGetGameSettings(id)
        kv.split(";").filter { it.contains("=") }.forEach {
            val idx = it.indexOf('=')
            if (idx > 0) {
                val k = it.substring(0, idx)
                val v = it.substring(idx + 1).toIntOrNull() ?: -1
                pad[k] = v
            }
        }
        loaded = true
    }

    if (!loaded) {
        Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
            CircularProgressIndicator()
        }
        return
    }

    Scaffold(
        containerColor = MaterialTheme.colorScheme.background,
        topBar = {
            TopAppBar(
                title = { Text("Game settings") },
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
    ) { pad0 ->
        Column(
            Modifier.padding(pad0).fillMaxSize().verticalScroll(rememberScrollState())
                .padding(horizontal = 20.dp)
        ) {
            Text(id, style = MaterialTheme.typography.labelLarge,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                fontFamily = FontFamily.Monospace)
            Spacer(Modifier.height(8.dp))

            // ---- Disc key ------------------------------------------------
            SectionTitle("Disc key (encrypted ISO images)")
            Text(
                when {
                    encState == 0 -> "Plain image — no disc key needed."
                    hasKey -> "Disc key stored (app-private). Re-read applies it below."
                    else -> "Attach your disc key: a .dkey / .key file, or paste the 32 hex characters."
                },
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                lineHeight = 21.sp
            )
            Spacer(Modifier.height(12.dp))
            Button(
                onClick = { onPickKey(id) },
                modifier = Modifier.fillMaxWidth().height(50.dp),
                shape = RoundedCornerShapeV14
            ) { Text("Select disc key file (.dkey / .key)", style = MaterialTheme.typography.titleSmall) }

            Spacer(Modifier.height(12.dp))
            OutlinedTextField(
                value = typedKey,
                onValueChange = { t -> typedKey = t.trim() },
                label = { Text("or paste the 32-hex-character disc key") },
                placeholder = { Text("0123456789abcdef0123456789abcdef") },
                modifier = Modifier.fillMaxWidth(),
                singleLine = true,
                shape = RoundedCornerShapeV14,
                keyboardOptions = KeyboardOptions(
                    autoCorrect = false,
                    capitalization = KeyboardCapitalization.Characters)
            )
            if (typedKey.isNotEmpty() && typedKey.length != 32) {
                Spacer(Modifier.height(4.dp))
                Text(
                    "a disc key is exactly 32 hex characters — ${typedKey.length} so far",
                    fontSize = 12.sp, color = MaterialTheme.colorScheme.error
                )
            }
            if (typedKey.length == 32) {
                Spacer(Modifier.height(10.dp))
                Button(
                    onClick = {
                        onAttachKeyText(id, typedKey)
                        onClose()
                    },
                    modifier = Modifier.fillMaxWidth().height(50.dp),
                    shape = RoundedCornerShapeV14
                ) { Text("Attach key and re-read the ISO", style = MaterialTheme.typography.titleSmall) }
            }
            if (hasKey) {
                Spacer(Modifier.height(6.dp))
                Text(
                    "A key file is stored for this game. A new pick or typed key replaces it.",
                    fontSize = 12.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }

            // ---- Decrypted executable -------------------------------------
            SectionDivider()
            SectionTitle("Decrypted executable (encrypted retail EBOOTs)")
            Text(
                if (hasEboot) "Stored: $ebootName — Play boots this instead of the encrypted EBOOT.BIN."
                else "Retail discs carry an encrypted EBOOT.BIN. Any_Ps3 ships no keys, so it " +
                        "cannot decrypt them. If you decrypted your own legal dump with your own " +
                        "tools, pick that ELF/fSELF here.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                lineHeight = 21.sp
            )
            Spacer(Modifier.height(12.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                Button(
                    onClick = { onPickEboot(id) },
                    modifier = Modifier.weight(1f).height(50.dp),
                    shape = RoundedCornerShapeV14
                ) { Text("Select executable", style = MaterialTheme.typography.titleSmall, maxLines = 1) }
                if (hasEboot) {
                    OutlinedButton(
                        onClick = {
                            try {
                                NativeBridge.nativeClearDecryptedEboot(id)
                                hasEboot = false
                            } catch (t: Throwable) { }
                        },
                        modifier = Modifier.height(50.dp),
                        shape = RoundedCornerShapeV14
                    ) { Text("Clear", style = MaterialTheme.typography.titleSmall) }
                }
            }

            // ---- Controller mapping ---------------------------------------
            SectionDivider()
            SectionTitle("Controller mapping")
            Text(
                "Android keycode per guest pad button (0 = unmapped). Physical gamepads " +
                        "feed these live into the input module.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                lineHeight = 21.sp
            )
            Spacer(Modifier.height(12.dp))
            val buttons = listOf(
                "cross" to "Cross (X)", "circle" to "Circle (O)", "square" to "Square",
                "triangle" to "Triangle", "start" to "Start", "select" to "Select",
                "l1" to "L1", "r2" to "R2", "up" to "D-pad up", "down" to "D-pad down",
                "left" to "D-pad left", "right" to "D-pad right"
            )
            Surface(
                shape = RoundedCornerShapeV14,
                color = MaterialTheme.colorScheme.surfaceContainer
            ) {
                Column(Modifier.padding(6.dp)) {
                    buttons.forEachIndexed { i, (key, label) ->
                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            modifier = Modifier.fillMaxWidth().padding(horizontal = 10.dp)
                        ) {
                            Text(
                                label, modifier = Modifier.weight(1f),
                                style = MaterialTheme.typography.bodyMedium
                            )
                            OutlinedTextField(
                                value = (pad[key] ?: -1).toString(),
                                onValueChange = { t ->
                                    t.toIntOrNull()?.let { pad[key] = it }
                                },
                                modifier = Modifier.width(96.dp),
                                singleLine = true,
                                textStyle = MaterialTheme.typography.bodyMedium.copy(
                                    fontFamily = FontFamily.Monospace)
                            )
                        }
                        if (i < buttons.lastIndex) HorizontalDivider(
                            color = MaterialTheme.colorScheme.outlineVariant)
                    }
                }
            }

            Spacer(Modifier.height(24.dp))
            Button(
                onClick = {
                    try {
                        val kv = pad.entries.joinToString(";") { "pad_${it.key}=${it.value}" }
                        NativeBridge.nativeSetGameSettings(id, kv)
                    } catch (t: Throwable) { }
                    onClose()
                },
                modifier = Modifier.fillMaxWidth().height(52.dp),
                shape = RoundedCornerShapeV14
            ) {
                Icon(Icons.Filled.Check, contentDescription = null)
                Spacer(Modifier.width(8.dp))
                Text("Save settings", style = MaterialTheme.typography.titleMedium)
            }
            Spacer(Modifier.height(10.dp))
            TextButton(onClick = onClose, modifier = Modifier.fillMaxWidth()) { Text("Cancel") }
            Spacer(Modifier.height(24.dp))
        }
    }
}

private val RoundedCornerShapeV14 = androidx.compose.foundation.shape.RoundedCornerShape(14.dp)

@Composable
private fun SectionTitle(title: String) {
    Text(
        title, style = MaterialTheme.typography.titleMedium,
        fontWeight = FontWeight.SemiBold
    )
    Spacer(Modifier.height(6.dp))
}

@Composable
private fun SectionDivider() {
    Spacer(Modifier.height(20.dp))
    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
    Spacer(Modifier.height(20.dp))
}
