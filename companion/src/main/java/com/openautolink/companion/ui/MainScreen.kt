package com.openautolink.companion.ui

import android.annotation.SuppressLint
import android.bluetooth.BluetoothManager
import android.content.Context
import androidx.compose.animation.animateColorAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.Image
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Bluetooth
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.DirectionsCar
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.material.icons.outlined.Description
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawWithCache
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.BlendMode
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.CompositingStrategy
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.openautolink.companion.R
import com.openautolink.companion.BuildConfig
import com.openautolink.companion.CompanionPrefs
import com.openautolink.companion.autostart.WifiJobService
import com.openautolink.companion.service.CompanionService
import com.openautolink.companion.ui.theme.OalGreen
import com.openautolink.companion.ui.theme.OalOrange
import com.openautolink.companion.ui.theme.OalRed

@Composable
fun MainScreen(
    onStart: () -> Unit,
    onStop: () -> Unit,
) {
    val context = LocalContext.current
    val prefs = remember {
        context.getSharedPreferences(CompanionPrefs.NAME, Context.MODE_PRIVATE)
    }

    val isRunning by CompanionService.isRunning.collectAsState()
    val isConnected by CompanionService.isConnected.collectAsState()
    val statusText by CompanionService.statusText.collectAsState()

    var autoStartMode by remember {
        mutableIntStateOf(prefs.getInt(CompanionPrefs.AUTO_START_MODE, 0))
    }
    var selectedBtMacs by remember {
        mutableStateOf(prefs.getStringSet(CompanionPrefs.AUTO_START_BT_MACS, emptySet())
            ?: emptySet())
    }
    var stopOnBtDisconnect by remember {
        mutableStateOf(prefs.getBoolean(CompanionPrefs.BT_DISCONNECT_STOP, false))
    }
    var autoReconnect by remember {
        mutableStateOf(prefs.getBoolean(CompanionPrefs.BT_AUTO_RECONNECT, false))
    }
    var wifiSsids by remember {
        mutableStateOf(
            prefs.getStringSet(CompanionPrefs.AUTO_START_WIFI_SSIDS, emptySet())
                ?.joinToString(", ") ?: ""
        )
    }

    fun saveAutoStartMode(mode: Int) {
        autoStartMode = mode
        prefs.edit().putInt(CompanionPrefs.AUTO_START_MODE, mode).apply()

        // Schedule/cancel WiFi monitoring
        if (mode == CompanionPrefs.AUTO_START_WIFI) {
            WifiJobService.schedule(context)
        } else {
            WifiJobService.cancel(context)
        }
    }

    Scaffold { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .padding(horizontal = 20.dp, vertical = 16.dp)
                .verticalScroll(rememberScrollState()),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            // ── Header ─────────────────────────────────────────────
            Spacer(Modifier.height(24.dp))
            Box(
                modifier = Modifier
                    .size(112.dp)
                    .graphicsLayer { compositingStrategy = CompositingStrategy.Offscreen }
                    .drawWithCache {
                        val radius = size.minDimension / 2f
                        val center = Offset(size.width / 2f, size.height / 2f)
                        val bgColor = androidx.compose.ui.graphics.Color(0xFF1565C0)
                        val mask = Brush.radialGradient(
                            colorStops = arrayOf(
                                0.0f to androidx.compose.ui.graphics.Color.Black,
                                0.70f to androidx.compose.ui.graphics.Color.Black,
                                1.0f to androidx.compose.ui.graphics.Color.Transparent,
                            ),
                            center = center,
                            radius = radius,
                        )
                        onDrawWithContent {
                            drawCircle(color = bgColor, radius = radius, center = center)
                            drawContent()
                            drawCircle(brush = mask, radius = radius, center = center, blendMode = BlendMode.DstIn)
                        }
                    },
                contentAlignment = Alignment.Center,
            ) {
                Image(
                    painter = painterResource(id = R.drawable.ic_launcher_foreground),
                    contentDescription = null,
                    modifier = Modifier.size(112.dp),
                )
            }
            Spacer(Modifier.height(8.dp))
            Text(
                text = "OpenAutoLink",
                style = MaterialTheme.typography.headlineMedium,
                fontWeight = FontWeight.Bold,
            )
            Text(
                text = "Companion v${BuildConfig.VERSION_NAME}",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )

            Spacer(Modifier.height(24.dp))

            // ── Status Card ────────────────────────────────────────
            StatusCard(isRunning, isConnected, statusText)

            Spacer(Modifier.height(16.dp))

            // ── Start / Stop ───────────────────────────────────────
            val buttonColor by animateColorAsState(
                targetValue = if (isRunning) OalRed else OalGreen,
                label = "button",
            )
            Button(
                onClick = { if (isRunning) onStop() else onStart() },
                modifier = Modifier.fillMaxWidth().height(56.dp),
                colors = ButtonDefaults.buttonColors(containerColor = buttonColor),
            ) {
                Icon(
                    imageVector = if (isRunning) Icons.Default.Stop else Icons.Default.PlayArrow,
                    contentDescription = null,
                )
                Spacer(Modifier.width(8.dp))
                Text(
                    text = if (isRunning) "Stop" else "Start",
                    style = MaterialTheme.typography.titleMedium,
                )
            }

            Spacer(Modifier.height(28.dp))
            HorizontalDivider()
            Spacer(Modifier.height(20.dp))

            // ── Connection Mode ─────────────────────────────────────
            Text(
                text = "Connection Mode",
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.fillMaxWidth(),
            )
            Spacer(Modifier.height(8.dp))
            var connectionMode by remember {
                mutableStateOf(
                    prefs.getString(
                        CompanionPrefs.CONNECTION_MODE,
                        CompanionPrefs.DEFAULT_CONNECTION_MODE,
                    ) ?: CompanionPrefs.DEFAULT_CONNECTION_MODE
                )
            }
            SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
                SegmentedButton(
                    selected = connectionMode == CompanionPrefs.MODE_PHONE_HOTSPOT,
                    onClick = {
                        connectionMode = CompanionPrefs.MODE_PHONE_HOTSPOT
                        prefs.edit().putString(
                            CompanionPrefs.CONNECTION_MODE,
                            CompanionPrefs.MODE_PHONE_HOTSPOT,
                        ).apply()
                    },
                    shape = SegmentedButtonDefaults.itemShape(0, 2),
                ) { Text("Phone Hotspot") }
                SegmentedButton(
                    selected = connectionMode == CompanionPrefs.MODE_CAR_HOTSPOT,
                    onClick = {
                        connectionMode = CompanionPrefs.MODE_CAR_HOTSPOT
                        prefs.edit().putString(
                            CompanionPrefs.CONNECTION_MODE,
                            CompanionPrefs.MODE_CAR_HOTSPOT,
                        ).apply()
                    },
                    shape = SegmentedButtonDefaults.itemShape(1, 2),
                ) { Text("Car Hotspot") }
            }
            Text(
                text = when (connectionMode) {
                    CompanionPrefs.MODE_PHONE_HOTSPOT ->
                        "This phone hosts the WiFi hotspot. The car connects to this phone."
                    CompanionPrefs.MODE_CAR_HOTSPOT ->
                        "Connect this phone to the car's WiFi hotspot. Multiple phones can be connected at once; the car-side app picks the active phone via the floating switcher button."
                    else -> ""
                },
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(vertical = 4.dp),
            )

            // Phone identity — visible so users know what they look like to
            // the car app. Friendly name is editable; phone_id is read-only.
            Spacer(Modifier.height(12.dp))
            val phoneIdShort = remember {
                CompanionPrefs.getOrCreatePhoneId(prefs).take(8)
            }
            var friendlyName by remember {
                mutableStateOf(CompanionPrefs.getFriendlyName(prefs))
            }
            Text("Phone identity", style = MaterialTheme.typography.labelLarge)
            Spacer(Modifier.height(4.dp))
            androidx.compose.material3.OutlinedTextField(
                value = friendlyName,
                onValueChange = {
                    friendlyName = it
                    prefs.edit().putString(CompanionPrefs.PHONE_FRIENDLY_NAME, it).apply()
                },
                label = { Text("Friendly name") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            Text(
                text = "ID: $phoneIdShort  (auto-generated, used by the car app to remember this phone)",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(vertical = 4.dp),
            )

            Spacer(Modifier.height(20.dp))
            HorizontalDivider()
            Spacer(Modifier.height(20.dp))

            // ── Transport Mode ─────────────────────────────────────
            Text(
                text = "Transport Mode",
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.fillMaxWidth(),
            )
            Spacer(Modifier.height(8.dp))

            var transportMode by remember {
                mutableStateOf(
                    prefs.getString(CompanionPrefs.TRANSPORT_MODE, CompanionPrefs.DEFAULT_TRANSPORT)
                        ?: CompanionPrefs.DEFAULT_TRANSPORT
                )
            }

            // Nearby mode is hidden for now: on GM AAOS the car-side app can't
            // get the system permissions needed to drive the BT→WiFi handoff,
            // so users always have to pre-connect to the phone hotspot anyway.
            // That makes Hotspot mode the only working path. We migrate any
            // existing "nearby" pref to "tcp" here so the service starts in
            // the right mode. Code is kept intact for a future revisit.
            if (transportMode != CompanionPrefs.TRANSPORT_TCP) {
                transportMode = CompanionPrefs.TRANSPORT_TCP
                prefs.edit().putString(CompanionPrefs.TRANSPORT_MODE, CompanionPrefs.TRANSPORT_TCP).apply()
            }

            Text(
                text = "Transport: WiFi Hotspot",
                style = MaterialTheme.typography.bodyMedium,
                modifier = Modifier.padding(vertical = 4.dp),
            )

            /*
            // Disabled — see comment above.
            SingleChoiceSegmentedButtonRow(
                modifier = Modifier.fillMaxWidth(),
            ) {
                SegmentedButton(
                    selected = transportMode == CompanionPrefs.TRANSPORT_TCP,
                    onClick = {
                        transportMode = CompanionPrefs.TRANSPORT_TCP
                        prefs.edit().putString(CompanionPrefs.TRANSPORT_MODE, CompanionPrefs.TRANSPORT_TCP).apply()
                    },
                    shape = SegmentedButtonDefaults.itemShape(0, 2),
                ) { Text("WiFi Hotspot") }
                SegmentedButton(
                    selected = transportMode == CompanionPrefs.TRANSPORT_NEARBY,
                    onClick = {
                        transportMode = CompanionPrefs.TRANSPORT_NEARBY
                        prefs.edit().putString(CompanionPrefs.TRANSPORT_MODE, CompanionPrefs.TRANSPORT_NEARBY).apply()
                    },
                    shape = SegmentedButtonDefaults.itemShape(1, 2),
                ) { Text("Nearby") }
            }
            */

            Text(
                text = "Car connects to this phone's hotspot via TCP. Enable your phone's WiFi hotspot and connect the car to it.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(vertical = 4.dp),
            )

            Text(
                text = "⚠ Restart the service after changing transport mode.",
                style = MaterialTheme.typography.bodySmall,
                color = OalOrange,
                modifier = Modifier.padding(vertical = 2.dp),
            )

            Spacer(Modifier.height(20.dp))
            HorizontalDivider()
            Spacer(Modifier.height(20.dp))

            // ── Auto-Start Section ─────────────────────────────────
            Text(
                text = "Auto-Start",
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.fillMaxWidth(),
            )
            Spacer(Modifier.height(12.dp))

            AutoStartModeSelector(autoStartMode) { saveAutoStartMode(it) }

            Spacer(Modifier.height(16.dp))

            // ── BT Config ──────────────────────────────────────────
            if (autoStartMode == CompanionPrefs.AUTO_START_BT) {
                BtAutoStartConfig(
                    selectedMacs = selectedBtMacs,
                    onMacsChanged = { macs ->
                        selectedBtMacs = macs
                        prefs.edit()
                            .putStringSet(CompanionPrefs.AUTO_START_BT_MACS, macs)
                            .apply()
                    },
                    stopOnDisconnect = stopOnBtDisconnect,
                    onStopOnDisconnectChanged = { v ->
                        stopOnBtDisconnect = v
                        prefs.edit()
                            .putBoolean(CompanionPrefs.BT_DISCONNECT_STOP, v)
                            .apply()
                    },
                    autoReconnect = autoReconnect,
                    onAutoReconnectChanged = { v ->
                        autoReconnect = v
                        prefs.edit()
                            .putBoolean(CompanionPrefs.BT_AUTO_RECONNECT, v)
                            .apply()
                    },
                )
            }

            // ── WiFi Config ────────────────────────────────────────
            if (autoStartMode == CompanionPrefs.AUTO_START_WIFI) {
                WifiAutoStartConfig(
                    ssids = wifiSsids,
                    onSsidsChanged = { text ->
                        wifiSsids = text
                        val ssidSet = text.split(",")
                            .map { it.trim() }
                            .filter { it.isNotBlank() }
                            .toSet()
                        prefs.edit()
                            .putStringSet(CompanionPrefs.AUTO_START_WIFI_SSIDS, ssidSet)
                            .apply()
                    },
                )
            }

            if (autoStartMode == CompanionPrefs.AUTO_START_APP_OPEN) {
                Text(
                    text = "Service will start automatically when the app is opened.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.fillMaxWidth(),
                )
            }

            Spacer(Modifier.height(28.dp))
            HorizontalDivider()
            Spacer(Modifier.height(20.dp))

            // ── File Logging ───────────────────────────────────────
            FileLoggingSection()

            Spacer(Modifier.height(28.dp))
            HorizontalDivider()
            Spacer(Modifier.height(20.dp))

            // ── Deep Link Info ─────────────────────────────────────
            Text(
                text = "Deep Links",
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier.fillMaxWidth(),
            )
            Spacer(Modifier.height(8.dp))
            Text(
                text = "oalcompanion://start — Start advertising\noalcompanion://stop — Stop service",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.fillMaxWidth(),
            )

            Spacer(Modifier.height(32.dp))
        }
    }
}

