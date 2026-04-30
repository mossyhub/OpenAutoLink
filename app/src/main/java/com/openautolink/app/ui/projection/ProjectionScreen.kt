package com.openautolink.app.ui.projection

import android.annotation.SuppressLint
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.FiberManualRecord
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.PhoneAndroid
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color

import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalView
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import com.openautolink.app.R
import com.openautolink.app.audio.AudioStats
import com.openautolink.app.session.SessionState
import com.openautolink.app.ui.components.DraggableOverlayButton
import com.openautolink.app.video.VideoStats

@Composable
fun ProjectionScreen(
    viewModel: ProjectionViewModel = viewModel(),
    onNavigateToSettings: () -> Unit = {},
    settingsOverlay: @Composable (onBack: () -> Unit) -> Unit = {},
) {
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()

    // Settings overlay state
    var showSettings by rememberSaveable { mutableStateOf(false) }

    LaunchedEffect(showSettings) {
        viewModel.setSettingsOpen(showSettings)
    }

    DisposableEffect(Unit) {
        viewModel.connect()
        onDispose {
            // Don't disconnect on dispose — session survives config changes
        }
    }

    // Compute explicit padding from system bar AND display cutout dimensions.
    // Use getInsetsIgnoringVisibility() because AAOS CarSystemUI may not update
    // inset values when bars are requested to hide via WindowInsetsController.
    // Display cutout insets represent physically curved/missing screen areas.
    val view = LocalView.current
    val rootInsets = view.rootWindowInsets
    val sysBarInsets = rootInsets?.getInsetsIgnoringVisibility(
        android.view.WindowInsets.Type.systemBars()
    )
    val cutoutInsets = rootInsets?.getInsetsIgnoringVisibility(
        android.view.WindowInsets.Type.displayCutout()
    )
    val barTop = sysBarInsets?.top ?: 0
    val barBottom = sysBarInsets?.bottom ?: 0
    val barLeft = sysBarInsets?.left ?: 0
    val barRight = sysBarInsets?.right ?: 0
    val cutTop = cutoutInsets?.top ?: 0
    val cutBottom = cutoutInsets?.bottom ?: 0
    val cutLeft = cutoutInsets?.left ?: 0
    val cutRight = cutoutInsets?.right ?: 0

    val density = LocalDensity.current
    val displayModePadding = with(density) {
        when (uiState.displayMode) {
            // System UI visible — pad for status bar (top), nav bar (bottom), and cutouts.
            "system_ui_visible" -> androidx.compose.foundation.layout.PaddingValues(
                top = maxOf(barTop, cutTop).toDp(),
                bottom = maxOf(barBottom, cutBottom).toDp(),
                start = maxOf(barLeft, cutLeft).toDp(),
                end = maxOf(barRight, cutRight).toDp()
            )
            // Fullscreen — no padding. Video fills entire framebuffer.
            // AA stable_insets (right only) keep buttons away from the curve.
            "fullscreen_immersive" -> androidx.compose.foundation.layout.PaddingValues()
            else -> androidx.compose.foundation.layout.PaddingValues()
        }
    }

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.Black)
            .padding(displayModePadding)
            .testTag("projectionScreen")
    ) {
        // Crop: fillMaxSize — video fills entire surface, pixel_aspect tells AA to
        // pre-distort UI so stretched pixels render correctly on wide displays.
        // Letterbox: constrain to 16:9, no stretching needed.
        val surfaceModifier = if (uiState.videoScalingMode == "crop") {
            Modifier
                .fillMaxSize()
                .testTag("projectionSurface")
        } else {
            Modifier
                .align(Alignment.Center)
                .aspectRatio(16f / 9f, matchHeightConstraintsFirst = true)
                .testTag("projectionSurface")
        }

        // SurfaceView for video rendering — intercepts touch for forwarding to bridge
        // Key on videoScalingMode so Compose recreates the SurfaceView when mode changes.
        // Without this, the old SurfaceView keeps its dimensions after switching modes.
        @SuppressLint("ClickableViewAccessibility")
        key(uiState.videoScalingMode, uiState.displayMode) {
        AndroidView(
            factory = { context ->
                SurfaceView(context).apply {
                    holder.addCallback(object : SurfaceHolder.Callback {
                        override fun surfaceCreated(holder: SurfaceHolder) {
                            viewModel.onSurfaceAvailable(
                                holder.surface,
                                holder.surfaceFrame.width(),
                                holder.surfaceFrame.height()
                            )
                        }

                        override fun surfaceChanged(
                            holder: SurfaceHolder,
                            format: Int,
                            width: Int,
                            height: Int
                        ) {
                            viewModel.onSurfaceAvailable(holder.surface, width, height)
                        }

                        override fun surfaceDestroyed(holder: SurfaceHolder) {
                            viewModel.onSurfaceDestroyed()
                        }
                    })

                    setOnTouchListener { v, event ->
                        // Don't forward touch to AA while settings overlay is open
                        if (!showSettings) {
                            viewModel.onTouchEvent(event, v.width, v.height)
                        }
                        true
                    }
                }
            },
            modifier = surfaceModifier
        )
        }

        // Connection status HUD — centered overlay, visible when not streaming
        if (uiState.sessionState != SessionState.STREAMING) {
            ConnectionHud(
                uiState = uiState,
                modifier = Modifier.align(Alignment.Center)
            )
        }

        // Waiting-for-keyframe indicator — subtle spinner during STREAMING when
        // the decoder has no IDR yet (black video). Disappears as soon as first
        // frame is decoded. Placed bottom-center to avoid blocking the projection.
        if (uiState.sessionState == SessionState.STREAMING && uiState.videoStats.waitingForKeyframe) {
            Box(
                modifier = Modifier
                    .align(Alignment.BottomCenter)
                    .padding(bottom = 48.dp)
                    .clip(RoundedCornerShape(12.dp))
                    .background(Color(0x99000000))
                    .padding(horizontal = 20.dp, vertical = 12.dp)
                    .testTag("keyframeWaitIndicator"),
                contentAlignment = Alignment.Center
            ) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(12.dp)
                ) {
                    CircularProgressIndicator(
                        modifier = Modifier.size(20.dp),
                        color = Color.White,
                        strokeWidth = 2.dp
                    )
                    Text(
                        text = "Loading video\u2026",
                        style = MaterialTheme.typography.bodyMedium,
                        color = Color.White
                    )
                }
            }
        }

        // Floating overlay buttons — bottom-right, above nav bar. Draggable.
        Column(
            modifier = Modifier
                .align(Alignment.BottomEnd)
                .padding(end = 16.dp, bottom = 16.dp),
        ) {
            // Settings button — draggable
            DraggableOverlayButton(
                icon = Icons.Default.Settings,
                contentDescription = "Settings",
                onClick = { showSettings = true },
                positionKey = "overlay_settings",
                modifier = Modifier.testTag("settingsButton"),
            )

            Spacer(modifier = Modifier.height(8.dp))

            // Switch Phone button — hidden for now; multi-phone UX is incomplete
            // and the bridge-mode switching flow doesn't apply to the direct/JNI
            // path. Re-enable once a single coherent flow exists.
            // DraggableOverlayButton(
            //     icon = Icons.Default.PhoneAndroid,
            //     contentDescription = "Switch Phone",
            //     onClick = { viewModel.showPhoneChooser() },
            //     positionKey = "overlay_switch_phone",
            //     modifier = Modifier.testTag("switchPhoneButton"),
            // )
            //
            // Spacer(modifier = Modifier.height(8.dp))

            // Stats button — draggable
            DraggableOverlayButton(
                icon = Icons.Default.Info,
                contentDescription = "Stats for nerds",
                onClick = { viewModel.toggleStats() },
                positionKey = "overlay_stats",
                containerColor = if (uiState.showStats) {
                    MaterialTheme.colorScheme.primary.copy(alpha = 0.7f)
                } else {
                    MaterialTheme.colorScheme.surface.copy(alpha = 0.5f)
                },
                tint = if (uiState.showStats) {
                    MaterialTheme.colorScheme.onPrimary
                } else {
                    MaterialTheme.colorScheme.onSurface
                },
                modifier = Modifier.testTag("statsButton"),
            )

            // File logging button — only shown when enabled in Settings → Diagnostics
            if (uiState.fileLoggingEnabled) {
            Spacer(modifier = Modifier.height(8.dp))

            DraggableOverlayButton(
                icon = Icons.Default.FiberManualRecord,
                contentDescription = "File Logging",
                onClick = { viewModel.toggleFileLogging() },
                positionKey = "overlay_file_log",
                containerColor = if (uiState.fileLoggingActive) {
                    Color.Red.copy(alpha = 0.7f)
                } else {
                    MaterialTheme.colorScheme.surface.copy(alpha = 0.5f)
                },
                tint = if (uiState.fileLoggingActive) {
                    Color.White
                } else {
                    MaterialTheme.colorScheme.onSurface
                },
                modifier = Modifier.testTag("fileLogButton"),
            )
            } // end fileLoggingEnabled

        }

        // Stats overlay panel — bottom-left
        if (uiState.showStats) {
            VideoStatsOverlay(
                stats = uiState.videoStats,
                audioStats = uiState.audioStats,
                sessionState = uiState.sessionState,
                phoneName = uiState.phoneName,
                phoneBatteryLevel = uiState.phoneBatteryLevel,
                aaPixelAspect = uiState.aaPixelAspect,
                aaDpi = uiState.aaDpi,
                wifiFrequencyMhz = uiState.wifiFrequencyMhz,
                modifier = Modifier
                    .align(Alignment.BottomStart)
                    .padding(12.dp)
            )
        }

        // Settings overlay — slides in from left, video keeps playing underneath
        AnimatedVisibility(
            visible = showSettings,
            enter = slideInHorizontally { -it }, // slide in from left edge
            exit = slideOutHorizontally { -it },  // slide out to left edge
        ) {
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .background(Color.Black.copy(alpha = 0.85f))
            ) {
                settingsOverlay { showSettings = false }
            }
        }

        // Phone chooser overlay — slides in from right
        val showChooser by viewModel.showPhoneChooser.collectAsStateWithLifecycle()
        val endpoints by viewModel.discoveredEndpoints.collectAsStateWithLifecycle()
        AnimatedVisibility(
            visible = showChooser,
            enter = slideInHorizontally { it }, // slide in from right
            exit = slideOutHorizontally { it },  // slide out to right
        ) {
            PhoneChooserOverlay(
                endpoints = endpoints,
                onSelect = { id, name -> viewModel.selectPhone(id, name) },
                onDismiss = { viewModel.dismissPhoneChooser() },
            )
        }

    }
}

