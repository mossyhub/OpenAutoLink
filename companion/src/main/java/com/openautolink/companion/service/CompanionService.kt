package com.openautolink.companion.service

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
    private var tcpAdvertiser: TcpAdvertiser? = null
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
        tcpAdvertiser?.stop()

        val prefs = getSharedPreferences(CompanionPrefs.NAME, MODE_PRIVATE)
        val mode = prefs.getString(CompanionPrefs.TRANSPORT_MODE, CompanionPrefs.DEFAULT_TRANSPORT)

        when (mode) {
            CompanionPrefs.TRANSPORT_TCP -> {
                Log.i(TAG, "Transport mode: TCP (hotspot)")
                tcpAdvertiser = TcpAdvertiser(this, this)
                tcpAdvertiser?.start()
                updateNotification("TCP: waiting for car on port ${TcpAdvertiser.PORT}...")
            }
            else -> {
                Log.i(TAG, "Transport mode: Nearby")
                nearbyAdvertiser = NearbyAdvertiser(this, serviceScope, this)
                nearbyAdvertiser?.start()
                updateNotification("Nearby: searching for car...")
            }
        }
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
        Log.i(TAG, "AA proxy disconnected")
        updateNotification("Disconnected — tap Start to retry")
        // Auto-reconnect disabled during development to avoid interfering
        // with the phone's AA session lifecycle.
    }

    override fun onLaunchTimeout() {
        Log.i(TAG, "Launch timeout — waiting for AA to connect to proxy")
        // Don't restart — the proxy is still listening, AA may connect late.
    }

    // ── Wake lock ──────────────────────────────────────────────────────

    private fun acquireWakeLock() {
        // Release existing lock before acquiring a new one to reset the timeout
        releaseWakeLock()
        val pm = getSystemService(POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "OalCompanion:WakeLock")
        wakeLock?.acquire(4 * 60 * 60 * 1000L) // 4 hours — covers long drives
    }

    private fun releaseWakeLock() {
        if (wakeLock?.isHeld == true) {
            wakeLock?.release()
        }
        wakeLock = null
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
        tcpAdvertiser?.stop()
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
        private val _isRunning = MutableStateFlow(false)
        val isRunning: kotlinx.coroutines.flow.StateFlow<Boolean> = _isRunning

        private val _isConnected = MutableStateFlow(false)
        val isConnected: kotlinx.coroutines.flow.StateFlow<Boolean> = _isConnected

        private val _statusText = MutableStateFlow("Stopped")
        val statusText: kotlinx.coroutines.flow.StateFlow<String> = _statusText
    }
}
