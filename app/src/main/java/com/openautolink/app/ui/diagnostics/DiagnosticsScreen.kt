package com.openautolink.app.ui.diagnostics

import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Abc
import androidx.compose.material.icons.filled.Cloud
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.DirectionsCar
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.NetworkCheck
import androidx.compose.material.icons.filled.Terminal
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilledTonalIconButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationRail
import androidx.compose.material3.NavigationRailItem
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel

private enum class DiagnosticsTab(
    val title: String,
    val icon: ImageVector,
) {
    SYSTEM("System", Icons.Default.Info),
    NETWORK("Network", Icons.Default.NetworkCheck),
    BRIDGE("Bridge", Icons.Default.Cloud),
    CAR("Car", Icons.Default.DirectionsCar),
    LOGS("Logs", Icons.Default.Terminal),
}

@Composable
fun DiagnosticsScreen(
    viewModel: DiagnosticsViewModel = viewModel(),
    onBack: () -> Unit = {},
) {
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    var selectedTab by remember { mutableStateOf(DiagnosticsTab.SYSTEM) }

    Surface(
        modifier = Modifier.fillMaxSize(),
        color = MaterialTheme.colorScheme.background,
    ) {
        Row(
            modifier = Modifier
                .fillMaxSize()
                .windowInsetsPadding(WindowInsets.safeDrawing)
                .testTag("diagnosticsScreen"),
        ) {
            // Left rail — back button + tab icons
            Column(modifier = Modifier.fillMaxHeight()) {
                Box(modifier = Modifier.padding(vertical = 16.dp, horizontal = 12.dp)) {
                    FilledTonalIconButton(
                        onClick = onBack,
                        modifier = Modifier.size(56.dp),
                    ) {
                        Icon(
                            imageVector = Icons.AutoMirrored.Filled.ArrowBack,
                            contentDescription = "Back",
                            modifier = Modifier.size(24.dp),
                        )
                    }
                }

                NavigationRail(
                    modifier = Modifier.weight(1f),
                    containerColor = MaterialTheme.colorScheme.background,
                ) {
                    Spacer(modifier = Modifier.weight(1f))
                    DiagnosticsTab.entries.forEach { tab ->
                        NavigationRailItem(
                            selected = selectedTab == tab,
                            onClick = { selectedTab = tab },
                            icon = {
                                Icon(
                                    imageVector = tab.icon,
                                    contentDescription = tab.title,
                                )
                            },
                            label = { Text(tab.title) },
                            modifier = Modifier.padding(vertical = 8.dp),
                        )
                    }
                    Spacer(modifier = Modifier.weight(1f))
                }
            }

            // Content pane
            Box(
                modifier = Modifier
                    .fillMaxHeight()
                    .weight(1f)
                    .padding(horizontal = 24.dp, vertical = 16.dp),
            ) {
                when (selectedTab) {
                    DiagnosticsTab.SYSTEM -> SystemTab(uiState.system)
                    DiagnosticsTab.NETWORK -> NetworkTab(uiState.network)
                    DiagnosticsTab.BRIDGE -> BridgeTab(uiState.bridge)
                    DiagnosticsTab.CAR -> CarTab(uiState.car)
                    DiagnosticsTab.LOGS -> LogsTab(uiState.logs, uiState.logFilter, viewModel)
                }
            }
        }
    }
}

// --- System Tab ---

@Composable
private fun SystemTab(info: SystemInfo) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState()),
    ) {
        SectionHeader("Device")
        DiagRow("Android", "${info.androidVersion} (API ${info.sdkLevel})")
        DiagRow("Manufacturer", info.manufacturer)
        DiagRow("Model", info.model)
        DiagRow("Device", info.device)
        DiagRow("SoC", info.soc)

        Spacer(modifier = Modifier.height(16.dp))
        SectionHeader("Display")
        DiagRow("Resolution", "${info.displayWidth} × ${info.displayHeight}")
        DiagRow("Density", "${info.displayDpi} dpi")

        Spacer(modifier = Modifier.height(16.dp))
        SectionHeader("H.264 Decoders")
        if (info.h264Decoders.isEmpty()) {
            DiagRow("", "None available")
        } else {
            info.h264Decoders.forEach { codec ->
                DiagRow(
                    codec.name,
                    if (codec.hwAccelerated) "HW" else "SW",
                    valueColor = if (codec.hwAccelerated) Color(0xFF4CAF50) else Color(0xFFFF9800),
                )
            }
        }

        Spacer(modifier = Modifier.height(16.dp))
        SectionHeader("H.265 Decoders")
        if (info.h265Decoders.isEmpty()) {
            DiagRow("", "None available")
        } else {
            info.h265Decoders.forEach { codec ->
                DiagRow(
                    codec.name,
                    if (codec.hwAccelerated) "HW" else "SW",
                    valueColor = if (codec.hwAccelerated) Color(0xFF4CAF50) else Color(0xFFFF9800),
                )
            }
        }

        Spacer(modifier = Modifier.height(16.dp))
        SectionHeader("VP9 Decoders")
        if (info.vp9Decoders.isEmpty()) {
            DiagRow("", "None available")
        } else {
            info.vp9Decoders.forEach { codec ->
                DiagRow(
                    codec.name,
                    if (codec.hwAccelerated) "HW" else "SW",
                    valueColor = if (codec.hwAccelerated) Color(0xFF4CAF50) else Color(0xFFFF9800),
                )
            }
        }
    }
}