// ── Status Card ────────────────────────────────────────────────────────

@Composable
private fun StatusCard(isRunning: Boolean, isConnected: Boolean, statusText: String) {
    val statusColor by animateColorAsState(
        targetValue = when {
            isConnected -> OalGreen
            isRunning -> OalOrange
            else -> MaterialTheme.colorScheme.onSurfaceVariant
        },
        label = "status",
    )

    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceContainer,
        ),
    ) {
        Row(
            modifier = Modifier.padding(20.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            if (isRunning && !isConnected) {
                CircularProgressIndicator(
                    modifier = Modifier.size(24.dp),
                    strokeWidth = 2.dp,
                    color = statusColor,
                )
            } else {
                Icon(
                    imageVector = if (isConnected) Icons.Default.CheckCircle
                    else Icons.Default.DirectionsCar,
                    contentDescription = null,
                    modifier = Modifier.size(24.dp),
                    tint = statusColor,
                )
            }
            Spacer(Modifier.width(16.dp))
            Column {
                Text(
                    text = statusText,
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Medium,
                    color = statusColor,
                )
                if (isRunning && !isConnected) {
                    Text(
                        text = "Waiting for car to discover this phone...",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }
    }
}

// ── Auto-Start Mode Selector ───────────────────────────────────────────

@Composable
private fun AutoStartModeSelector(selected: Int, onSelected: (Int) -> Unit) {
    val modes = listOf("Off", "Bluetooth", "WiFi", "App Open")

    SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
        modes.forEachIndexed { index, label ->
            SegmentedButton(
                selected = selected == index,
                onClick = { onSelected(index) },
                shape = SegmentedButtonDefaults.itemShape(index = index, count = modes.size),
                label = { Text(label, style = MaterialTheme.typography.labelSmall) },
            )
        }
    }
}

// ── BT Auto-Start Config ───────────────────────────────────────────────

@SuppressLint("MissingPermission")
@Composable
private fun BtAutoStartConfig(
    selectedMacs: Set<String>,
    onMacsChanged: (Set<String>) -> Unit,
    stopOnDisconnect: Boolean,
    onStopOnDisconnectChanged: (Boolean) -> Unit,
    autoReconnect: Boolean,
    onAutoReconnectChanged: (Boolean) -> Unit,
) {
    val context = LocalContext.current
    var showDevicePicker by remember { mutableStateOf(false) }

    val pairedDevices = remember {
        try {
            val bm = context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
            bm.adapter?.bondedDevices?.map { it.address to (it.name ?: it.address) } ?: emptyList()
        } catch (_: Exception) {
            emptyList()
        }
    }

    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceContainer,
        ),
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            Text(
                text = "Bluetooth Trigger",
                style = MaterialTheme.typography.titleSmall,
                fontWeight = FontWeight.Medium,
            )
            Spacer(Modifier.height(8.dp))

            if (selectedMacs.isEmpty()) {
                Text(
                    text = "No devices selected",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            } else {
                selectedMacs.forEach { mac ->
                    val name = pairedDevices.firstOrNull { it.first == mac }?.second ?: mac
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Icon(
                            Icons.Default.Bluetooth, null,
                            modifier = Modifier.size(16.dp),
                            tint = MaterialTheme.colorScheme.primary,
                        )
                        Spacer(Modifier.width(8.dp))
                        Text(name, style = MaterialTheme.typography.bodyMedium)
                    }
                }
            }

            Spacer(Modifier.height(8.dp))
            TextButton(onClick = { showDevicePicker = true }) {
                Text("Select Devices")
            }

            HorizontalDivider(modifier = Modifier.padding(vertical = 8.dp))

            SwitchRow("Stop on BT disconnect", stopOnDisconnect, onStopOnDisconnectChanged)
            SwitchRow("Auto-reconnect", autoReconnect, onAutoReconnectChanged)
        }
    }

    if (showDevicePicker) {
        BtDevicePickerDialog(
            pairedDevices = pairedDevices,
            selectedMacs = selectedMacs,
            onDismiss = { showDevicePicker = false },
            onConfirm = { macs ->
                onMacsChanged(macs)
                showDevicePicker = false
            },
        )
    }
}

