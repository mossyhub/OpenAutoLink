package com.openautolink.companion

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import com.openautolink.companion.service.CompanionService
import com.openautolink.companion.ui.MainScreen
import com.openautolink.companion.ui.theme.OalCompanionTheme

class MainActivity : ComponentActivity() {

    private val requiredPermissions = buildList {
        add(Manifest.permission.ACCESS_FINE_LOCATION)
        add(Manifest.permission.ACCESS_COARSE_LOCATION)
        add(Manifest.permission.BLUETOOTH_CONNECT)
        add(Manifest.permission.BLUETOOTH_ADVERTISE)
        add(Manifest.permission.BLUETOOTH_SCAN)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            add(Manifest.permission.POST_NOTIFICATIONS)
            add(Manifest.permission.NEARBY_WIFI_DEVICES)
        }
    }.toTypedArray()

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { /* permissions granted/denied — UI will reflect state */ }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        requestMissingPermissions()
        handleIntent(intent)

        val prefs = getSharedPreferences(CompanionPrefs.NAME, MODE_PRIVATE)
        val autoStartMode = prefs.getInt(CompanionPrefs.AUTO_START_MODE, 0)
        if (autoStartMode == CompanionPrefs.AUTO_START_APP_OPEN && !CompanionService.isRunning.value) {
            startCompanionService()
        }

        setContent {
            OalCompanionTheme {
                MainScreen(
                    onStart = { startCompanionService() },
                    onStop = { stopCompanionService() },
                )
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleIntent(intent)
    }

    private fun handleIntent(intent: Intent?) {
        val uri = intent?.data ?: return
        if (uri.scheme != "oalcompanion") return
        when (uri.host) {
            "start" -> startCompanionService()
            "stop" -> stopCompanionService()
        }
    }

    private fun startCompanionService() {
        val serviceIntent = Intent(this, CompanionService::class.java).apply {
            action = CompanionService.ACTION_START
        }
        ContextCompat.startForegroundService(this, serviceIntent)
    }

    private fun stopCompanionService() {
        val serviceIntent = Intent(this, CompanionService::class.java).apply {
            action = CompanionService.ACTION_STOP
        }
        startService(serviceIntent)
    }

    private fun requestMissingPermissions() {
        val missing = requiredPermissions.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isNotEmpty()) {
            permissionLauncher.launch(missing.toTypedArray())
        }
    }
}

/** SharedPreferences keys for the companion app. */
object CompanionPrefs {
    const val NAME = "OalCompanionPrefs"

    const val AUTO_START_MODE = "auto_start_mode"
    const val AUTO_START_BT_MACS = "auto_start_bt_macs"
    const val BT_DISCONNECT_STOP = "bt_disconnect_stop"
    const val BT_AUTO_RECONNECT = "bt_auto_reconnect"
    const val AUTO_START_WIFI_SSIDS = "auto_start_wifi_ssids"

    const val AUTO_START_OFF = 0
    const val AUTO_START_BT = 1
    const val AUTO_START_WIFI = 2
    const val AUTO_START_APP_OPEN = 3

    const val TRANSPORT_MODE = "transport_mode"
    const val TRANSPORT_NEARBY = "nearby"
    const val TRANSPORT_TCP = "tcp"
    const val DEFAULT_TRANSPORT = TRANSPORT_TCP
}
