package com.openautolink.app.ui.settings

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
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
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.BugReport
import androidx.compose.material.icons.filled.DisplaySettings
import androidx.compose.material.icons.filled.Keyboard
import androidx.compose.material.icons.filled.Mic
import androidx.compose.material.icons.filled.Router
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.VideoSettings
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.FilledTonalIconButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationRail
import androidx.compose.material3.NavigationRailItem
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.key.onKeyEvent
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel

import com.openautolink.app.session.SessionState
import androidx.compose.material3.FilterChip

private enum class SettingsTab(
    val title: String,
    val icon: ImageVector,
) {
    CONNECTION("Connection", Icons.Default.Router),
    DISPLAY("Display", Icons.Default.DisplaySettings),
    VIDEO("Video", Icons.Default.VideoSettings),
    AUDIO("Audio", Icons.Default.Mic),
    INPUT("Input", Icons.Default.Keyboard),
    DIAGNOSTICS("Diagnostics", Icons.Default.BugReport),
}

private data class DisplayModeOption(
    val key: String,
    val label: String,
    val description: String,
)

private val displayModes = listOf(
    DisplayModeOption(
        "system_ui_visible",
        "System UI Visible",
        "Video avoids system bars. Recommended default."
    ),
    DisplayModeOption(
        "fullscreen_immersive",
        "Fullscreen Immersive",
        "Video fills entire screen edge-to-edge."
    ),
)