@Composable
private fun PhoneChooserOverlay(
    endpoints: List<com.openautolink.app.transport.direct.AaNearbyManager.DiscoveredEndpoint>,
    onSelect: (id: String, name: String) -> Unit,
    onDismiss: () -> Unit,
) {
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color.Black.copy(alpha = 0.85f))
            .clickable(onClick = onDismiss),
        contentAlignment = Alignment.Center,
    ) {
        Column(
            modifier = Modifier
                .clip(RoundedCornerShape(16.dp))
                .background(MaterialTheme.colorScheme.surface)
                .padding(24.dp)
                .clickable(enabled = false) {} // prevent click-through
                .width(360.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Text(
                "Select Phone",
                style = MaterialTheme.typography.headlineSmall,
                fontWeight = FontWeight.Bold,
            )
            Spacer(modifier = Modifier.height(8.dp))

            if (endpoints.isEmpty()) {
                Spacer(modifier = Modifier.height(24.dp))
                CircularProgressIndicator(
                    modifier = Modifier.size(32.dp),
                    strokeWidth = 3.dp,
                )
                Spacer(modifier = Modifier.height(12.dp))
                Text(
                    "Searching for phones...",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Text(
                    "Make sure the companion app is running on your phone",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Spacer(modifier = Modifier.height(24.dp))
            } else {
                Spacer(modifier = Modifier.height(12.dp))
                endpoints.forEach { endpoint ->
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .clip(RoundedCornerShape(8.dp))
                            .clickable { onSelect(endpoint.id, endpoint.name) }
                            .background(MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.3f))
                            .padding(horizontal = 16.dp, vertical = 12.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Icon(
                            Icons.Default.PhoneAndroid,
                            contentDescription = null,
                            modifier = Modifier.size(24.dp),
                            tint = MaterialTheme.colorScheme.primary,
                        )
                        Spacer(modifier = Modifier.width(12.dp))
                        Text(
                            endpoint.name,
                            style = MaterialTheme.typography.bodyLarge,
                            fontWeight = FontWeight.Medium,
                        )
                    }
                    Spacer(modifier = Modifier.height(8.dp))
                }
            }

            Spacer(modifier = Modifier.height(8.dp))
            androidx.compose.material3.TextButton(onClick = onDismiss) {
                Text("Cancel")
            }
        }
    }
}

@Composable
private fun VideoStatsOverlay(
    stats: VideoStats,
    audioStats: AudioStats,
    sessionState: SessionState,
    phoneName: String?,
    phoneBatteryLevel: Int?,
    aaPixelAspect: Int,
    aaDpi: Int,
    wifiFrequencyMhz: Int = 0,
    modifier: Modifier = Modifier
) {
    Box(
        modifier = modifier
            .clip(RoundedCornerShape(12.dp))
            .background(Color.Black.copy(alpha = 0.72f))
            .padding(14.dp)
            .testTag("videoStatsOverlay")
    ) {
        Column {
            StatLine("Stats for nerds", "", header = true)
            Spacer(modifier = Modifier.height(6.dp))

            StatLine("Session", sessionState.name)

            // WiFi band
            if (wifiFrequencyMhz > 0) {
                val band = if (wifiFrequencyMhz > 4000) "5 GHz" else "2.4 GHz"
                StatLine("WiFi", "$band (${wifiFrequencyMhz} MHz)",
                    valueColor = if (wifiFrequencyMhz > 4000) Color.Green else Color(0xFFFFFF00))
            }

            // Phone info
            if (phoneName != null) {
                StatLine("Phone", phoneName)
            }
            if (phoneBatteryLevel != null) {
                StatLine("Battery", "$phoneBatteryLevel%",
                    valueColor = when {
                        phoneBatteryLevel >= 50 -> Color.Green
                        phoneBatteryLevel >= 20 -> Color(0xFFFFFF00)
                        else -> Color.Red
                    })
            }

            if (stats.codec != "none") {
                Spacer(modifier = Modifier.height(4.dp))
                if (stats.codecFormat.isNotBlank()) {
                    StatLine("Codec", stats.codecFormat)
                }
                StatLine("Decoder", "${stats.decoderName}${if (stats.isHardware) " HW" else " SW"}")
                if (stats.width > 0) {
                    StatLine("Resolution", "${stats.width}x${stats.height}")
                }
                StatLine("DPI", "$aaDpi (real)")
                if (aaPixelAspect > 0) {
                    StatLine("Pixel Aspect", "${"%.4f".format(aaPixelAspect / 10000f)} (${aaPixelAspect}e⁻⁴)")
                } else {
                    StatLine("Pixel Aspect", "auto")
                }
                StatLine("FPS", "${"%.1f".format(stats.fps)} fps",
                    valueColor = when {
                        stats.fps >= 50 -> Color.Green
                        stats.fps >= 25 -> Color(0xFFFFFF00)
                        else -> Color.Red
                    })
                StatLine("Frames", "Rx:${stats.framesReceived}  Dec:${stats.framesDecoded}")
                if (stats.bitrateKbps > 0) {
                    val bitrateStr = if (stats.bitrateKbps >= 1000) {
                        "${"%.1f".format(stats.bitrateKbps / 1000)} Mbps"
                    } else {
                        "${stats.bitrateKbps.toInt()} kbps"
                    }
                    StatLine("Bitrate", bitrateStr,
                        valueColor = when {
                            stats.bitrateKbps >= 4000 -> Color.Green
                            stats.bitrateKbps >= 2000 -> Color(0xFFFFFF00)
                            else -> Color.Red
                        })
                }
                if (stats.framesDropped > 0) {
                    StatLine("Dropped", stats.framesDropped.toString(),
                        valueColor = Color(0xFFFFAB00))
                }
                if (stats.codecResets > 0) {
                    StatLine("Resets", stats.codecResets.toString(),
                        valueColor = Color(0xFFFF6E40))
                }
            } else {
                StatLine("Video", "not active")
            }

            // Audio stats section
            Spacer(modifier = Modifier.height(6.dp))
            if (audioStats.activePurposes.isNotEmpty()) {
                StatLine("Audio", audioStats.activePurposes.joinToString(", ") { it.name.lowercase() })
                if (audioStats.sampleRate > 0) {
                    StatLine("Rate", "${audioStats.sampleRate} Hz")
                }
                val totalFrames = audioStats.framesWritten.values.sum()
                if (totalFrames > 0) {
                    StatLine("Frames", totalFrames.toString())
                }
                audioStats.underruns.forEach { (purpose, count) ->
                    if (count > 0) {
                        StatLine("${purpose.name.lowercase()} xrun", count.toString(),
                            valueColor = Color(0xFFFFAB00))
                    }
                }
            } else {
                StatLine("Audio", "not active")
            }
        }
    }
}

@Composable
private fun StatLine(
    label: String,
    value: String,
    header: Boolean = false,
    valueColor: Color = Color.White,
) {
    Row(modifier = Modifier.padding(vertical = 1.dp)) {
        Text(
            text = label,
            color = if (header) Color(0xFF64FFDA) else Color(0xFFB0BEC5),
            fontSize = if (header) 16.sp else 13.sp,
            fontWeight = if (header) FontWeight.Bold else FontWeight.Normal,
            fontFamily = FontFamily.Monospace,
            lineHeight = if (header) 20.sp else 16.sp,
            modifier = Modifier.width(if (header) 200.dp else 100.dp),
        )
        if (value.isNotEmpty()) {
            Spacer(modifier = Modifier.width(6.dp))
            Text(
                text = value,
                color = valueColor,
                fontSize = 13.sp,
                fontFamily = FontFamily.Monospace,
                lineHeight = 16.sp,
            )
        }
    }
}

private fun formatUptime(seconds: Long): String {
    val h = seconds / 3600
    val m = (seconds % 3600) / 60
    val s = seconds % 60
    return if (h > 0) "${h}h ${m}m ${s}s" else if (m > 0) "${m}m ${s}s" else "${s}s"
}

@Composable
private fun ConnectionHud(
    uiState: ProjectionUiState,
    modifier: Modifier = Modifier
) {
    Box(
        modifier = modifier
            .clip(RoundedCornerShape(16.dp))
            .background(Color(0xCC000000))
            .padding(32.dp)
            .testTag("connectionHud"),
        contentAlignment = Alignment.Center
    ) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {
            Image(
                painter = painterResource(id = R.drawable.ic_logo),
                contentDescription = "OpenAutoLink",
                modifier = Modifier.height(120.dp),
            )

            Spacer(modifier = Modifier.height(12.dp))

            Text(
                text = "OpenAutoLink",
                style = MaterialTheme.typography.headlineLarge,
                color = Color.White
            )

            Spacer(modifier = Modifier.height(16.dp))

            when (uiState.sessionState) {
                SessionState.IDLE -> {
                    Text(
                        text = uiState.statusMessage,
                        style = MaterialTheme.typography.bodyLarge,
                        color = Color(0xFFB0B0B0)
                    )
                }
                SessionState.CONNECTING -> {
                    CircularProgressIndicator(
                        modifier = Modifier.size(32.dp),
                        color = MaterialTheme.colorScheme.primary,
                        strokeWidth = 3.dp
                    )
                    Spacer(modifier = Modifier.height(8.dp))
                    Text(
                        text = uiState.statusMessage,
                        style = MaterialTheme.typography.bodyLarge,
                        color = MaterialTheme.colorScheme.primary
                    )
                }
                SessionState.CONNECTED -> {
                    Text(
                        text = "Connected",
                        style = MaterialTheme.typography.bodyLarge,
                        color = MaterialTheme.colorScheme.secondary
                    )
                }
                SessionState.STREAMING -> {
                    // HUD is hidden during streaming
                }
                SessionState.ERROR -> {
                    Text(
                        text = uiState.statusMessage,
                        style = MaterialTheme.typography.bodyLarge,
                        color = MaterialTheme.colorScheme.error
                    )
                }
            }
        }
    }
}

