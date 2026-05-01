package com.openautolink.companion.wifi

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.WifiNetworkSpecifier
import android.os.Handler
import android.os.Looper
import com.openautolink.companion.diagnostics.CompanionLog
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/**
 * Manages WifiNetworkSpecifier lifecycle to ensure the phone connects to
 * a car's WiFi hotspot, even when already on another WiFi (e.g. home).
 *
 * Runs as a pure additive layer — TcpAdvertiser on 0.0.0.0 is unaffected.
 * This class simply ensures the car's WiFi is reachable as an interface.
 *
 * Retry strategy: each requestNetwork() scans ~30s. On failure, wait 5s
 * and retry, up to [MAX_ATTEMPTS] times (~7 min total coverage).
 */
class CarWifiManager(private val context: Context) {

    sealed class State {
        data object Idle : State()
        data class Scanning(val attempt: Int, val maxAttempts: Int) : State()
        data class Connected(val ssid: String) : State()
        data class Failed(val reason: String) : State()
    }

    private val connectivityManager =
        context.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
    private val handler = Handler(Looper.getMainLooper())

    private val _state = MutableStateFlow<State>(State.Idle)
    val state: StateFlow<State> = _state

    private var entries: List<CarWifiEntry> = emptyList()
    private var currentCallback: ConnectivityManager.NetworkCallback? = null
    private var attempt = 0
    private var running = false
    private var retryRunnable: Runnable? = null

    fun start(carWifiEntries: List<CarWifiEntry>) {
        if (carWifiEntries.isEmpty()) {
            CompanionLog.w(TAG, "No car WiFi entries configured, skipping")
            return
        }
        entries = carWifiEntries
        running = true
        attempt = 0
        CompanionLog.i(TAG, "Starting car WiFi manager for ${entries.size} SSID(s)")
        tryConnect()
    }

    fun stop() {
        running = false
        cancelRetry()
        releaseCallback()
        _state.value = State.Idle
        CompanionLog.i(TAG, "Stopped")
    }

    private fun tryConnect() {
        if (!running) return
        if (attempt >= MAX_ATTEMPTS) {
            val msg = "Gave up after $MAX_ATTEMPTS attempts"
            CompanionLog.w(TAG, msg)
            _state.value = State.Failed(msg)
            return
        }

        attempt++
        _state.value = State.Scanning(attempt, MAX_ATTEMPTS)

        // Use the first entry for now. Multi-car: scan for which SSID is
        // in range and pick the matching one. For most users there is one car.
        val entry = entries.first()
        CompanionLog.i(TAG, "Attempt $attempt/$MAX_ATTEMPTS: requesting \"${entry.ssid}\"")

        releaseCallback()

        val specifier = WifiNetworkSpecifier.Builder()
            .setSsid(entry.ssid)
            .setWpa2Passphrase(entry.password)
            .build()

        val request = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .setNetworkSpecifier(specifier)
            .build()

        val callback = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) {
                if (!running) return
                CompanionLog.i(TAG, "Connected to \"${entry.ssid}\" on attempt $attempt")
                _state.value = State.Connected(entry.ssid)
                // Reset attempt counter so reconnections get full retry budget
                attempt = 0
            }

            override fun onUnavailable() {
                if (!running) return
                CompanionLog.w(TAG, "Attempt $attempt failed (SSID not found or declined)")
                scheduleRetry()
            }

            override fun onLost(network: Network) {
                if (!running) return
                CompanionLog.w(TAG, "Car WiFi \"${entry.ssid}\" lost")
                _state.value = State.Scanning(attempt, MAX_ATTEMPTS)
                // Debounce: wait before retrying to avoid thrashing on WiFi flaps
                scheduleRetry(LOST_RETRY_DELAY_MS)
            }
        }

        currentCallback = callback
        connectivityManager.requestNetwork(request, callback)
    }

    private fun scheduleRetry(delayMs: Long = RETRY_DELAY_MS) {
        cancelRetry()
        val r = Runnable { tryConnect() }
        retryRunnable = r
        handler.postDelayed(r, delayMs)
    }

    private fun cancelRetry() {
        retryRunnable?.let { handler.removeCallbacks(it) }
        retryRunnable = null
    }

    private fun releaseCallback() {
        currentCallback?.let {
            try {
                connectivityManager.unregisterNetworkCallback(it)
            } catch (_: Exception) {
                // Already unregistered or never registered
            }
        }
        currentCallback = null
    }

    companion object {
        private const val TAG = "OAL_CarWifi"
        private const val MAX_ATTEMPTS = 12
        private const val RETRY_DELAY_MS = 5_000L
        private const val LOST_RETRY_DELAY_MS = 2_000L
    }
}

/**
 * A car WiFi network entry (SSID + password).
 */
data class CarWifiEntry(val ssid: String, val password: String) {
    /** Serialize to prefs format: "ssid\tpassword" */
    fun toPrefsString(): String = "$ssid\t$password"

    companion object {
        /** Parse from prefs format: "ssid\tpassword" */
        fun fromPrefsString(s: String): CarWifiEntry? {
            val parts = s.split('\t', limit = 2)
            if (parts.size != 2 || parts[0].isBlank()) return null
            return CarWifiEntry(parts[0], parts[1])
        }

        /** Load all entries from SharedPreferences */
        fun loadAll(prefs: android.content.SharedPreferences): List<CarWifiEntry> {
            val raw = prefs.getStringSet(
                com.openautolink.companion.CompanionPrefs.CAR_WIFI_ENTRIES,
                emptySet()
            ) ?: emptySet()
            return raw.mapNotNull { fromPrefsString(it) }
        }

        /** Save all entries to SharedPreferences */
        fun saveAll(prefs: android.content.SharedPreferences, entries: List<CarWifiEntry>) {
            prefs.edit()
                .putStringSet(
                    com.openautolink.companion.CompanionPrefs.CAR_WIFI_ENTRIES,
                    entries.map { it.toPrefsString() }.toSet()
                )
                .apply()
        }
    }
}
