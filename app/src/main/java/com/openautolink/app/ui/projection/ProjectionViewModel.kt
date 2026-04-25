package com.openautolink.app.ui.projection

import android.app.Application
import android.content.Context
import android.media.AudioManager
import android.net.ConnectivityManager
import android.net.LinkProperties
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.os.SystemClock
import android.util.Log
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.Surface
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.openautolink.app.audio.AudioStats
import com.openautolink.app.data.AppPreferences
import com.openautolink.app.input.SteeringWheelController
import com.openautolink.app.input.TouchForwarder
import com.openautolink.app.input.TouchForwarderImpl
import com.openautolink.app.navigation.ManeuverState
import com.openautolink.app.session.SessionManager
import com.openautolink.app.session.SessionState
import com.openautolink.app.video.VideoStats
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch

data class ProjectionUiState(
    val sessionState: SessionState = SessionState.IDLE,
    val statusMessage: String = "Ready",
    val phoneName: String? = null,
    val videoStats: VideoStats = VideoStats(),
    val audioStats: AudioStats = AudioStats(),
    val showStats: Boolean = false,
    val maneuver: ManeuverState? = null,
    val phoneBatteryLevel: Int? = null,
    val phoneBatteryCritical: Boolean = false,
    val voiceSessionActive: Boolean = false,
    val phoneSignalStrength: Int? = null,
    val wifiFrequencyMhz: Int = 0,
    val displayMode: String = AppPreferences.DEFAULT_DISPLAY_MODE,
    val safeAreaTop: Int = AppPreferences.DEFAULT_SAFE_AREA_TOP,
    val safeAreaBottom: Int = AppPreferences.DEFAULT_SAFE_AREA_BOTTOM,
    val safeAreaLeft: Int = AppPreferences.DEFAULT_SAFE_AREA_LEFT,
    val safeAreaRight: Int = AppPreferences.DEFAULT_SAFE_AREA_RIGHT,
    val videoScalingMode: String = AppPreferences.DEFAULT_VIDEO_SCALING_MODE,
    val aaPixelAspect: Int = 10000,
    val aaDpi: Int = 160,
)

class ProjectionViewModel(application: Application) : AndroidViewModel(application) {

    companion object {
        private const val TAG = "ProjectionViewModel"
    }

    private val preferences = AppPreferences.getInstance(application)
    private val audioManager = application.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    private val connectivityManager = application.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
    private val sessionManager = SessionManager.getInstance(viewModelScope, application, audioManager)
    @Volatile private var selectedNetworkInterfaceName: String = ""
    @Volatile private var lastTransportNetworkEventAt: Long = 0L
    private val trackedTransportNetworks = mutableSetOf<Long>()

    /** Suppress config_echo DataStore writes while Settings is open. */
    fun setSettingsOpen(open: Boolean) {
    }

    private val touchForwarder: TouchForwarder = TouchForwarderImpl { touchMessage ->
        viewModelScope.launch {
            sessionManager.sendControlMessage(touchMessage)
        }
    }

    private val steeringWheelController = SteeringWheelController(
        sendMessage = { buttonMessage ->
            viewModelScope.launch {
                sessionManager.sendControlMessage(buttonMessage)
            }
        },
        audioManager = audioManager
    )

    private val _phoneName = MutableStateFlow<String?>(null)
    private val _videoStats = MutableStateFlow(VideoStats())
    private val _audioStats = MutableStateFlow(AudioStats())
    private val _showStats = MutableStateFlow(false)

    // Pending surface — stored when surfaceCreated fires before decoder exists.
    // Attached to decoder on session start or when decoder becomes available.
    private var pendingSurface: Surface? = null
    private var pendingSurfaceWidth: Int = 0
    private var pendingSurfaceHeight: Int = 0
    private var surfaceDebounceJob: kotlinx.coroutines.Job? = null

    private val transportNetworkCallback = object : ConnectivityManager.NetworkCallback() {
        override fun onAvailable(network: Network) {
            handleTransportNetworkUpdate(network, "available")
        }

        override fun onCapabilitiesChanged(network: Network, networkCapabilities: NetworkCapabilities) {
            handleTransportNetworkUpdate(network, "capabilities")
        }

        override fun onLinkPropertiesChanged(network: Network, linkProperties: LinkProperties) {
            handleTransportNetworkUpdate(network, "link")
        }

        override fun onLost(network: Network) {
            val handle = network.networkHandle
            val wasTracked = synchronized(trackedTransportNetworks) {
                trackedTransportNetworks.remove(handle)
            }
            if (!wasTracked) return
            requestTransportReconnect("lost")
        }
    }