/**
 * Transparent overlay showing dashed lines at safe area and content inset boundaries.
 * Teal = stable (safe area), Red = content (hard cutoff).
 */
@Composable
private fun SafeAreaOverlay(
    top: Int,
    bottom: Int,
    left: Int,
    right: Int,
    contentTop: Int,
    contentBottom: Int,
    contentLeft: Int,
    contentRight: Int,
    videoWidth: Int,
    videoHeight: Int,
    modifier: Modifier = Modifier,
) {
    val hasStable = top > 0 || bottom > 0 || left > 0 || right > 0
    val hasContent = contentTop > 0 || contentBottom > 0 || contentLeft > 0 || contentRight > 0
    if (!hasStable && !hasContent) return

    val stableColor = Color(0xFF64FFDA)
    val contentColor = Color(0xFFFF6E40)
    val dashEffect = androidx.compose.ui.graphics.PathEffect.dashPathEffect(
        floatArrayOf(12f, 8f), 0f
    )

    Canvas(modifier = modifier) {
        val w = size.width
        val h = size.height

        fun drawInsetLines(t: Int, b: Int, l: Int, r: Int, color: Color) {
            if (t > 0) {
                val y = (t.toFloat() / videoHeight) * h
                drawLine(color, androidx.compose.ui.geometry.Offset(0f, y),
                    androidx.compose.ui.geometry.Offset(w, y), 3f, pathEffect = dashEffect)
            }
            if (b > 0) {
                val y = h - (b.toFloat() / videoHeight) * h
                drawLine(color, androidx.compose.ui.geometry.Offset(0f, y),
                    androidx.compose.ui.geometry.Offset(w, y), 3f, pathEffect = dashEffect)
            }
            if (l > 0) {
                val x = (l.toFloat() / videoWidth) * w
                drawLine(color, androidx.compose.ui.geometry.Offset(x, 0f),
                    androidx.compose.ui.geometry.Offset(x, h), 3f, pathEffect = dashEffect)
            }
            if (r > 0) {
                val x = w - (r.toFloat() / videoWidth) * w
                drawLine(color, androidx.compose.ui.geometry.Offset(x, 0f),
                    androidx.compose.ui.geometry.Offset(x, h), 3f, pathEffect = dashEffect)
            }
        }

        drawInsetLines(top, bottom, left, right, stableColor)
        drawInsetLines(contentTop, contentBottom, contentLeft, contentRight, contentColor)
    }
}
