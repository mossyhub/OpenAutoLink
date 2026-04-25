package com.openautolink.companion.service

import android.annotation.SuppressLint
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.PowerManager
import android.util.Log
import androidx.core.app.NotificationCompat
import com.openautolink.companion.CompanionPrefs
import com.openautolink.companion.MainActivity
import com.openautolink.companion.R
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch

/**
 * Foreground service that manages the Nearby advertising lifecycle.
 * Keeps the phone discoverable by the car's OAL app and relays AA
 * traffic through a Nearby stream tunnel.
 */
class CompanionService : Service(), NearbyAdvertiser.StateListener {

    private val serviceJob = SupervisorJob()
    private val serviceScope = CoroutineScope(Dispatchers.IO + serviceJob)
    private var nearbyAdvertiser: NearbyAdvertiser? = null
    private var wakeLock: PowerManager.WakeLock? = null

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
        startForeground(NOTIFICATION_ID, createNotification("Starting..."))
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                Log.i(TAG, "Stop requested")
                _isRunning.value = false
                _isConnected.value = false
                _statusText.value = "Stopped"
                stopSelf()
            }

            ACTION_START -> {
                Log.i(TAG, "Start requested")
                _isRunning.value = true
                _isConnected.value = false
                _statusText.value = "Advertising..."
                startNearby()
            }

            else -> {
                // System restart
                if (_isRunning.value) {
                    Log.i(TAG, "Service restarted by system, resuming")
                    startNearby()
                }
            }
        }
        return START_STICKY
    }

    private fun startNearby() {
        acquireWakeLock()
        nearbyAdvertiser?.stop()

        nearbyAdvertiser = NearbyAdvertiser(this, serviceScope, this)
        nearbyAdvertiser?.start()
        updateNotification("Searching for car...")
    }

    // ── NearbyAdvertiser.StateListener ─────────────────────────────────

    override fun onConnecting() {
        _statusText.value = "Connecting..."
        updateNotification("Connecting to car...")
    }

    override fun onProxyConnected() {
        _isConnected.value = true
        _statusText.value = "Connected"
        acquireWakeLock()
        updateNotification("Connected — AA active")
    }

    override fun onProxyDisconnected() {
        _isConnected.value = false
        _statusText.value = "Disconnected"
        Log.i(TAG, "AA proxy disconnected — restarting advertising")

        // Always restart advertising after disconnect so the car can reconnect
        // (e.g., after Save & Restart on the car side).
        _statusText.value = "Reconnecting..."
        updateNotification("Waiting for car...")
        serviceScope.launch {
            delay(2000)
            startNearby()
        }
    }

    override fun onLaunchTimeout() {
        Log.i(TAG, "Launch timeout, restarting advertising")
        _statusText.value = "Timeout — retrying..."
        updateNotification("Searching for car...")
        nearbyAdvertiser?.stop()
        startNearby()
    }

    // ── Bluetooth helpers ──────────────────────────────────────────────

    @SuppressLint("MissingPermission")
    private fun shouldAutoReconnect(): Boolean {
        val prefs = getSharedPreferences(CompanionPrefs.NAME, MODE_PRIVATE)
        val targetMacs = prefs.getStringSet(CompanionPrefs.AUTO_START_BT_MACS, emptySet())
            ?: emptySet()
        if (targetMacs.isEmpty()) return true // No BT trigger configured — always reconnect

        return try {
            val bm = getSystemService(BLUETOOTH_SERVICE) as android.bluetooth.BluetoothManager
            val adapter = bm.adapter ?: return false
            targetMacs.any { mac ->
                try {
                    val device = adapter.getRemoteDevice(mac)
                    val method = device.javaClass.getMethod("isConnected")
                    method.invoke(device) as? Boolean ?: false
                } catch (_: Exception) {
                    false
                }
            }
        } catch (_: Exception) {
            false
        }
    }

    // ── Wake lock ──────────────────────────────────────────────────────

    private fun acquireWakeLock() {
        if (wakeLock?.isHeld == true) return
        val pm = getSystemService(POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "OalCompanion:WakeLock")
        wakeLock?.acquire(10 * 60 * 1000L)
    }

    private fun releaseWakeLock() {
        if (wakeLock?.isHeld == true) {
            wakeLock?.release()
        }
    }

    // ── Notification ───────────────────────────────────────────────────

    private fun createNotificationChannel() {
        val channel = NotificationChannel(
            CHANNEL_ID, "OpenAutoLink Companion",
            NotificationManager.IMPORTANCE_LOW
        )
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(channel)
    }

    private fun createNotification(text: String): Notification {
        val stopIntent = Intent(this, CompanionService::class.java).apply {
            action = ACTION_STOP
        }
        val stopPending = PendingIntent.getService(
            this, 0, stopIntent, PendingIntent.FLAG_IMMUTABLE
        )

        val openIntent = Intent(this, MainActivity::class.java)
        val openPending = PendingIntent.getActivity(
            this, 0, openIntent, PendingIntent.FLAG_IMMUTABLE
        )

        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("OpenAutoLink Companion")
            .setContentText(text)
            .setSmallIcon(R.drawable.ic_notification)
            .setOngoing(true)
            .setContentIntent(openPending)
            .addAction(android.R.drawable.ic_menu_close_clear_cancel, "Stop", stopPending)
            .build()
    }

    private fun updateNotification(text: String) {
        val nm = getSystemService(NotificationManager::class.java)
        nm.notify(NOTIFICATION_ID, createNotification(text))
    }

    // ── Lifecycle ──────────────────────────────────────────────────────

    override fun onDestroy() {
        _isRunning.value = false
        _isConnected.value = false
        _statusText.value = "Stopped"
        nearbyAdvertiser?.stop()
        releaseWakeLock()
        serviceScope.cancel()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?) = null

    companion object {
        private const val TAG = "OAL_Service"
        private const val CHANNEL_ID = "oal_companion_channel"
        private const val NOTIFICATION_ID = 1

        const val ACTION_START = "ACTION_START"
        const val ACTION_STOP = "ACTION_STOP"

        /** Observable service state for UI. */
        val isRunning = MutableStateFlow(false)
        private val _isRunning = isRunning

        val isConnected = MutableStateFlow(false)
        private val _isConnected = isConnected

        val statusText = MutableStateFlow("Stopped")
        private val _statusText = statusText
    }
}