    val uiState: StateFlow<ProjectionUiState> = combine(
        sessionManager.sessionState,
        sessionManager.statusMessage,
        _phoneName,
        _videoStats,
        _audioStats,
        _showStats,
        sessionManager.currentManeuver,
        sessionManager.phoneBatteryLevel,
        sessionManager.phoneBatteryCritical,
        sessionManager.voiceSessionActive,
        preferences.displayMode,
        preferences.safeAreaTop,
        preferences.safeAreaBottom,
        preferences.safeAreaLeft,
        preferences.safeAreaRight,
        sessionManager.phoneSignalStrength,
        preferences.videoScalingMode,
        sessionManager.wifiFrequencyMhz,
    ) { values ->
        ProjectionUiState(
            sessionState = values[0] as SessionState,
            statusMessage = values[1] as String,
            phoneName = values[2] as? String,
            videoStats = values[3] as VideoStats,
            audioStats = values[4] as AudioStats,
            showStats = values[5] as Boolean,
            maneuver = values[6] as? ManeuverState,
            phoneBatteryLevel = values[7] as? Int,
            phoneBatteryCritical = values[8] as Boolean,
            voiceSessionActive = values[9] as Boolean,
            displayMode = values[10] as String,
            safeAreaTop = values[11] as Int,
            safeAreaBottom = values[12] as Int,
            safeAreaLeft = values[13] as Int,
            safeAreaRight = values[14] as Int,
            phoneSignalStrength = values[15] as? Int,
            videoScalingMode = values[16] as String,
            wifiFrequencyMhz = values[17] as Int,
        )
    }.stateIn(
        viewModelScope,
        SharingStarted.WhileSubscribed(5000),
        ProjectionUiState()
    )

    init {
        registerTransportNetworkCallback()

        // Collect video and audio stats when streaming
        viewModelScope.launch {
            sessionManager.sessionState.collect { state ->
                // Attach pending surface when decoder becomes available
                if (state == SessionState.CONNECTED ||
                    state == SessionState.STREAMING) {
                    attachPendingSurface()
                }
                if (state == SessionState.STREAMING) {
                    sessionManager.videoStats?.let { statsFlow ->
                        launch {
                            statsFlow.collect { stats ->
                                _videoStats.value = stats
                            }
                        }
                    }
                    sessionManager.audioStats?.let { statsFlow ->
                        launch {
                            statsFlow.collect { stats ->
                                _audioStats.value = stats
                            }
                        }
                    }
                }
            }
        }
    }

    private var hasConnected = false

