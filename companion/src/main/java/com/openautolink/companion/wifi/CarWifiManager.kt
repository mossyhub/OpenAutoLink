package com.openautolink.companion.wifi

import android.annotation.SuppressLint
import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.WifiManager
import android.net.wifi.WifiNetworkSpecifier
import android.net.wifi.WifiNetworkSuggestion
import android.os.Build
import android.os.Handler
import android.os.Looper
import com.openautolink.companion.diagnostics.CompanionLog
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/**
 * Manages WiFi connectivity to the car's hotspot using a two-layer approach:
 *
 * 1. **WifiNetworkSuggestion** (Android 10+): Registers the car's SSID as a
 *    saved/preferred network suggestion. Android will auto-connect without
 *    showing the "Stay connected?" dialog, because suggestions are treated
 *    like user-saved networks. This is the persistent layer.
 *
 * 2. **WifiNetworkSpecifier** + **requestNetwork**: Explicitly requests the
 *    car's network and binds the process to it via [ConnectivityManager.bindProcessToNetwork].
 *    This ensures our TCP sockets route through the car's WiFi even when the
 *    phone is simultaneously on another WiFi (e.g. home network). The binding
 *    is cleared when the car network is lost.
 *
 * The "Stay connected to network with no internet?" dialog is the root cause
 * of connection instability: when the user dismisses it, Android tears down
 * the WifiNetworkSpecifier network. WifiNetworkSuggestion avoids the dialog.
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
    private val wifiManager =
        context.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
    private val handler = Handler(Looper.getMainLooper())

    private val _state = MutableStateFlow<State>(State.Idle)
    val state: StateFlow<State> = _state

    private var entries: List<CarWifiEntry> = emptyList()
    private var currentCallback: ConnectivityManager.NetworkCallback? = null
    private var attempt = 0
    private var running = false
    private var retryRunnable: Runnable? = null

    /** Suggestions currently registered with WifiManager (to remove on stop). */
    private var activeSuggestions: List<WifiNetworkSuggestion> = emptyList()

    fun start(carWifiEntries: List<CarWifiEntry>) {
        if (carWifiEntries.isEmpty()) {
            CompanionLog.w(TAG, "No car WiFi entries configured, skipping")
            return
        }
        entries = carWifiEntries
        running = true
        attempt = 0
        CompanionLog.i(TAG, "Starting car WiFi manager for ${entries.size} SSID(s)")
        registerSuggestions()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) tryConnect()
    }

    fun stop() {
        running = false
        cancelRetry()
        releaseCallback()
        clearNetworkBinding()
        removeSuggestions()
        _state.value = State.Idle
        CompanionLog.i(TAG, "Stopped")
    }

    /**
     * Register [WifiNetworkSuggestion]s for all configured car SSIDs.
     *
     * Suggestions tell Android to auto-connect to these networks in the background
     * without any user-visible "Stay connected?" prompt. The OS treats them like
     * saved networks but owned by the app. Requires [WifiManager.STATUS_NETWORK_SUGGESTIONS_SUCCESS]
     * — on first install the user may need to grant "WiFi control" via Settings once.
     */
    private fun registerSuggestions() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            CompanionLog.d(TAG, "WifiNetworkSuggestion not available (API < 29)")
            return
        }
        try {
            val suggestions = entries.map { entry ->
                WifiNetworkSuggestion.Builder()
                    .setSsid(entry.ssid)
                    .setWpa2Passphrase(entry.password)
                    // isUserInteractionRequired = false → no "Stay connected?"
                    // isInitialAutojoinEnabled = true → Android auto-connects
                    .setIsInitialAutojoinEnabled(true)
                    .build()
            }
            // Remove stale suggestions before adding (idempotent).
            if (activeSuggestions.isNotEmpty()) {
                val removeStatus = wifiManager.removeNetworkSuggestions(activeSuggestions)
                CompanionLog.d(TAG, "Removed ${activeSuggestions.size} old suggestion(s), status=$removeStatus")
            }
            val status = wifiManager.addNetworkSuggestions(suggestions)
            activeSuggestions = if (status == WifiManager.STATUS_NETWORK_SUGGESTIONS_SUCCESS) {
                CompanionLog.i(TAG, "Registered ${suggestions.size} WiFi suggestion(s) for auto-connect")
                suggestions
            } else {
                CompanionLog.w(TAG,
                    "WifiNetworkSuggestion registration failed (status=$status). " +
                    "User may need to grant 'WiFi control' in app settings. " +
                    "Falling back to WifiNetworkSpecifier only.")
                emptyList()
            }
        } catch (e: Exception) {
            CompanionLog.w(TAG, "WifiNetworkSuggestion error: ${e.message}")
        }
    }

    private fun removeSuggestions() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return
        if (activeSuggestions.isEmpty()) return
        try {
            wifiManager.removeNetworkSuggestions(activeSuggestions)
            CompanionLog.d(TAG, "Removed ${activeSuggestions.size} WiFi suggestion(s)")
        } catch (e: Exception) {
            CompanionLog.d(TAG, "removeSuggestions error: ${e.message}")
        }
        activeSuggestions = emptyList()
    }

    @SuppressLint("NewApi")
    @androidx.annotation.RequiresApi(Build.VERSION_CODES.Q)
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
                // Bind this process to the car's network so all our TCP sockets
                // (TcpAdvertiser server, identity probe, UDP) route through it.
                // Without this, Android may silently route through cellular/home
                // WiFi even though the car network is "connected" as a secondary.
                bindToNetwork(network)
                // Reset attempt counter so reconnections get full retry budget
                attempt = 0
            }

            override fun onUnavailable() {
                if (!running) return
                CompanionLog.w(TAG, "Attempt $attempt failed (SSID not found or wrong password)")
                clearNetworkBinding()
                scheduleRetry()
            }

            override fun onLost(network: Network) {
                if (!running) return
                CompanionLog.w(TAG, "Car WiFi \"${entry.ssid}\" lost")
                clearNetworkBinding()
                _state.value = State.Scanning(attempt, MAX_ATTEMPTS)
                // Debounce: wait before retrying to avoid thrashing on WiFi flaps
                scheduleRetry(LOST_RETRY_DELAY_MS)
            }
        }

        currentCallback = callback
        connectivityManager.requestNetwork(request, callback)
    }

    /**
     * Expose the active car [Network] so callers can bind individual sockets
     * (e.g. outbound TCP connections or DatagramSockets that must reach the
     * car subnet). Do NOT use [ConnectivityManager.bindProcessToNetwork] —
     * a process-wide binding disrupts loopback sockets (the AA proxy) on
     * some Android versions.
     */
    val activeNetwork: Network?
        get() = _activeNetwork

    @Volatile private var _activeNetwork: Network? = null

    private fun bindToNetwork(network: Network) {
        _activeNetwork = network
        CompanionLog.i(TAG, "Car WiFi network available — per-socket binding ready")
    }

    private fun clearNetworkBinding() {
        _activeNetwork = null
        CompanionLog.d(TAG, "Car WiFi network cleared")
    }

    private fun scheduleRetry(delayMs: Long = RETRY_DELAY_MS) {
        cancelRetry()
        val r = Runnable { if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) tryConnect() }
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