// --- Network Tab ---

@Composable
private fun NetworkTab(info: NetworkInfo) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState()),
    ) {
        SectionHeader("Bridge Connection")
        DiagRow("Host", info.bridgeHost)
        DiagRow("Port", info.bridgePort.toString())
        DiagRow("Session", info.sessionState.name, valueColor = sessionStateColor(info.sessionState))

        Spacer(modifier = Modifier.height(16.dp))
        SectionHeader("TCP Channels")
        DiagRow("Control (5288)", info.controlState, valueColor = channelStateColor(info.controlState))
        DiagRow("Video (5290)", info.videoState, valueColor = channelStateColor(info.videoState))
        DiagRow("Audio (5289)", info.audioState, valueColor = channelStateColor(info.audioState))
    }
}

// --- Bridge Tab ---

@Composable
private fun BridgeTab(stats: BridgeStats) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState()),
    ) {
        SectionHeader("Bridge Info")
        DiagRow("Name", stats.bridgeName ?: "—")
        DiagRow("Version", stats.bridgeVersion?.toString() ?: "—")
        if (stats.capabilities.isNotEmpty()) {
            DiagRow("Capabilities", stats.capabilities.joinToString(", "))
        }

        if (stats.uptimeSeconds > 0) {
            Spacer(modifier = Modifier.height(16.dp))
            SectionHeader("Bridge Statistics")
            DiagRow("Uptime", formatUptime(stats.uptimeSeconds))
            DiagRow("Video frames sent", stats.videoFramesSent.toString())
            DiagRow("Audio frames sent", stats.audioFramesSent.toString())
        }

        Spacer(modifier = Modifier.height(16.dp))
        SectionHeader("Video Decoder")
        DiagRow("Codec", stats.videoStats.codec)
        if (stats.videoStats.width > 0) {
            DiagRow("Resolution", "${stats.videoStats.width}×${stats.videoStats.height}")
        }
        DiagRow("FPS", "${"%.1f".format(stats.videoStats.fps)}",
            valueColor = if (stats.videoStats.fps >= 25) Color(0xFF4CAF50) else Color(0xFFFF5722))
        if (stats.videoStats.bitrateKbps > 0) {
            val bitrateStr = if (stats.videoStats.bitrateKbps >= 1000) {
                "${"%.1f".format(stats.videoStats.bitrateKbps / 1000)} Mbps"
            } else {
                "${stats.videoStats.bitrateKbps.toInt()} kbps"
            }
            DiagRow("Bitrate", bitrateStr,
                valueColor = if (stats.videoStats.bitrateKbps >= 2000) Color(0xFF4CAF50) else Color(0xFFFF9800))
        }
        DiagRow("Decoded", stats.videoStats.framesDecoded.toString())
        DiagRow("Dropped", stats.videoStats.framesDropped.toString(),
            valueColor = if (stats.videoStats.framesDropped > 0) Color(0xFFFF9800) else Color.White)

        Spacer(modifier = Modifier.height(16.dp))
        SectionHeader("Audio Player")
        if (stats.audioStats.activePurposes.isNotEmpty()) {
            DiagRow("Active", stats.audioStats.activePurposes.joinToString(", ") { it.name.lowercase() })
            stats.audioStats.underruns.forEach { (purpose, count) ->
                if (count > 0) {
                    DiagRow("${purpose.name.lowercase()} underruns", count.toString(),
                        valueColor = Color(0xFFFF9800))
                }
            }
            stats.audioStats.framesWritten.forEach { (purpose, count) ->
                DiagRow("${purpose.name.lowercase()} frames", count.toString())
            }
        } else {
            DiagRow("Status", "No active audio")
        }
    }
}