    fun connect() {
        if (hasConnected && sessionManager.sessionState.value != SessionState.IDLE) {
            // Session is already running — don't restart.
            // This prevents reconnect when navigating back from Settings
            // or when the app resumes from background.
            // But check if cluster needs relaunching (Templates Host may have killed it).
            sessionManager.ensureClusterAlive()
            return
        }
        hasConnected = true
        viewModelScope.launch {
            val codec = preferences.videoCodec.first()
            val micSrc = preferences.micSource.first()
            val scalingMode = preferences.videoScalingMode.first()
            val hotspotSsid = preferences.hotspotSsid.first()
            val hotspotPassword = preferences.hotspotPassword.first()
            val directTransport = preferences.directTransport.first()
            val videoAutoNeg = preferences.videoAutoNegotiate.first()
            val aaRes = preferences.aaResolution.first()
            val aaDpi = preferences.aaDpi.first()
            val aaWM = preferences.aaWidthMargin.first()
            val aaHM = preferences.aaHeightMargin.first()
            val aaPA = preferences.aaPixelAspect.first()
            val videoFps = preferences.videoFps.first()
            val driveSide = preferences.driveSide.first()
            val hideClock = preferences.hideAaClock.first()
            val hideSignal = preferences.hidePhoneSignal.first()
            val hideBattery = preferences.hideBatteryLevel.first()

            // Load key remap from preferences
            val keyRemapJson = preferences.keyRemap.first()
            if (keyRemapJson.isNotBlank()) {
                try {
                    val map = mutableMapOf<Int, Int>()
                    val json = org.json.JSONObject(keyRemapJson)
                    for (key in json.keys()) {
                        map[key.toInt()] = json.getInt(key)
                    }
                    steeringWheelController.customKeyMap = map
                } catch (_: Exception) {
                    steeringWheelController.customKeyMap = emptyMap()
                }
            }

            // Load volume offsets
            val volMedia = preferences.volumeOffsetMedia.first()
            val volNav = preferences.volumeOffsetNavigation.first()
            val volAssistant = preferences.volumeOffsetAssistant.first()

            sessionManager.start(
                codecPreference = codec,
                micSourcePreference = micSrc,
                scalingMode = scalingMode,
                directTransport = directTransport,
                hotspotSsid = hotspotSsid,
                hotspotPassword = hotspotPassword,
                videoAutoNegotiate = videoAutoNeg,
                aaResolution = aaRes,
                aaDpi = aaDpi,
                aaWidthMargin = aaWM,
                aaHeightMargin = aaHM,
                aaPixelAspect = aaPA,
                videoFps = videoFps,
                driveSide = driveSide,
                hideClock = hideClock,
                hideSignal = hideSignal,
                hideBattery = hideBattery,
                volumeOffsetMedia = volMedia,
                volumeOffsetNavigation = volNav,
                volumeOffsetAssistant = volAssistant,
            )
        }
    }

    /** Force reconnect — used by "Save & Connect" button in Settings. */
    fun reconnect() {
        hasConnected = false
        connect()
    }

    fun disconnect() {
        sessionManager.stop()
    }

