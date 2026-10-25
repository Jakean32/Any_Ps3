// Any_Ps3 — full-screen log viewer (Android). The log is readable IN the
// app (no export needed): level filter, text search, selectable/copyable
// text, save-to-Downloads and share actions. This screen is the fallback
// that guarantees the user can always get to their logs.
package com.anyps3.app

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material.icons.filled.KeyboardArrowUp
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

private val LEVELS = listOf("All", "Info", "Warn", "Error")

private fun lineMatches(line: String, level: Int, query: String): Boolean {
    if (query.isNotEmpty() && !line.contains(query, ignoreCase = true)) return false
    if (level == 0) return true
    val l = line.uppercase()
    val isErr = l.contains("ERROR") || l.contains("ERR:") || l.contains("FATAL") || l.contains("CRASH")
    val isWarn = l.contains("WARN") || l.contains("MISSING") || l.contains("NOT IMPLEMENTED") || l.contains("ENCRYPTED")
    return when (level) {
        3 -> isErr
        2 -> isErr || isWarn || l.contains("DEBUG")
        else -> true // "Info" shows everything except nothing — plain view
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun LogViewerScreen(
    logText: String,
    onBack: () -> Unit,
    onSave: () -> Unit,
    onShare: () -> Unit,
    onCopy: () -> Unit
) {
    var level by remember { mutableStateOf(0) }
    var query by remember { mutableStateOf("") }
    val listState = rememberLazyListState()
    var autoScroll by remember { mutableStateOf(true) }

    val lines = remember(logText, level, query) {
        logText.split('\n').filter { it.isNotBlank() && lineMatches(it, level, query.trim()) }
    }

    // Jump to the newest line while auto-scroll is on.
    LaunchedEffect(lines.size, autoScroll) {
        if (autoScroll && lines.isNotEmpty()) listState.scrollToItem(lines.lastIndex.coerceAtLeast(0))
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Log") },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back")
                    }
                },
                actions = {
                    TextButton(onClick = onCopy) { Text("Copy") }
                    TextButton(onClick = onSave) { Text("Save") }
                    TextButton(onClick = onShare) { Text("Share") }
                }
            )
        },
        floatingActionButton = {
            if (lines.size > 30) {
                SmallFloatingActionButton(onClick = { autoScroll = !autoScroll }) {
                    Icon(
                        if (autoScroll) Icons.Filled.KeyboardArrowDown else Icons.Filled.KeyboardArrowUp,
                        contentDescription = if (autoScroll) "Stop auto-scroll" else "Jump to newest"
                    )
                }
            }
        }
    ) { pad ->
        Column(Modifier.padding(pad).fillMaxSize()) {
            OutlinedTextField(
                value = query,
                onValueChange = { query = it },
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp),
                placeholder = { Text("Search in log") },
                singleLine = true,
                trailingIcon = {
                    if (query.isNotEmpty()) {
                        IconButton(onClick = { query = "" }) {
                            Icon(Icons.Filled.Close, contentDescription = "Clear search")
                        }
                    }
                }
            )
            Row(
                Modifier.padding(horizontal = 16.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                LEVELS.forEachIndexed { i, label ->
                    FilterChip(
                        selected = level == i,
                        onClick = { level = i },
                        label = { Text(label) }
                    )
                }
            }
            Row(
                Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    "${lines.size} lines",
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                Spacer(Modifier.width(12.dp))
                Text(
                    if (autoScroll) "auto-scroll on (newest at bottom)" else "auto-scroll off",
                    style = MaterialTheme.typography.labelMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
            HorizontalDivider()
            SelectionContainer {
                LazyColumn(
                    state = listState,
                    modifier = Modifier.fillMaxSize().padding(horizontal = 12.dp, vertical = 8.dp)
                ) {
                    items(lines) { line ->
                        Text(
                            line,
                            fontFamily = FontFamily.Monospace,
                            fontSize = 12.sp,
                            lineHeight = 17.sp,
                            color = MaterialTheme.colorScheme.onSurface,
                            modifier = Modifier.padding(vertical = 1.dp)
                        )
                    }
                }
            }
        }
    }
}