// --- Car Tab ---

@Composable
private fun CarTab(car: CarInfo) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState()),
    ) {
        if (!car.isActive) {
            SectionHeader("Vehicle Sensors")
            DiagRow("Status", "Unavailable", valueColor = Color(0xFF808080))
            DiagRow("", "VHAL not connected — requires AAOS vehicle")
        } else {
            SectionHeader("Powertrain")
            DiagRow("Speed", car.speedKmh?.let { "${"%.1f".format(it)} km/h" } ?: "—")
            DiagRow("Gear", car.gear ?: "—",
                valueColor = when (car.gear) {
                    "P" -> Color(0xFF4CAF50)
                    "R" -> Color(0xFFFF9800)
                    "D", "1", "2", "3", "4" -> Color(0xFF2196F3)
                    else -> Color.White
                })
            DiagRow("Parking Brake", car.parkingBrake?.let { if (it) "ON" else "OFF" } ?: "—",
                valueColor = if (car.parkingBrake == true) Color(0xFFFF9800) else Color(0xFF4CAF50))
            car.rpmE3?.let {
                DiagRow("RPM", "${"%.0f".format(it / 1000f)}")
            }

            Spacer(modifier = Modifier.height(16.dp))
            SectionHeader("Energy")
            car.batteryPct?.let { DiagRow("EV Battery", "$it%",
                valueColor = when {
                    it > 50 -> Color(0xFF4CAF50)
                    it > 20 -> Color(0xFFFFC107)
                    else -> Color(0xFFFF5722)
                }) }
            car.fuelLevelPct?.let { DiagRow("Fuel Level", "$it%") }
            car.rangeKm?.let { DiagRow("Range", "${"%.1f".format(it)} km") }
            car.lowFuel?.let { DiagRow("Low Fuel", if (it) "YES" else "No",
                valueColor = if (it) Color(0xFFFF5722) else Color(0xFF4CAF50)) }

            Spacer(modifier = Modifier.height(16.dp))
            SectionHeader("Environment")
            DiagRow("Night Mode", car.nightMode?.let { if (it) "ON" else "OFF" } ?: "—",
                valueColor = if (car.nightMode == true) Color(0xFF64B5F6) else Color(0xFFFFC107))
            car.ambientTempC?.let { DiagRow("Outside Temp", "${"%.1f".format(it)} °C") }

            if (car.turnSignal != null || car.headlight != null || car.hazardLights != null) {
                Spacer(modifier = Modifier.height(16.dp))
                SectionHeader("Lights")
                car.turnSignal?.let { DiagRow("Turn Signal", it.replaceFirstChar { c -> c.uppercase() }) }
                car.headlight?.let { DiagRow("Headlights", headlightToString(it)) }
                car.hazardLights?.let { DiagRow("Hazard Lights", if (it) "ON" else "OFF",
                    valueColor = if (it) Color(0xFFFF9800) else Color.White) }
            }

            if (car.steeringAngleDeg != null || car.odometerKm != null) {
                Spacer(modifier = Modifier.height(16.dp))
                SectionHeader("Other")
                car.steeringAngleDeg?.let { DiagRow("Steering Angle", "${"%.1f".format(it)}°") }
                car.odometerKm?.let { DiagRow("Odometer", "${"%.1f".format(it)} km") }
            }
        }
    }
}

private fun headlightToString(state: Int): String = when (state) {
    0 -> "Off"
    1 -> "On"
    2 -> "Daytime Running"
    else -> "Unknown ($state)"
}

// --- Logs Tab ---