    /**
     * Resolve a saved network interface name to an [android.net.Network] for socket binding.
     * Uses two-tier lookup: first by interface name via ConnectivityManager, then falls back
     * to default routing if not found. Skips binding for loopback addresses.
     *
     * When no interface is configured (empty string), auto-selects eth0 if available.
     * On GM AAOS head units, a USB NIC always appears as eth0.
     */
    private fun resolveNetwork(interfaceName: String): Network? {
        val targetName = interfaceName.ifBlank {
            // Auto-select: prefer eth0 (USB NIC on GM AAOS), then any Ethernet interface
            val autoName = findDefaultEthernetInterface()
            if (autoName != null) {
                Log.i(TAG, "Auto-selected network interface: $autoName")
                com.openautolink.app.diagnostics.DiagnosticLog.i("transport",
                    "Auto-selected network interface: $autoName")
            }
            autoName ?: return null // no ethernet found — default routing
        }
        try {
            for (network in connectivityManager.allNetworks) {
                val linkProps = connectivityManager.getLinkProperties(network) ?: continue
                if (linkProps.interfaceName == targetName) {
                    val caps = connectivityManager.getNetworkCapabilities(network)
                    val transport = when {
                        caps?.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET) == true -> "Ethernet"
                        caps?.hasTransport(NetworkCapabilities.TRANSPORT_USB) == true -> "USB"
                        caps?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) == true -> "WiFi"
                        else -> "other"
                    }
                    Log.i(TAG, "Bound to interface '$targetName' ($transport) handle=${network.networkHandle}")
                    com.openautolink.app.diagnostics.DiagnosticLog.i("transport",
                        "Bound to interface '$targetName' ($transport)")
                    return network
                }
            }
            Log.w(TAG, "Interface '$targetName' not found in ConnectivityManager — default routing")
            com.openautolink.app.diagnostics.DiagnosticLog.w("transport",
                "Interface '$targetName' not found — default routing")
        } catch (e: Exception) {
            Log.w(TAG, "Network resolution failed: ${e.message}")
        }
        return null
    }

    /**
     * Find the best default Ethernet interface for bridge communication.
     * Prefers eth0 (USB NIC on GM AAOS), then any other Ethernet/USB transport interface.
     */
    private fun findDefaultEthernetInterface(): String? {
        var fallback: String? = null
        try {
            for (network in connectivityManager.allNetworks) {
                val caps = connectivityManager.getNetworkCapabilities(network) ?: continue
                val linkProps = connectivityManager.getLinkProperties(network) ?: continue
                val name = linkProps.interfaceName ?: continue
                val isEthernet = caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET) ||
                        caps.hasTransport(NetworkCapabilities.TRANSPORT_USB)
                if (!isEthernet) continue
                if (name == "eth0") return name // preferred — GM AAOS USB NIC
                if (fallback == null) fallback = name
            }
        } catch (e: Exception) {
            Log.w(TAG, "Ethernet interface scan failed: ${e.message}")
        }
        return fallback
    }

    fun toggleStats() {
        _showStats.value = !_showStats.value
    }

    /** Forward a touch event from the projection surface to the bridge. */
    fun onTouchEvent(event: MotionEvent, surfaceWidth: Int, surfaceHeight: Int) {
        val stats = _videoStats.value
        if (stats.width <= 0 || stats.height <= 0) return
        touchForwarder.onTouch(event, surfaceWidth, surfaceHeight, stats.width, stats.height)
    }

    /** Handle a steering wheel key event. Returns true if consumed. */
    fun onKeyEvent(event: KeyEvent): Boolean {
        return steeringWheelController.onKeyEvent(event)
    }

    /** Called when the SurfaceView surface is created or changed. */
    fun onSurfaceAvailable(surface: Surface, width: Int, height: Int) {
        pendingSurface = surface
        pendingSurfaceWidth = width
        pendingSurfaceHeight = height

        // Debounce surface changes — AAOS animates surface size on launch (788→864 in ~30 steps).
        // Without debounce, each step resets the codec, losing the codec config frame.
        surfaceDebounceJob?.cancel()
        surfaceDebounceJob = viewModelScope.launch {
            kotlinx.coroutines.delay(150)
            Log.d(TAG, "Surface stabilized at ${width}x${height}")
            com.openautolink.app.diagnostics.DiagnosticLog.i("video",
                "Surface stabilized: ${width}x${height}")
            sessionManager.videoDecoder?.attach(surface, width, height)
            // Surface may have attached after the bridge's SPS/PPS+IDR replay arrived,
            // meaning the IDR was dropped (codec wasn't configured yet). Request a
            // fresh keyframe so the bridge sends a new IDR now that the codec is ready.
            sessionManager.requestKeyframe()
        }
    }

    /** Called when the SurfaceView surface is destroyed. */
    fun onSurfaceDestroyed() {
        pendingSurface = null
        pendingSurfaceWidth = 0
        pendingSurfaceHeight = 0
        sessionManager.videoDecoder?.detach()
    }

    /** Attach pending surface to a newly created decoder. Called by session observer. */
    internal fun attachPendingSurface() {
        val s = pendingSurface ?: return
        sessionManager.videoDecoder?.attach(s, pendingSurfaceWidth, pendingSurfaceHeight)
    }

    private fun registerTransportNetworkCallback() {
        try {
            connectivityManager.registerNetworkCallback(
                NetworkRequest.Builder().build(),
                transportNetworkCallback,
            )
        } catch (e: Exception) {
            Log.w(TAG, "Failed to register transport network callback: ${e.message}")
        }
    }

    private fun handleTransportNetworkUpdate(network: Network, reason: String) {
        if (!isTransportNetwork(network)) return
        synchronized(trackedTransportNetworks) {
            trackedTransportNetworks.add(network.networkHandle)
        }
        requestTransportReconnect(reason)
    }

    private fun isTransportNetwork(network: Network): Boolean {
        val linkProps = connectivityManager.getLinkProperties(network)
        val interfaceName = linkProps?.interfaceName
        if (selectedNetworkInterfaceName.isNotBlank() && interfaceName == selectedNetworkInterfaceName) {
            return true
        }

        val caps = connectivityManager.getNetworkCapabilities(network) ?: return false
        return caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET) ||
            caps.hasTransport(NetworkCapabilities.TRANSPORT_USB)
    }

    private fun requestTransportReconnect(reason: String) {
        val now = SystemClock.elapsedRealtime()
        if (now - lastTransportNetworkEventAt < 750L) return
        lastTransportNetworkEventAt = now
        Log.i(TAG, "Transport network event: $reason")
    }

    override fun onCleared() {
        try {
            connectivityManager.unregisterNetworkCallback(transportNetworkCallback)
        } catch (_: Exception) {}
        sessionManager.stop()
        super.onCleared()
    }
}