@Composable
fun SettingsScreen(
    viewModel: SettingsViewModel = viewModel(),
    sessionState: SessionState = SessionState.IDLE,
    onSaveAndConnect: () -> Unit = {},
    onBack: () -> Unit = {},
    onNavigateToDiagnostics: () -> Unit = {},
    onNavigateToSafeAreaEditor: () -> Unit = {},
) {
    val uiState by viewModel.settingsState.collectAsStateWithLifecycle()
    var selectedTab by remember { mutableStateOf(SettingsTab.CONNECTION) }

    Surface(
        modifier = Modifier.fillMaxSize(),
        color = MaterialTheme.colorScheme.background,
    ) {
        Row(
            modifier = Modifier
                .fillMaxSize()
                .windowInsetsPadding(WindowInsets.safeDrawing)
                .testTag("settingsScreen"),
        ) {
            // Left rail — back button + tab icons
            Column(
                modifier = Modifier.fillMaxHeight(),
            ) {
                // Back button
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

                // Tab rail
                NavigationRail(
                    modifier = Modifier.weight(1f),
                    containerColor = MaterialTheme.colorScheme.background,
                ) {
                    Spacer(modifier = Modifier.weight(1f))
                    SettingsTab.entries.forEach { tab ->
                        NavigationRailItem(
                            selected = selectedTab == tab,
                            onClick = {
                                selectedTab = tab
                            },
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

            // Content pane — fills remaining width
            Column(
                modifier = Modifier
                    .fillMaxHeight()
                    .weight(1f)
                    .padding(horizontal = 24.dp, vertical = 16.dp),
            ) {
                // Connection status bar + Save & Connect button — always visible
                ConnectionStatusBar(
                    sessionState = sessionState,
                    onSaveAndConnect = onSaveAndConnect,
                )

                Spacer(modifier = Modifier.height(8.dp))

                // Tab content
                Box(modifier = Modifier.weight(1f)) {
                    when (selectedTab) {
                        SettingsTab.CONNECTION -> ConnectionTab(viewModel, uiState)
                        SettingsTab.DISPLAY -> DisplayTab(
                            viewModel, uiState,
                            onNavigateToSafeAreaEditor,
                        )
                        SettingsTab.VIDEO -> VideoTab(viewModel, uiState)
                        SettingsTab.AUDIO -> AudioTab(viewModel, uiState)
                        SettingsTab.INPUT -> InputTab(viewModel, uiState)
                        SettingsTab.DIAGNOSTICS -> DiagnosticsSettingsTab(
                            viewModel, uiState, onNavigateToDiagnostics
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun ConnectionStatusBar(
    sessionState: SessionState,
    onSaveAndConnect: () -> Unit,
) {
    val statusColor = when (sessionState) {
        SessionState.STREAMING -> Color(0xFF4CAF50) // Green
        SessionState.CONNECTED -> Color(0xFFFF9800) // Orange
        SessionState.CONNECTING -> Color(0xFFFFC107) // Amber
        SessionState.IDLE -> Color(0xFF9E9E9E) // Grey
        SessionState.ERROR -> Color(0xFFF44336) // Red
    }
    val statusText = when (sessionState) {
        SessionState.STREAMING -> "Streaming"
        SessionState.CONNECTED -> "Connected"
        SessionState.CONNECTING -> "Connecting..."
        SessionState.IDLE -> "Disconnected"
        SessionState.ERROR -> "Error"
    }

    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(bottom = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        // Save & Connect button — left-aligned, first element
        Button(
            onClick = onSaveAndConnect,
            modifier = Modifier.testTag("saveAndConnectButton"),
        ) {
            Icon(
                imageVector = Icons.Default.Refresh,
                contentDescription = null,
                modifier = Modifier.size(18.dp),
            )
            Spacer(modifier = Modifier.width(8.dp))
            Text("Save & Restart")
        }

        // Status indicator dot + text
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Box(
                modifier = Modifier
                    .size(12.dp)
                    .background(statusColor, shape = MaterialTheme.shapes.small),
            )
            Text(
                text = statusText,
                style = MaterialTheme.typography.bodyMedium,
                fontWeight = FontWeight.SemiBold,
                color = MaterialTheme.colorScheme.onSurface,
            )
            if (sessionState == SessionState.CONNECTING) {
                CircularProgressIndicator(modifier = Modifier.size(16.dp), strokeWidth = 2.dp)
            }
        }
    }
}

@Composable
private fun ConnectionTab(viewModel: SettingsViewModel, uiState: SettingsUiState) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState()),
    ) {
        // --- Transport Method ---
        SectionHeader("Transport Method")

        Spacer(modifier = Modifier.height(8.dp))

        Row(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            listOf(
                "nearby" to "Google Nearby",
                "hotspot" to "Phone Hotspot",
            ).forEach { (mode, label) ->
                FilterChip(
                    selected = uiState.directTransport == mode,
                    onClick = { viewModel.updateDirectTransport(mode) },
                    label = { Text(label) },
                )
            }
        }

        Text(
            text = when (uiState.directTransport) {
                "nearby" -> "Connect via Google Nearby Connections. Requires Wireless Helper companion app on phone."
                "hotspot" -> "Connect via phone's WiFi hotspot. Enter hotspot SSID/password below. Car must be connected to phone hotspot in WiFi settings."
                else -> ""
            },
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
        )

        Spacer(modifier = Modifier.height(16.dp))

        // --- Hotspot Credentials (hotspot transport only) ---
        if (uiState.directTransport == "hotspot") {
            SectionHeader("Phone Hotspot Credentials")

            Spacer(modifier = Modifier.height(4.dp))

            Text(
                text = "Enter your phone's hotspot SSID and password. These are sent to the phone during the Bluetooth handshake so it knows which WiFi network to use. Leave blank to test without credentials.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
            )

            Spacer(modifier = Modifier.height(8.dp))

            var ssidInput by remember(uiState.hotspotSsid) { mutableStateOf(uiState.hotspotSsid) }
            OutlinedTextField(
                value = ssidInput,
                onValueChange = { ssidInput = it; viewModel.updateHotspotSsid(it) },
                label = { Text("Hotspot SSID") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp),
            )

            Spacer(modifier = Modifier.height(8.dp))

            var pskInput by remember(uiState.hotspotPassword) { mutableStateOf(uiState.hotspotPassword) }
            OutlinedTextField(
                value = pskInput,
                onValueChange = { pskInput = it; viewModel.updateHotspotPassword(it) },
                label = { Text("Hotspot Password") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp),
            )

            Spacer(modifier = Modifier.height(24.dp))

            HorizontalDivider(modifier = Modifier.fillMaxWidth(0.5f))

            Spacer(modifier = Modifier.height(24.dp))
        }
    }
}

@Composable
private fun DisplayTab(
    viewModel: SettingsViewModel,
    uiState: SettingsUiState,
    onNavigateToSafeAreaEditor: () -> Unit,
) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState()),
    ) {
        SectionHeader("Display Mode")

        Spacer(modifier = Modifier.height(4.dp))

        Text(
            text = "Controls how the app interacts with AAOS system bars. " +
                    "Changes apply immediately. On some AAOS head units, the system may " +
                    "prevent apps from hiding bars. To change the video encoding resolution, " +
                    "use AA Resolution on the Video tab.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(bottom = 12.dp)
        )

        displayModes.forEach { mode ->
            Row(
                modifier = Modifier
                    .fillMaxWidth(0.7f)
                    .clickable { viewModel.updateDisplayMode(mode.key) }
                    .padding(vertical = 10.dp)
                    .testTag("displayMode_${mode.key}"),
                verticalAlignment = Alignment.CenterVertically
            ) {
                RadioButton(
                    selected = uiState.displayMode == mode.key,
                    onClick = { viewModel.updateDisplayMode(mode.key) }
                )
                Spacer(modifier = Modifier.width(12.dp))
                Column {
                    Text(
                        text = mode.label,
                        style = MaterialTheme.typography.bodyLarge,
                        fontWeight = if (uiState.displayMode == mode.key) FontWeight.SemiBold else FontWeight.Normal,
                    )
                    Text(
                        text = mode.description,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }

        Spacer(modifier = Modifier.height(24.dp))

        HorizontalDivider(modifier = Modifier.fillMaxWidth(0.7f))

        Spacer(modifier = Modifier.height(24.dp))

        // --- AA Display Insets ---
        SectionHeader("AA Display Insets")

        Spacer(modifier = Modifier.height(4.dp))

        Text(
            text = "Insets the AA projection from the display edges.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier
                .fillMaxWidth(0.7f)
                .padding(bottom = 12.dp)
        )

        Row(
            modifier = Modifier.fillMaxWidth(0.7f),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            FilledTonalButton(
                onClick = onNavigateToSafeAreaEditor,
                modifier = Modifier.testTag("editSafeAreaButton"),
            ) {
                Text("Edit Display Insets")
            }

            Text(
                text = formatInsetLabel(
                    uiState.safeAreaTop, uiState.safeAreaBottom,
                    uiState.safeAreaLeft, uiState.safeAreaRight
                ),
                style = MaterialTheme.typography.bodyMedium,
                color = if (uiState.safeAreaTop > 0 || uiState.safeAreaBottom > 0 ||
                    uiState.safeAreaLeft > 0 || uiState.safeAreaRight > 0)
                    MaterialTheme.colorScheme.primary
                else MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }

        Spacer(modifier = Modifier.height(24.dp))

        HorizontalDivider(modifier = Modifier.fillMaxWidth(0.7f))

        Spacer(modifier = Modifier.height(24.dp))

        // --- Drive Side ---
        Column {
        SectionHeader("Drive Side")

        Spacer(modifier = Modifier.height(4.dp))

        Text(
            text = "Select your driving side. Affects Android Auto UI layout.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(bottom = 12.dp)
        )

        listOf("left" to "Left-Hand Drive (LHD)", "right" to "Right-Hand Drive (RHD)").forEach { (key, label) ->
            Row(
                modifier = Modifier
                    .fillMaxWidth(0.7f)
                    .clickable { viewModel.updateDriveSide(key) }
                    .padding(vertical = 10.dp)
                    .testTag("driveSide_$key"),
                verticalAlignment = Alignment.CenterVertically
            ) {
                RadioButton(
                    selected = uiState.driveSide == key,
                    onClick = { viewModel.updateDriveSide(key) }
                )
                Spacer(modifier = Modifier.width(12.dp))
                Text(
                    text = label,
                    style = MaterialTheme.typography.bodyLarge,
                    fontWeight = if (uiState.driveSide == key) FontWeight.SemiBold else FontWeight.Normal,
                )
            }
        }

        Spacer(modifier = Modifier.height(24.dp))
        } // Column (Drive Side)

        HorizontalDivider(modifier = Modifier.fillMaxWidth(0.7f))

        Spacer(modifier = Modifier.height(24.dp))

        // --- Features ---
        SectionHeader("Features")

        Spacer(modifier = Modifier.height(8.dp))

        // GPS Forwarding
        Row(
            modifier = Modifier
                .fillMaxWidth(0.7f)
                .padding(vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = "GPS Forwarding",
                    style = MaterialTheme.typography.bodyLarge,
                    fontWeight = FontWeight.SemiBold,
                )
                Text(
                    text = "Forward vehicle GPS to Android Auto for navigation.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Switch(
                checked = uiState.gpsForwarding,
                onCheckedChange = { viewModel.updateGpsForwarding(it) },
                modifier = Modifier.testTag("gpsForwardingToggle"),
            )
        }

        // Cluster Navigation
        Row(
            modifier = Modifier
                .fillMaxWidth(0.7f)
                .padding(vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = "Cluster Navigation",
                    style = MaterialTheme.typography.bodyLarge,
                    fontWeight = FontWeight.SemiBold,
                )
                Text(
                    text = "Show turn-by-turn directions on the instrument cluster.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Switch(
                checked = uiState.clusterNavigation,
                onCheckedChange = { viewModel.updateClusterNavigation(it) },
                modifier = Modifier.testTag("clusterNavigationToggle"),
            )
        }

        // IMU Sensors
        Row(
            modifier = Modifier
                .fillMaxWidth(0.7f)
                .padding(vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = "Send IMU Sensors",
                    style = MaterialTheme.typography.bodyLarge,
                    fontWeight = FontWeight.SemiBold,
                )
                Text(
                    text = "Send accelerometer, gyroscope, and compass data to the bridge.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Switch(
                checked = uiState.sendImuSensors,
                onCheckedChange = { viewModel.updateSendImuSensors(it) },
                modifier = Modifier.testTag("sendImuSensorsToggle"),
            )
        }

        Spacer(modifier = Modifier.height(24.dp))

        HorizontalDivider(modifier = Modifier.fillMaxWidth(0.7f))

        Spacer(modifier = Modifier.height(24.dp))

        // --- Overlay Buttons ---
        SectionHeader("Overlay Buttons")

        Spacer(modifier = Modifier.height(4.dp))

        Text(
            text = "Show or hide floating overlay buttons during streaming.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(bottom = 12.dp)
        )

        Row(
            modifier = Modifier
                .fillMaxWidth(0.7f)
                .padding(vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = "Settings Button",
                    style = MaterialTheme.typography.bodyLarge,
                    fontWeight = FontWeight.SemiBold,
                )
                Text(
                    text = "Floating gear icon to access settings during projection.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Switch(
                checked = uiState.overlaySettingsButton,
                onCheckedChange = { viewModel.updateOverlaySettingsButton(it) },
                modifier = Modifier.testTag("overlaySettingsToggle"),
            )
        }

        Row(
            modifier = Modifier
                .fillMaxWidth(0.7f)
                .padding(vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = "Stats Button",
                    style = MaterialTheme.typography.bodyLarge,
                    fontWeight = FontWeight.SemiBold,
                )
                Text(
                    text = "Floating button to toggle performance stats overlay.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Switch(
                checked = uiState.overlayStatsButton,
                onCheckedChange = { viewModel.updateOverlayStatsButton(it) },
                modifier = Modifier.testTag("overlayStatsToggle"),
            )
        }

        Spacer(modifier = Modifier.height(24.dp))

        HorizontalDivider(modifier = Modifier.fillMaxWidth(0.7f))

        Spacer(modifier = Modifier.height(24.dp))

        // --- AA UI Customization ---
        SectionHeader("Android Auto UI")

        Spacer(modifier = Modifier.height(8.dp))

        Row(
            modifier = Modifier
                .fillMaxWidth(0.7f)
                .padding(vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = "Sync AA Theme",
                    style = MaterialTheme.typography.bodyLarge,
                    fontWeight = FontWeight.SemiBold,
                )
                Text(
                    text = "Sync Android Auto day/night theme with the car.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Switch(
                checked = uiState.syncAaTheme,
                onCheckedChange = { viewModel.updateSyncAaTheme(it) },
                modifier = Modifier.testTag("syncAaThemeToggle"),
            )
        }

        // Hide flags
        Column {
        Row(
            modifier = Modifier
                .fillMaxWidth(0.7f)
                .padding(vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = "Hide AA Clock",
                    style = MaterialTheme.typography.bodyLarge,
                    fontWeight = FontWeight.SemiBold,
                )
                Text(
                    text = "Hide Android Auto's clock (AAOS has its own).",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Switch(
                checked = uiState.hideAaClock,
                onCheckedChange = { viewModel.updateHideAaClock(it) },
                modifier = Modifier.testTag("hideAaClockToggle"),
            )
        }

        Row(
            modifier = Modifier
                .fillMaxWidth(0.7f)
                .padding(vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = "Hide Phone Signal",
                    style = MaterialTheme.typography.bodyLarge,
                    fontWeight = FontWeight.SemiBold,
                )
                Text(
                    text = "Hide the phone signal strength indicator from AA status bar.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Switch(
                checked = uiState.hidePhoneSignal,
                onCheckedChange = { viewModel.updateHidePhoneSignal(it) },
                modifier = Modifier.testTag("hidePhoneSignalToggle"),
            )
        }

        Row(
            modifier = Modifier
                .fillMaxWidth(0.7f)
                .padding(vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = "Hide Battery Level",
                    style = MaterialTheme.typography.bodyLarge,
                    fontWeight = FontWeight.SemiBold,
                )
                Text(
                    text = "Hide the phone battery indicator from AA status bar.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Switch(
                checked = uiState.hideBatteryLevel,
                onCheckedChange = { viewModel.updateHideBatteryLevel(it) },
                modifier = Modifier.testTag("hideBatteryLevelToggle"),
            )
        }
        } // Column (hide flags)

        Spacer(modifier = Modifier.height(16.dp))

        SectionHeader("Distance Units")

        Spacer(modifier = Modifier.height(4.dp))

        Text(
            text = "Units for navigation distances shown in the cluster display.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(bottom = 8.dp)
        )

        val distanceUnitOptions = listOf(
            "auto" to "Auto (based on locale)",
            "metric" to "Metric (km, m)",
            "imperial" to "Imperial (mi, ft)",
        )
        distanceUnitOptions.forEach { (key, label) ->
            Row(
                modifier = Modifier
                    .fillMaxWidth(0.7f)
                    .clickable { viewModel.updateDistanceUnits(key) }
                    .padding(vertical = 6.dp)
                    .testTag("distanceUnits_$key"),
                verticalAlignment = Alignment.CenterVertically
            ) {
                RadioButton(
                    selected = uiState.distanceUnits == key,
                    onClick = { viewModel.updateDistanceUnits(key) }
                )
                Spacer(modifier = Modifier.width(12.dp))
                Text(text = label, style = MaterialTheme.typography.bodyLarge)
            }
        }
    }
}

@Composable
private fun VideoTab(viewModel: SettingsViewModel, uiState: SettingsUiState) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState()),
    ) {
        SectionHeader("Video Negotiation")

        Spacer(modifier = Modifier.height(4.dp))

        Text(
            text = "When enabled, the phone picks the best codec and resolution it supports. " +
                    "Disable to manually choose a specific codec and resolution.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(bottom = 12.dp)
        )

        Row(
            modifier = Modifier
                .fillMaxWidth(0.7f)
                .clickable { viewModel.updateVideoAutoNegotiate(!uiState.videoAutoNegotiate) }
                .padding(vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Switch(
                checked = uiState.videoAutoNegotiate,
                onCheckedChange = { viewModel.updateVideoAutoNegotiate(it) }
            )
            Spacer(modifier = Modifier.width(12.dp))
            Text(
                text = if (uiState.videoAutoNegotiate) "Auto (Recommended)" else "Manual",
                style = MaterialTheme.typography.bodyLarge,
                fontWeight = FontWeight.SemiBold,
            )
        }

        if (!uiState.videoAutoNegotiate) {

        Spacer(modifier = Modifier.height(24.dp))

        HorizontalDivider(modifier = Modifier.fillMaxWidth(0.7f))

        Spacer(modifier = Modifier.height(24.dp))

        SectionHeader("Video Codec")

        Spacer(modifier = Modifier.height(4.dp))

        Text(
            text = "Video codec the phone uses to encode the AA stream. " +
                    "H.265 is recommended for 1440p/4K. H.264 may work at higher resolutions " +
                    "depending on your phone.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(bottom = 12.dp)
        )

        val isHighRes = uiState.aaResolution in listOf("1440p", "4k")
        listOf(
            Triple("h264", "H.264", if (isHighRes) "" else " (Recommended)"),
            Triple("h265", "H.265 / HEVC", if (isHighRes) " (Recommended)" else ""),
            Triple("vp9", "VP9", ""),
        ).forEach { (key, label, suffix) ->
            Row(
                modifier = Modifier
                    .fillMaxWidth(0.7f)
                    .clickable { viewModel.updateVideoCodec(key) }
                    .padding(vertical = 10.dp)
                    .testTag("videoCodec_$key"),
                verticalAlignment = Alignment.CenterVertically
            ) {
                RadioButton(
                    selected = uiState.videoCodec == key,
                    onClick = { viewModel.updateVideoCodec(key) }
                )
                Spacer(modifier = Modifier.width(12.dp))
                Text(
                    text = label + suffix,
                    style = MaterialTheme.typography.bodyLarge,
                    fontWeight = if (uiState.videoCodec == key) FontWeight.SemiBold else FontWeight.Normal,
                )
            }
        }

        Spacer(modifier = Modifier.height(24.dp))

        HorizontalDivider(modifier = Modifier.fillMaxWidth(0.7f))

        Spacer(modifier = Modifier.height(24.dp))

        // --- Frame Rate ---
        SectionHeader("Frame Rate")

        Spacer(modifier = Modifier.height(4.dp))

        Text(
            text = "Video frame rate the phone encodes at. Most phones support 60 FPS at 1080p.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(bottom = 12.dp)
        )

        listOf(30 to "30 FPS", 60 to "60 FPS").forEach { (fps, label) ->
            Row(
                modifier = Modifier
                    .fillMaxWidth(0.7f)
                    .clickable { viewModel.updateVideoFps(fps) }
                    .padding(vertical = 10.dp)
                    .testTag("videoFps_$fps"),
                verticalAlignment = Alignment.CenterVertically
            ) {
                RadioButton(
                    selected = uiState.videoFps == fps,
                    onClick = { viewModel.updateVideoFps(fps) }
                )
                Spacer(modifier = Modifier.width(12.dp))
                Text(
                    text = label,
                    style = MaterialTheme.typography.bodyLarge,
                    fontWeight = if (uiState.videoFps == fps) FontWeight.SemiBold else FontWeight.Normal,
                )
            }
        }

        Spacer(modifier = Modifier.height(24.dp))

        HorizontalDivider(modifier = Modifier.fillMaxWidth(0.7f))

        Spacer(modifier = Modifier.height(24.dp))

        // --- Resolution Tier ---
        SectionHeader("AA Resolution")

        Spacer(modifier = Modifier.height(4.dp))

        Text(
            text = "Resolution tier the phone encodes at. Higher = better quality, more bandwidth. " +
                    "H.265 or VP9 recommended for 1440p/4K. Phone AA developer settings may need " +
                    "to be enabled for resolutions above 1080p.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(bottom = 12.dp)
        )

        listOf(
            Triple("480p", "480p (800×480)", false),
            Triple("720p", "720p (1280×720)", false),
            Triple("1080p", "1080p (1920×1080)", false),
            Triple("1440p", "1440p (2560×1440)", true),
            Triple("4k", "4K (3840×2160)", true),
        ).forEach { (key, label, isHighRes) ->
            val warningColor = Color(0xFFFFB74D)
            Row(
                modifier = Modifier
                    .fillMaxWidth(0.7f)
                    .clickable { viewModel.updateAaResolution(key) }
                    .padding(vertical = 10.dp)
                    .testTag("aaResolution_$key"),
                verticalAlignment = Alignment.CenterVertically
            ) {
                RadioButton(
                    selected = uiState.aaResolution == key,
                    onClick = { viewModel.updateAaResolution(key) }
                )
                Spacer(modifier = Modifier.width(12.dp))
                Column {
                    Text(
                        text = label,
                        style = MaterialTheme.typography.bodyLarge,
                        fontWeight = if (uiState.aaResolution == key) FontWeight.SemiBold else FontWeight.Normal,
                        color = if (isHighRes) warningColor else Color.Unspecified,
                    )
                    if (isHighRes) {
                        Text(
                            text = "AA Developer Mode required on phone",
                            style = MaterialTheme.typography.bodySmall,
                            color = warningColor,
                        )
                    }
                }
            }
        }

        } // end if (!videoAutoNegotiate)

        Spacer(modifier = Modifier.height(24.dp))

        HorizontalDivider(modifier = Modifier.fillMaxWidth(0.7f))

        Spacer(modifier = Modifier.height(24.dp))

        // --- DPI ---
        SectionHeader("AA Display Density (DPI)")

        Spacer(modifier = Modifier.height(4.dp))

        val recommendedDpi = when (uiState.aaResolution) {
            "480p" -> 80
            "720p" -> 107
            "1440p" -> 213
            "4k" -> 320
            else -> 160
        }

        Text(
            text = "Controls how Android Auto sizes its UI elements. " +
                    "160 is standard for 1080p. For higher resolutions, scale DPI proportionally " +
                    "to keep UI elements the same visual size (e.g. 320 for 4K). " +
                    "Lower = more content, smaller controls. Higher = bigger controls, less content.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(bottom = 12.dp)
        )

        if (!uiState.videoAutoNegotiate && uiState.aaResolution != "1080p" && uiState.aaDpi != recommendedDpi) {
            Text(
                text = "Recommended for ${uiState.aaResolution}: $recommendedDpi DPI (tap to apply)",
                style = MaterialTheme.typography.bodySmall,
                color = Color(0xFF64B5F6),
                modifier = Modifier
                    .padding(bottom = 8.dp)
                    .clickable { viewModel.updateAaDpi(recommendedDpi) }
            )
        }

        // Slider for custom DPI (80-400 range)
        var sliderDpi by remember(uiState.aaDpi) { mutableIntStateOf(uiState.aaDpi) }
        Text(
            text = "DPI: $sliderDpi",
            style = MaterialTheme.typography.titleMedium,
            fontWeight = FontWeight.SemiBold,
            modifier = Modifier.padding(bottom = 4.dp)
        )
        Slider(
            value = sliderDpi.toFloat(),
            onValueChange = { sliderDpi = it.toInt() },
            onValueChangeFinished = { viewModel.updateAaDpi(sliderDpi) },
            valueRange = 80f..400f,
            steps = 0,
            modifier = Modifier.fillMaxWidth(0.7f)
        )

        // Quick presets — laid out as chips, not aligned to slider
        Row(
            modifier = Modifier
                .fillMaxWidth(0.7f)
                .padding(top = 4.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            listOf(120, 160, 200, 240, 320).forEach { preset ->
                val isSelected = uiState.aaDpi == preset
                Surface(
                    shape = MaterialTheme.shapes.small,
                    color = if (isSelected) MaterialTheme.colorScheme.primary
                            else MaterialTheme.colorScheme.surfaceVariant,
                    modifier = Modifier.clickable {
                        sliderDpi = preset
                        viewModel.updateAaDpi(preset)
                    }
                ) {
                    Text(
                        text = "$preset",
                        style = MaterialTheme.typography.bodySmall,
                        fontWeight = if (isSelected) FontWeight.Bold else FontWeight.Normal,
                        color = if (isSelected) MaterialTheme.colorScheme.onPrimary
                                else MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(horizontal = 10.dp, vertical = 6.dp)
                    )
                }
            }
        }

        Spacer(modifier = Modifier.height(24.dp))

        HorizontalDivider(modifier = Modifier.fillMaxWidth(0.7f))

        Spacer(modifier = Modifier.height(24.dp))

        // --- Video Scaling Mode ---
        SectionHeader("Video Scaling")

        Spacer(modifier = Modifier.height(4.dp))

        Text(
            text = "How the video fits your screen. " +
                    "Letterbox shows the full frame with black bars on the sides. " +
                    "Crop fills the screen but cuts off top/bottom. " +
                    "Requires reconnect (Save & Restart).",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(bottom = 12.dp)
        )

        listOf(
            "letterbox" to "Letterbox (no crop, black bars on sides)",
            "crop" to "Fill screen (set Pixel Aspect for correct proportions)",
        ).forEach { (key, label) ->
            Row(
                modifier = Modifier
                    .fillMaxWidth(0.7f)
                    .clickable { viewModel.updateVideoScalingMode(key) }
                    .padding(vertical = 10.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                RadioButton(
                    selected = uiState.videoScalingMode == key,
                    onClick = { viewModel.updateVideoScalingMode(key) }
                )
                Spacer(modifier = Modifier.width(12.dp))
                Text(
                    text = label,
                    style = MaterialTheme.typography.bodyLarge,
                    fontWeight = if (uiState.videoScalingMode == key) FontWeight.SemiBold else FontWeight.Normal,
                )
            }
        }

        Spacer(modifier = Modifier.height(24.dp))

        HorizontalDivider(modifier = Modifier.fillMaxWidth(0.7f))

        Spacer(modifier = Modifier.height(24.dp))

        // --- AA Video Margins ---
        SectionHeader("AA Video Margins")

        Spacer(modifier = Modifier.height(4.dp))

        Text(
            text = "Override how AA layouts its UI within the video frame. " +
                    "0 = auto-computed from your display aspect ratio. " +
                    "Use these to adjust if AA buttons appear behind letterbox bars or display curves.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(bottom = 12.dp)
        )

        // Width Margin
        Row(
            modifier = Modifier
                .fillMaxWidth(0.7f)
                .padding(vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                text = "Width Margin",
                style = MaterialTheme.typography.bodyLarge,
                modifier = Modifier.width(140.dp),
            )
            OutlinedTextField(
                value = if (uiState.aaWidthMargin == 0) "" else uiState.aaWidthMargin.toString(),
                onValueChange = { value ->
                    val margin = value.filter { it.isDigit() }.toIntOrNull() ?: 0
                    viewModel.updateAaWidthMargin(margin.coerceIn(0, 1000))
                },
                placeholder = { Text("0 (auto)") },
                singleLine = true,
                modifier = Modifier.width(120.dp).testTag("aaWidthMargin"),
            )
            Spacer(modifier = Modifier.width(8.dp))
            Text("px", style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }

        // Height Margin
        Row(
            modifier = Modifier
                .fillMaxWidth(0.7f)
                .padding(vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                text = "Height Margin",
                style = MaterialTheme.typography.bodyLarge,
                modifier = Modifier.width(140.dp),
            )
            OutlinedTextField(
                value = if (uiState.aaHeightMargin == 0) "" else uiState.aaHeightMargin.toString(),
                onValueChange = { value ->
                    val margin = value.filter { it.isDigit() }.toIntOrNull() ?: 0
                    viewModel.updateAaHeightMargin(margin.coerceIn(0, 1000))
                },
                placeholder = { Text("0 (auto)") },
                singleLine = true,
                modifier = Modifier.width(120.dp).testTag("aaHeightMargin"),
            )
            Spacer(modifier = Modifier.width(8.dp))
            Text("px", style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }

        Spacer(modifier = Modifier.height(24.dp))

        HorizontalDivider(modifier = Modifier.fillMaxWidth(0.7f))

        Spacer(modifier = Modifier.height(24.dp))

        // --- Pixel Aspect Ratio ---
        SectionHeader("Pixel Aspect Ratio")

        Spacer(modifier = Modifier.height(4.dp))

        Text(
            text = "Compensates for wide displays (non-16:9). 0 = auto-compute from your display's " +
                    "aspect ratio vs video aspect ratio. When AA renders into a letterboxed area, " +
                    "pixel aspect pre-distorts the layout so circles stay circular. " +
                    "Value is in 1/10000 units (e.g. 14454 ≈ 1.45:1 for the Blazer EV's 2914×1134 display at 1080p).",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(bottom = 12.dp)
        )

        Row(
            modifier = Modifier
                .fillMaxWidth(0.7f)
                .padding(vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                text = "Pixel Aspect (×10⁴)",
                style = MaterialTheme.typography.bodyLarge,
                modifier = Modifier.width(180.dp),
            )
            OutlinedTextField(
                value = if (uiState.aaPixelAspect == 0) "" else uiState.aaPixelAspect.toString(),
                onValueChange = { value ->
                    val pa = value.filter { it.isDigit() }.toIntOrNull() ?: 0
                    viewModel.updateAaPixelAspect(pa.coerceIn(0, 30000))
                },
                placeholder = { Text("0 (auto)") },
                singleLine = true,
                modifier = Modifier.width(140.dp).testTag("aaPixelAspect"),
            )
        }

        Spacer(modifier = Modifier.height(16.dp))

        Text(
            text = "Changes to video settings require an AA session restart (Save & Restart).",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

// --- Input Tab: Key Remapping ---

/** AA actions that can be mapped to physical keys. */
private data class MappableAction(
    val aaKeycode: Int,
    val label: String,
    val description: String,
)

private val mappableActions = listOf(
    MappableAction(85, "Play / Pause", "Toggle media playback"),
    MappableAction(87, "Next Track", "Skip to next track"),
    MappableAction(88, "Previous Track", "Go to previous track"),
    MappableAction(86, "Stop", "Stop media playback"),
    MappableAction(126, "Play", "Start media playback"),
    MappableAction(127, "Pause", "Pause media playback"),
    MappableAction(84, "Voice Assistant", "Activate Google Assistant"),
    MappableAction(19, "DPAD Up", "Navigate up"),
    MappableAction(20, "DPAD Down", "Navigate down"),
    MappableAction(21, "DPAD Left", "Navigate left"),
    MappableAction(22, "DPAD Right", "Navigate right"),
    MappableAction(23, "DPAD Center", "Select / Confirm"),
    MappableAction(5, "Call", "Answer phone call"),
    MappableAction(6, "End Call", "Hang up phone call"),
)

@Composable
private fun InputTab(viewModel: SettingsViewModel, uiState: SettingsUiState) {
    // Parse current key map from JSON
    val currentMap = remember(uiState.keyRemap) {
        if (uiState.keyRemap.isBlank()) emptyMap()
        else try {
            val json = org.json.JSONObject(uiState.keyRemap)
            buildMap { for (key in json.keys()) put(key.toInt(), json.getInt(key)) }
        } catch (_: Exception) { emptyMap() }
    }

    // Reverse map: AA keycode → list of hardware keycodes mapped to it
    val reverseMap = remember(currentMap) {
        val rev = mutableMapOf<Int, MutableList<Int>>()
        for ((hwKey, aaKey) in currentMap) {
            rev.getOrPut(aaKey) { mutableListOf() }.add(hwKey)
        }
        rev
    }

    // Key capture dialog state
    var captureTarget by remember { mutableStateOf<MappableAction?>(null) }
    var lastDetectedKey by remember { mutableStateOf<Pair<Int, String>?>(null) }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState()),
    ) {
        SectionHeader("Key Remapping")

        Spacer(modifier = Modifier.height(4.dp))

        Text(
            text = "Map physical buttons (steering wheel, remote) to Android Auto actions. " +
                    "Tap an action, then press the physical button you want to assign. " +
                    "Requires Save & Restart.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(bottom = 12.dp)
        )

        // Last detected key (diagnostic)
        if (lastDetectedKey != null) {
            Surface(
                color = MaterialTheme.colorScheme.tertiaryContainer,
                shape = MaterialTheme.shapes.small,
                modifier = Modifier.fillMaxWidth(0.7f).padding(bottom = 12.dp),
            ) {
                Text(
                    "Last key: ${lastDetectedKey!!.second} (${lastDetectedKey!!.first})",
                    modifier = Modifier.padding(8.dp),
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }

        // List of mappable actions
        mappableActions.forEach { action ->
            val boundKeys = reverseMap[action.aaKeycode] ?: emptyList()
            val boundLabel = if (boundKeys.isNotEmpty()) {
                boundKeys.joinToString(", ") { code ->
                    android.view.KeyEvent.keyCodeToString(code)
                        .removePrefix("KEYCODE_")
                }
            } else "Not mapped"

            Row(
                modifier = Modifier
                    .fillMaxWidth(0.7f)
                    .clickable { captureTarget = action }
                    .padding(vertical = 8.dp, horizontal = 4.dp)
                    .testTag("keyMap_${action.aaKeycode}"),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.SpaceBetween,
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        action.label,
                        style = MaterialTheme.typography.bodyLarge,
                        fontWeight = FontWeight.Medium,
                    )
                    Text(
                        boundLabel,
                        style = MaterialTheme.typography.bodySmall,
                        color = if (boundKeys.isNotEmpty()) MaterialTheme.colorScheme.primary
                               else MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                Text(
                    "Tap to assign",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.outline,
                )
            }
            HorizontalDivider(modifier = Modifier.fillMaxWidth(0.7f))
        }

        Spacer(modifier = Modifier.height(16.dp))

        // Reset all mappings button
        if (currentMap.isNotEmpty()) {
            FilledTonalButton(
                onClick = { viewModel.updateKeyRemap("") },
                modifier = Modifier.testTag("resetKeyMap"),
            ) {
                Text("Reset All Mappings")
            }
        }

        Spacer(modifier = Modifier.height(24.dp))
    }

    // Key capture dialog
    if (captureTarget != null) {
        val target = captureTarget!!
        val focusRequester = remember { androidx.compose.ui.focus.FocusRequester() }
        LaunchedEffect(Unit) { focusRequester.requestFocus() }

        androidx.compose.material3.AlertDialog(
            onDismissRequest = { captureTarget = null },
            title = { Text("Assign key to: ${target.label}") },
            text = {
                Column(
                    modifier = Modifier
                        .focusRequester(focusRequester)
                        .onKeyEvent { event ->
                            if (event.nativeKeyEvent.action == android.view.KeyEvent.ACTION_UP) {
                                val code = event.nativeKeyEvent.keyCode
                                val name = android.view.KeyEvent.keyCodeToString(code)
                                    .removePrefix("KEYCODE_")
                                lastDetectedKey = code to name
                            }
                            true // consume all key events
                        }
                ) {
                    Text("Press any physical button (steering wheel, remote, keyboard) now.")
                    Spacer(modifier = Modifier.height(8.dp))
                    Text(
                        "The next key press will be mapped to ${target.label} (AA keycode ${target.aaKeycode}).",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    if (lastDetectedKey != null) {
                        Spacer(modifier = Modifier.height(12.dp))
                        Text(
                            "Detected: ${lastDetectedKey!!.second} (${lastDetectedKey!!.first})",
                            style = MaterialTheme.typography.bodyMedium,
                            fontWeight = FontWeight.Bold,
                            color = MaterialTheme.colorScheme.primary,
                        )
                    }
                }
            },
            confirmButton = {
                if (lastDetectedKey != null) {
                    Button(onClick = {
                        val hwKey = lastDetectedKey!!.first
                        val newMap = currentMap.toMutableMap()
                        // Remove any existing mapping for this hardware key
                        newMap.keys.removeAll { it == hwKey }
                        newMap[hwKey] = target.aaKeycode
                        val json = org.json.JSONObject(newMap.mapKeys { it.key.toString() }).toString()
                        viewModel.updateKeyRemap(json)
                        captureTarget = null
                        lastDetectedKey = null
                    }) {
                        Text("Assign ${lastDetectedKey!!.second}")
                    }
                }
            },
            dismissButton = {
                Button(onClick = { captureTarget = null }) {
                    Text("Cancel")
                }
            },
        )
    }
}

@Composable
private fun AudioTab(viewModel: SettingsViewModel, uiState: SettingsUiState) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState()),
    ) {
        // --- Microphone Source ---
        SectionHeader("Microphone Source")

        Spacer(modifier = Modifier.height(4.dp))

        Text(
            text = "Choose which microphone is used for voice assistant " +
                    "and phone calls during Android Auto.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(bottom = 12.dp)
        )

        micSourceOptions.forEach { option ->
            Row(
                modifier = Modifier
                    .fillMaxWidth(0.7f)
                    .clickable { viewModel.updateMicSource(option.key) }
                    .padding(vertical = 10.dp)
                    .testTag("micSource_${option.key}"),
                verticalAlignment = Alignment.CenterVertically
            ) {
                RadioButton(
                    selected = uiState.micSource == option.key,
                    onClick = { viewModel.updateMicSource(option.key) }
                )
                Spacer(modifier = Modifier.width(12.dp))
                Column {
                    Text(
                        text = option.label,
                        style = MaterialTheme.typography.bodyLarge,
                        fontWeight = if (uiState.micSource == option.key) FontWeight.SemiBold else FontWeight.Normal,
                    )
                    Text(
                        text = option.description,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }

        Spacer(modifier = Modifier.height(24.dp))

        HorizontalDivider(modifier = Modifier.fillMaxWidth(0.7f))

        Spacer(modifier = Modifier.height(24.dp))

        // --- Per-Purpose Volume Offsets ---
        SectionHeader("Volume Offsets")

        Spacer(modifier = Modifier.height(4.dp))

        Text(
            text = "Adjust relative volume for each audio purpose. " +
                    "0 = default. Positive = louder, negative = quieter. " +
                    "Requires Save & Restart.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(bottom = 12.dp)
        )

        VolumeOffsetSlider(
            label = "Media",
            value = uiState.volumeOffsetMedia,
            onValueChange = { viewModel.updateVolumeOffsetMedia(it) }
        )
        VolumeOffsetSlider(
            label = "Navigation",
            value = uiState.volumeOffsetNavigation,
            onValueChange = { viewModel.updateVolumeOffsetNavigation(it) }
        )
        VolumeOffsetSlider(
            label = "Assistant / Speech",
            value = uiState.volumeOffsetAssistant,
            onValueChange = { viewModel.updateVolumeOffsetAssistant(it) }
        )

        Spacer(modifier = Modifier.height(24.dp))
    }
}

@Composable
private fun VolumeOffsetSlider(
    label: String,
    value: Int,
    onValueChange: (Int) -> Unit,
) {
    Column(modifier = Modifier.fillMaxWidth(0.7f).padding(vertical = 4.dp)) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
        ) {
            Text(label, style = MaterialTheme.typography.bodyLarge)
            val display = if (value >= 0) "+$value%" else "$value%"
            Text(
                display,
                style = MaterialTheme.typography.bodyMedium,
                fontWeight = FontWeight.SemiBold,
                color = if (value == 0) MaterialTheme.colorScheme.onSurfaceVariant
                       else MaterialTheme.colorScheme.primary,
            )
        }
        Slider(
            value = value.toFloat(),
            onValueChange = { onValueChange(it.toInt()) },
            valueRange = -100f..100f,
            steps = 19, // 10% increments
            modifier = Modifier.testTag("volumeOffset_$label"),
        )
    }
}

private data class MicSourceOption(
    val key: String,
    val label: String,
    val description: String,
)

private val micSourceOptions = listOf(
    MicSourceOption(
        "car",
        "Car Microphone",
        "Uses the head unit's built-in microphone. Best for in-car voice control."
    ),
    MicSourceOption(
        "phone",
        "Phone Microphone",
        "Uses the phone's microphone via Bluetooth. Useful if the car mic is poor quality."
    ),
)

@Composable
private fun SettingRow(label: String, value: String) {
    Row(
        modifier = Modifier
            .fillMaxWidth(0.5f)
            .padding(vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.bodyLarge,
            modifier = Modifier.weight(1f),
        )
        Text(
            text = value,
            style = MaterialTheme.typography.bodyLarge,
            fontWeight = FontWeight.SemiBold,
            color = MaterialTheme.colorScheme.primary,
        )
    }
}

@Composable
private fun SectionHeader(title: String) {
    Text(
        text = title,
        style = MaterialTheme.typography.headlineSmall,
        color = MaterialTheme.colorScheme.primary,
        fontWeight = FontWeight.SemiBold,
        modifier = Modifier.padding(vertical = 8.dp)
    )
}

/**
 * Format inset values as readable text, showing only non-zero sides.
 */
private fun formatInsetLabel(top: Int, bottom: Int, left: Int, right: Int): String {
    val parts = mutableListOf<String>()
    if (top > 0) parts += "Top: ${top}px"
    if (bottom > 0) parts += "Bottom: ${bottom}px"
    if (left > 0) parts += "Left: ${left}px"
    if (right > 0) parts += "Right: ${right}px"
    return parts.joinToString(", ")
}

private val logLevelOptions = listOf("DEBUG", "INFO", "WARN", "ERROR")

@Composable
private fun DiagnosticsSettingsTab(
    viewModel: SettingsViewModel,
    uiState: SettingsUiState,
    onNavigateToDiagnostics: () -> Unit,
) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState()),
    ) {
        SectionHeader("Diagnostics")

        Spacer(modifier = Modifier.height(4.dp))

        Text(
            text = "View structured logs and telemetry in the diagnostics dashboard.",
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(bottom = 12.dp),
        )

        Spacer(modifier = Modifier.height(16.dp))

        // Link to full diagnostics screen
        FilledTonalButton(
            onClick = onNavigateToDiagnostics,
            modifier = Modifier.testTag("openDiagnosticsButton"),
        ) {
            Icon(
                imageVector = Icons.Default.BugReport,
                contentDescription = null,
                modifier = Modifier.size(18.dp),
            )
            Spacer(modifier = Modifier.width(8.dp))
            Text("Open Diagnostics Dashboard")
        }
    }
}