@Composable
private fun BtDevicePickerDialog(
    pairedDevices: List<Pair<String, String>>,
    selectedMacs: Set<String>,
    onDismiss: () -> Unit,
    onConfirm: (Set<String>) -> Unit,
) {
    var selection by remember { mutableStateOf(selectedMacs) }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Select Bluetooth Devices") },
        text = {
            Column {
                if (pairedDevices.isEmpty()) {
                    Text("No paired Bluetooth devices found.")
                } else {
                    pairedDevices.forEach { (mac, name) ->
                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            modifier = Modifier.fillMaxWidth(),
                        ) {
                            Checkbox(
                                checked = selection.contains(mac),
                                onCheckedChange = { checked ->
                                    selection = if (checked) selection + mac
                                    else selection - mac
                                },
                            )
                            Spacer(Modifier.width(8.dp))
                            Text(name)
                        }
                    }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = { onConfirm(selection) }) { Text("OK") }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text("Cancel") }
        },
    )
}

// ── WiFi Auto-Start Config ─────────────────────────────────────────────

@Composable
private fun WifiAutoStartConfig(ssids: String, onSsidsChanged: (String) -> Unit) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceContainer,
        ),
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            Text(
                text = "WiFi Trigger",
                style = MaterialTheme.typography.titleSmall,
                fontWeight = FontWeight.Medium,
            )
            Spacer(Modifier.height(8.dp))
            Icon(
                Icons.Default.Wifi, null,
                modifier = Modifier.size(16.dp),
                tint = MaterialTheme.colorScheme.primary,
            )
            Spacer(Modifier.height(8.dp))
            OutlinedTextField(
                value = ssids,
                onValueChange = onSsidsChanged,
                label = { Text("WiFi network names") },
                placeholder = { Text("HomeWiFi, CarWiFi") },
                modifier = Modifier.fillMaxWidth(),
                singleLine = false,
                maxLines = 3,
            )
            Spacer(Modifier.height(4.dp))
            Text(
                text = "Comma-separated SSIDs. Service starts when phone joins any of these networks.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

// ── Helpers ────────────────────────────────────────────────────────────

@Composable
private fun FileLoggingSection() {
    val context = LocalContext.current
    val isServiceRunning by CompanionService.isRunning.collectAsState()
    val fileLoggingActive by CompanionService.fileLoggingActive.collectAsState()
    val fileLoggingPath by CompanionService.fileLoggingPath.collectAsState()

    Text(
        text = "File Logging",
        style = MaterialTheme.typography.titleMedium,
        fontWeight = FontWeight.SemiBold,
        modifier = Modifier.fillMaxWidth(),
    )
    Spacer(Modifier.height(8.dp))

    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceContainer,
        ),
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Icon(
                        imageVector = Icons.Outlined.Description,
                        contentDescription = null,
                        modifier = Modifier.size(20.dp),
                        tint = if (fileLoggingActive) OalGreen
                        else MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Spacer(Modifier.width(8.dp))
                    Text(
                        text = if (fileLoggingActive) "Logging active" else "Log to file",
                        style = MaterialTheme.typography.bodyMedium,
                    )
                }
                Switch(
                    checked = fileLoggingActive,
                    enabled = true,
                    onCheckedChange = { enabled ->
                        if (enabled) {
                            val running = isServiceRunning
                            val service = getCompanionService(context)
                            if (running && service != null) {
                                service.startFileLogging()
                            } else {
                                // Service not running — piggyback on ACTION_START so
                                // logging starts atomically once the service is up,
                                // avoiding the race between startForegroundService
                                // returning and onCreate completing.
                                val intent = android.content.Intent(context, CompanionService::class.java).apply {
                                    action = CompanionService.ACTION_START
                                    putExtra(CompanionService.EXTRA_START_LOGGING, true)
                                }
                                androidx.core.content.ContextCompat.startForegroundService(context, intent)
                            }
                        } else {
                            getCompanionService(context)?.stopFileLogging()
                        }
                    },
                )
            }

            if (!isServiceRunning && !fileLoggingActive) {
                Text(
                    text = "Toggle on to start the service and begin logging.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }

            if (fileLoggingActive && fileLoggingPath != null) {
                Spacer(Modifier.height(4.dp))
                Text(
                    text = fileLoggingPath!!,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }

            Spacer(Modifier.height(8.dp))
            Text(
                text = "Logs persist as long as this toggle is on, regardless of " +
                    "connection state. If no connection is ever made within 10 minutes " +
                    "of enabling, logging auto-disables to prevent runaway files.\n\n" +
                    "Recommended only for troubleshooting — turn this back off " +
                    "when you're done. Continuous file logging adds I/O overhead " +
                    "and can affect proxy throughput.\n\n" +
                    "Files: Android/data/com.openautolink.companion/files/openautolink/logs/",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

/**
 * Get the CompanionService instance if it's running.
 * Uses [Context.getSystemService]-style lookup via the service connection.
 * Since CompanionService is a started (not bound) service, we access it
 * by sending intents. But for toggle we need the instance directly.
 * We use a simple companion-object reference set in onCreate.
 */
private fun getCompanionService(context: Context): CompanionService? {
    // We'll add a static instance ref to CompanionService
    return CompanionService.instance
}

@Composable
private fun SwitchRow(label: String, checked: Boolean, onCheckedChange: (Boolean) -> Unit) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(label, style = MaterialTheme.typography.bodyMedium)
        Switch(checked = checked, onCheckedChange = onCheckedChange)
    }
}