@Composable
private fun LogsTab(
    logs: List<LogEntry>,
    currentFilter: LogSeverity,
    viewModel: DiagnosticsViewModel,
) {
    Column(modifier = Modifier.fillMaxSize()) {
        // Filter chips row + clear button
        Row(
            modifier = Modifier
                .fillMaxWidth(0.7f)
                .padding(bottom = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            LogSeverity.entries.forEach { severity ->
                FilterChip(
                    selected = currentFilter == severity,
                    onClick = { viewModel.setLogFilter(severity) },
                    label = { Text(severity.name, fontSize = 11.sp) },
                    modifier = Modifier.padding(end = 6.dp),
                )
            }
            Spacer(modifier = Modifier.weight(1f))
            IconButton(onClick = { viewModel.clearLogs() }) {
                Icon(
                    imageVector = Icons.Default.Delete,
                    contentDescription = "Clear logs",
                    tint = MaterialTheme.colorScheme.error,
                )
            }
        }

        HorizontalDivider()

        // Log entries
        val listState = rememberLazyListState()
        if (logs.isEmpty()) {
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(top = 32.dp),
                contentAlignment = Alignment.TopCenter,
            ) {
                Text(
                    text = "No log entries",
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    style = MaterialTheme.typography.bodyMedium,
                )
            }
        } else {
            LazyColumn(
                state = listState,
                modifier = Modifier.fillMaxSize(),
            ) {
                items(logs) { entry ->
                    LogEntryRow(entry)
                }
            }
        }
    }
}

@Composable
private fun LogEntryRow(entry: LogEntry) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 2.dp, horizontal = 4.dp),
    ) {
        // Timestamp
        Text(
            text = formatTimestamp(entry.timestamp),
            fontSize = 9.sp,
            fontFamily = FontFamily.Monospace,
            color = Color(0xFF808080),
            modifier = Modifier.width(60.dp),
        )
        // Severity indicator
        Text(
            text = entry.severity.name.first().toString(),
            fontSize = 9.sp,
            fontFamily = FontFamily.Monospace,
            fontWeight = FontWeight.Bold,
            color = severityColor(entry.severity),
            modifier = Modifier.width(14.dp),
        )
        // Tag
        Text(
            text = entry.tag,
            fontSize = 9.sp,
            fontFamily = FontFamily.Monospace,
            color = Color(0xFF64B5F6),
            modifier = Modifier.width(72.dp),
        )
        // Message
        Text(
            text = entry.message,
            fontSize = 9.sp,
            fontFamily = FontFamily.Monospace,
            color = Color.White,
        )
    }
}

// --- Shared composables ---

@Composable
private fun SectionHeader(title: String) {
    Text(
        text = title,
        style = MaterialTheme.typography.headlineSmall,
        color = MaterialTheme.colorScheme.primary,
        fontWeight = FontWeight.SemiBold,
        modifier = Modifier.padding(vertical = 8.dp),
    )
}

@Composable
private fun DiagRow(
    label: String,
    value: String,
    valueColor: Color = Color.White,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth(0.7f)
            .padding(vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.weight(1f),
        )
        Text(
            text = value,
            style = MaterialTheme.typography.bodyMedium,
            fontWeight = FontWeight.Medium,
            color = valueColor,
            fontFamily = FontFamily.Monospace,
        )
    }
}

// --- Helpers ---

private fun sessionStateColor(state: com.openautolink.app.session.SessionState): Color = when (state) {
    com.openautolink.app.session.SessionState.STREAMING -> Color(0xFF4CAF50)
    com.openautolink.app.session.SessionState.PHONE_CONNECTED,
    com.openautolink.app.session.SessionState.BRIDGE_CONNECTED -> Color(0xFFFFC107)
    com.openautolink.app.session.SessionState.CONNECTING -> Color(0xFF2196F3)
    com.openautolink.app.session.SessionState.IDLE -> Color(0xFF808080)
    com.openautolink.app.session.SessionState.ERROR -> Color(0xFFFF5722)
}

private fun channelStateColor(state: String): Color = when (state) {
    "STREAMING" -> Color(0xFF4CAF50)
    "CONNECTED" -> Color(0xFFFFC107)
    "CONNECTING" -> Color(0xFF2196F3)
    else -> Color(0xFF808080)
}

private fun severityColor(severity: LogSeverity): Color = when (severity) {
    LogSeverity.DEBUG -> Color(0xFF808080)
    LogSeverity.INFO -> Color(0xFF4CAF50)
    LogSeverity.WARN -> Color(0xFFFFC107)
    LogSeverity.ERROR -> Color(0xFFFF5722)
}

private fun formatTimestamp(ms: Long): String {
    val sdf = java.text.SimpleDateFormat("HH:mm:ss", java.util.Locale.US)
    return sdf.format(java.util.Date(ms))
}

private fun formatUptime(seconds: Long): String {
    val h = seconds / 3600
    val m = (seconds % 3600) / 60
    val s = seconds % 60
    return if (h > 0) "${h}h ${m}m ${s}s" else if (m > 0) "${m}m ${s}s" else "${s}s"
}
