package com.openautolink.companion.ui

import android.annotation.SuppressLint
import android.bluetooth.BluetoothManager
import android.content.Context
import androidx.compose.animation.animateColorAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Bluetooth
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.DirectionsCar
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material.icons.filled.Wifi
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
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
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
            Icon(
                imageVector = Icons.Default.DirectionsCar,
                contentDescription = null,
                modifier = Modifier.size(48.dp),
                tint = MaterialTheme.colorScheme.primary,
            )
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

            Text(
                text = if (transportMode == CompanionPrefs.TRANSPORT_TCP)
                    "Car connects to this phone's hotspot via TCP. Enable your phone's WiFi hotspot and connect the car to it."
                else
                    "Car discovers this phone via Google Nearby Connections (Bluetooth → WiFi Direct).",
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
