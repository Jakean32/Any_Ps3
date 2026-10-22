// Any_Ps3 — per-game settings screen (Android). Reads/writes through the same
// settings.ini the core uses, via a tiny JNI key=value surface.
package com.anyps3.app

import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

@Composable
fun SettingsScreen(id: String, onClose: () -> Unit) {
    var loaded by remember { mutableStateOf(false) }
    var scale by remember { mutableStateOf(1) }
    var frameLimit by remember { mutableStateOf(0) }
    var shaderCache by remember { mutableStateOf(true) }
    var backend by remember { mutableStateOf(0) }

    LaunchedEffect(id) {
        val kv = NativeBridge.nativeGetGameSettings(id).split("|")
        scale = kv.getOrNull(0)?.toIntOrNull() ?: 1
        frameLimit = kv.getOrNull(1)?.toIntOrNull() ?: 0
        shaderCache = (kv.getOrNull(2)?.toIntOrNull() ?: 1) != 0
        backend = kv.getOrNull(3)?.toIntOrNull() ?: 0
        loaded = true
    }

    if (!loaded) return
    Column(Modifier.padding(20.dp)) {
        Text("Settings — $id", style = MaterialTheme.typography.titleLarge)
        Spacer(Modifier.height(12.dp))
        Text("Resolution scale: $scale")
        Slider(value = scale.toFloat(), onValueChange = { scale = it.toInt() },
            valueRange = 1f..4f, steps = 2)
        Text("Frame limit: ${if (frameLimit == 0) "unlimited" else "$frameLimit fps"}")
        Slider(value = frameLimit.toFloat(), onValueChange = { frameLimit = it.toInt() },
            valueRange = 0f..120f)
        Row {
            Checkbox(checked = shaderCache, onCheckedChange = { shaderCache = it })
            Text("Shader cache", Modifier.padding(top = 12.dp))
        }
        Row {
            RadioButton(selected = backend == 0, onClick = { backend = 0 })
            Text("AOT + fallback", Modifier.padding(top = 12.dp), fontSize = 13.sp)
            Spacer(Modifier.width(16.dp))
            RadioButton(selected = backend == 1, onClick = { backend = 1 })
            Text("Interpreter only", Modifier.padding(top = 12.dp), fontSize = 13.sp)
        }
        Spacer(Modifier.height(16.dp))
        Button(onClick = {
            NativeBridge.nativeSetGameSettings(
                id, "resolution_scale=$scale;frame_limit=$frameLimit;" +
                    "shader_cache=${if (shaderCache) 1 else 0};ppu_backend=$backend")
            onClose()
        }) { Text("Save") }
        Spacer(Modifier.width(8.dp))
        TextButton(onClick = onClose) { Text("Cancel") }
    }
}
