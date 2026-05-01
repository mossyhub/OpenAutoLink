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
import com.openautolink.app.data.KnownPhone
import com.openautolink.app.data.KnownPhonesStore
import com.openautolink.app.input.SteeringWheelController
import com.openautolink.app.input.TouchForwarder
import com.openautolink.app.input.TouchForwarderImpl
import com.openautolink.app.navigation.ManeuverState
import com.openautolink.app.session.SessionManager
import com.openautolink.app.session.SessionState
import com.openautolink.app.transport.direct.AaNearbyManager
import com.openautolink.app.video.VideoStats
import com.openautolink.app.diagnostics.OalLog
import com.openautolink.app.diagnostics.DiagnosticLog
import com.openautolink.app.diagnostics.FileLogWriter
import com.openautolink.app.diagnostics.LogcatCapture
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
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
    val aaPixelAspect: Int = -1,
    val aaDpi: Int = 160,
    val fileLoggingActive: Boolean = false,
    val fileLoggingPath: String? = null,
    val fileLoggingEnabled: Boolean = false,
)

class ProjectionViewModel(application: Application) : AndroidViewModel(application) {

    companion object {
        private const val TAG = "ProjectionViewModel"
    }

    private val preferences = AppPreferences.getInstance(application)
    private val knownPhonesStore = KnownPhonesStore(preferences)
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
    private val _showPhoneChooser = MutableStateFlow(false)
    private val _fileLoggingActive = MutableStateFlow(false)
    private val _fileLoggingPath = MutableStateFlow<String?>(null)
    private var fileLogWriter: FileLogWriter? = null
    private var logcatCapture: LogcatCapture? = null

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
        preferences.aaDpi,
        preferences.aaPixelAspect,
        _fileLoggingActive,
        _fileLoggingPath,
        preferences.fileLoggingEnabled,
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
            aaDpi = values[18] as Int,
            aaPixelAspect = values[19] as Int,
            fileLoggingActive = values[20] as Boolean,
            fileLoggingPath = values[21] as? String,
            fileLoggingEnabled = values[22] as Boolean,
        )
    }.stateIn(
        viewModelScope,
        SharingStarted.WhileSubscribed(5000),
        ProjectionUiState()
    )

    init {
        registerTransportNetworkCallback()

        // Collect connected phone name from Nearby
        viewModelScope.launch {
            AaNearbyManager.connectedPhoneName.collect { name ->
                _phoneName.value = name
            }
        }

        viewModelScope.launch {
            preferences.keyRemap.collect { keyRemapJson ->
                steeringWheelController.customKeyMap = parseKeyRemap(keyRemapJson)
            }
        }

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

        // Save & Reconnect replaces the decoder while the SurfaceView can stay
        // alive under the settings overlay. Reattach the current Surface to each
        // new decoder instance even when Android does not emit surfaceCreated.
        viewModelScope.launch {
            sessionManager.videoDecoderRevision.collectLatest {
                attachPendingSurface()
                val statsFlow = sessionManager.videoStats ?: return@collectLatest
                statsFlow.collect { stats ->
                    _videoStats.value = stats
                }
            }
        }
    }

    @Volatile private var hasConnected = false
    /**
     * Set while a [connect] coroutine is between claiming the slot and
     * handing off to [SessionManager.start]. Prevents the parallel-connect
     * storm we saw in early logs (21 simultaneous connect() invocations
     * each running the full resolve pipeline).
     */
    @Volatile private var connectInFlight = false
    /** Throttle for the auto-reconnect collector (Car Hotspot mode). */
    @Volatile private var lastAutoReconnectAttemptMs: Long = 0L
    private val AUTO_RECONNECT_MIN_GAP_MS = 10_000L
    /**
     * How long to wait for mDNS alone to surface a resolved phone before
     * falling back to a TCP sweep. mDNS on GM AAOS typically returns in
     * <1s; we give it 3s of headroom so the common case has zero sweep
     * activity.
     */
    private val MDNS_GRACE_MS = 3_000L

    /**
     * How long to give the warm-cache phase (parallel identity probes of
     * known phones' last-known IPs) before falling through to mDNS. A
     * successful identity probe on the local subnet typically completes
     * in <100ms; ~700ms is a generous ceiling that still feels instant.
     */
    private val WARM_CACHE_BUDGET_MS = 700L
    private val connectLock = Any()

    fun connect() {
        connect(overrideIp = null)
    }

    /**
     * Connect with an optional one-shot IP override (e.g. user picked a
     * specific phone in the Car Hotspot chooser). The override is captured
     * by-value here so a concurrent caller can't race on a shared field.
     */
    fun connect(overrideIp: String?) {
        // Open the chooser instead of auto-connecting when:
        //   - "Always ask" is on (Behavior 2), OR
        //   - No default phone is set yet (first-run or after Forget — the
        //     user hasn't told us which phone to prefer, so don't guess).
        // Explicit picks pass overrideIp and bypass this gate entirely.
        if (overrideIp == null) {
            val mode = connectionMode.value
            if (mode == AppPreferences.CONNECTION_MODE_CAR_HOTSPOT) {
                val noDefault = defaultPhoneId.value.isBlank()
                val askMode = alwaysAskPhone.value
                if (noDefault || askMode) {
                    val reason = when {
                        askMode && noDefault -> "always-ask + no default"
                        askMode -> "always-ask is on"
                        else -> "no default phone set"
                    }
                    OalLog.i(TAG, "Opening chooser instead of auto-connecting: $reason")
                    _showPhoneChooser.value = true
                    phoneDiscovery.start()
                    // No sweep here — mDNS handles the common case; user taps
                    // Scan in the chooser if it doesn't surface their phone.
                    return
                }
            }
        }
        synchronized(connectLock) {
            // Reentrancy guard. Three states:
            //   1. Already streaming/connecting → no-op.
            //   2. A previous connect() coroutine hasn't finished its
            //      resolve+start phase yet → no-op (otherwise N parallel
            //      callers each run the full pipeline; observed 21x in logs).
            //   3. Truly idle → claim the slot and proceed.
            if (hasConnected && sessionManager.sessionState.value != SessionState.IDLE) {
                sessionManager.ensureClusterAlive()
                return
            }
            if (connectInFlight) {
                OalLog.d(TAG, "connect() ignored — another connect coroutine is already in flight")
                return
            }
            connectInFlight = true
            hasConnected = true
        }
        viewModelScope.launch {
            try {
            val codec = preferences.videoCodec.first()
            val micSrc = preferences.micSource.first()
            val scalingMode = preferences.videoScalingMode.first()
            val hotspotSsid = preferences.hotspotSsid.first()
            val hotspotPassword = preferences.hotspotPassword.first()
            val directTransport = preferences.directTransport.first()
            val videoAutoNeg = preferences.videoAutoNegotiate.first()
            val aaRes = preferences.aaResolution.first()
            val aaDpi = preferences.aaDpi.first()
            OalLog.i(TAG, "Connect with aaDpi=$aaDpi aaRes=$aaRes codec=$codec autoNeg=$videoAutoNeg")
            val aaWM = preferences.aaWidthMargin.first()
            val aaHM = preferences.aaHeightMargin.first()
            val aaPA = preferences.aaPixelAspect.first()
            val aaTargetLayoutDp = preferences.aaTargetLayoutWidthDp.first()
            val videoFps = preferences.videoFps.first()
            val driveSide = preferences.driveSide.first()
            val hideClock = preferences.hideAaClock.first()
            val hideSignal = preferences.hidePhoneSignal.first()
            val hideBattery = preferences.hideBatteryLevel.first()

            // Safe area insets
            val saTop = preferences.safeAreaTop.first()
            val saBottom = preferences.safeAreaBottom.first()
            val saLeft = preferences.safeAreaLeft.first()
            val saRight = preferences.safeAreaRight.first()

            // Load key remap from preferences
            val keyRemapJson = preferences.keyRemap.first()
            steeringWheelController.customKeyMap = parseKeyRemap(keyRemapJson)

            // Load volume offsets
            val volMedia = preferences.volumeOffsetMedia.first()
            val volNav = preferences.volumeOffsetNavigation.first()
            val volAssistant = preferences.volumeOffsetAssistant.first()

            // Load manual IP for emulator testing
            val manualIpEnabled = preferences.manualIpEnabled.first()
            val manualIpFromPrefs = if (manualIpEnabled) preferences.manualIpAddress.first().takeIf { it.isNotBlank() } else null

            // Resolve the effective IP for this connect attempt:
            //   1. Explicit [overrideIp] from the Car Hotspot chooser wins.
            //   2. In Car Hotspot mode, look up the default (or first
            //      currently-discovered) phone via [phoneDiscovery] and use
            //      its IP. If discovery hasn't surfaced anything yet, wait
            //      briefly before giving up.
            //   3. Fall back to the persistent manual-IP setting.
            val mode = preferences.connectionMode.first()
            val carHotspotPhone = if (overrideIp == null && mode == AppPreferences.CONNECTION_MODE_CAR_HOTSPOT) {
                resolveCarHotspotPhone(timeoutMs = 15_000)
            } else null
            val carHotspotIp: String? = carHotspotPhone?.host
            val manualIp = overrideIp ?: carHotspotIp ?: manualIpFromPrefs
            if (mode == AppPreferences.CONNECTION_MODE_CAR_HOTSPOT) {
                OalLog.i(
                    TAG,
                    "Car Hotspot connect: overrideIp=$overrideIp resolved=$carHotspotIp final=$manualIp",
                )
                // Track which phone we're dialing so the chooser can show
                // the ACTIVE badge correctly. Explicit picks set this in
                // selectCarHotspotPhone; auto-connect sets it here.
                if (overrideIp == null && carHotspotPhone != null) {
                    val pickedId = carHotspotPhone.phoneId
                    if (!pickedId.isNullOrBlank()) {
                        _activePhoneId.value = pickedId
                    }
                }
            }

            // Load default phone name for auto-connect
            val defaultPhone = preferences.defaultPhoneName.first()
            sessionManager.setDefaultPhoneName(defaultPhone)

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
                aaTargetLayoutWidthDp = aaTargetLayoutDp,
                videoFps = videoFps,
                driveSide = driveSide,
                hideClock = hideClock,
                hideSignal = hideSignal,
                hideBattery = hideBattery,
                volumeOffsetMedia = volMedia,
                volumeOffsetNavigation = volNav,
                volumeOffsetAssistant = volAssistant,
                manualIpAddress = manualIp,
                safeAreaTop = saTop,
                safeAreaBottom = saBottom,
                safeAreaLeft = saLeft,
                safeAreaRight = saRight,
            )
            } finally {
                // Release the in-flight slot once we hand off to SessionManager.
                // sessionManager owns the post-start lifecycle from here; if it
                // settles back to IDLE the auto-reconnect collector picks up.
                connectInFlight = false
            }
        }
    }

    /**
     * Force reconnect — used by "Save & Connect" button in Settings and
     * other manual reconnect triggers.
     *
     * Stops cleanly and re-runs the full connect pipeline. In Car Hotspot
     * mode that pipeline already starts with a warm-cache probe (Phase 0
     * of [resolveCarHotspotPhone]) so reconnects are sub-second when the
     * AP re-leased the same IP, and fall through gracefully when it
     * didn't. **Don't** pass a cached IP as overrideIp here: if the IP
     * is stale, [TcpConnector] will retry forever on it because manualIp
     * mode has no fallback.
     */
    fun reconnect() {
        viewModelScope.launch {
            OalLog.i(TAG, "reconnect(): tearing down current session")
            sessionManager.stop()
            hasConnected = false
            connect(overrideIp = null)
        }
    }

    fun disconnect() {
        sessionManager.stop()
    }

    private fun parseKeyRemap(jsonString: String): Map<Int, Int> {
        if (jsonString.isBlank()) return emptyMap()
        return try {
            val json = org.json.JSONObject(jsonString)
            buildMap {
                for (key in json.keys()) {
                    put(key.toInt(), json.getInt(key))
                }
            }
        } catch (_: Exception) {
            emptyMap()
        }
    }

    // --- Multi-phone: Phone Chooser ---

    /** Discovered endpoints for the phone chooser overlay. */
    val discoveredEndpoints = AaNearbyManager.discoveredEndpoints

    /**
     * Live phone discovery for Car Hotspot mode. Runs mDNS passively while
     * projection is visible; sweep is on-demand from the chooser UI. Results
     * carry source tags ([PhoneDiscovery.Source.MDNS] / SWEEP / BOTH) so the
     * UX can show which mechanism worked.
     */
    private val phoneDiscovery = com.openautolink.app.transport.PhoneDiscovery.getInstance(application)
    val carHotspotPhones: StateFlow<List<com.openautolink.app.transport.PhoneDiscovery.DiscoveredPhone>> =
        phoneDiscovery.phones
    val carHotspotSweepActive: StateFlow<Boolean> = phoneDiscovery.isSweeping
    val carHotspotSweepProgress: StateFlow<String> = phoneDiscovery.sweepProgress

    /**
     * Current connection mode: phone-hotspot (default) or car-hotspot.
     * Drives whether the multi-phone UX is exposed in the projection screen.
     */
    val connectionMode: StateFlow<String> = preferences.connectionMode.stateIn(
        viewModelScope,
        SharingStarted.WhileSubscribed(5000),
        AppPreferences.DEFAULT_CONNECTION_MODE,
    )

    /** Persistent known-phones list, surfaced for the chooser + settings. */
    val knownPhones: StateFlow<List<KnownPhone>> = knownPhonesStore.phones.stateIn(
        viewModelScope,
        SharingStarted.WhileSubscribed(5000),
        emptyList(),
    )

    /** Currently-preferred phone_id (empty string = no default set). */
    val defaultPhoneId: StateFlow<String> = preferences.defaultPhoneId.stateIn(
        viewModelScope,
        SharingStarted.WhileSubscribed(5000),
        AppPreferences.DEFAULT_DEFAULT_PHONE_ID,
    )

    /**
     * Whether the user has opted out of auto-connecting to the saved default
     * phone (Behavior 2). When true, the chooser is shown on connect even if
     * a default exists.
     */
    val alwaysAskPhone: StateFlow<Boolean> = preferences.alwaysAskPhone.stateIn(
        viewModelScope,
        SharingStarted.WhileSubscribed(5000),
        AppPreferences.DEFAULT_ALWAYS_ASK_PHONE,
    )

    /** True while the car app is tearing down + redialing for a phone switch. */
    private val _carHotspotSwitching = MutableStateFlow(false)
    val carHotspotSwitching: StateFlow<Boolean> = _carHotspotSwitching.asStateFlow()

    /**
     * Last phone_id we deliberately dialed via the Car Hotspot flow. Used by
     * the chooser UI to mark the ACTIVE phone reliably (vs. comparing by
     * friendly_name, which is user-editable and not unique).
     */
    private val _activePhoneId = MutableStateFlow<String?>(null)
    val activePhoneId: StateFlow<String?> = _activePhoneId.asStateFlow()

    init {
        // Continuously run mDNS discovery while in Car Hotspot mode. This
        // keeps `knownPhones` "online" status fresh and lets the floating
        // switcher button surface phones the moment they appear on the AP.
        viewModelScope.launch {
            connectionMode.collect { mode ->
                if (mode == AppPreferences.CONNECTION_MODE_CAR_HOTSPOT) {
                    phoneDiscovery.start()
                } else {
                    phoneDiscovery.stop()
                }
            }
        }
        // Auto-touch known phones as their identity becomes visible. We
        // distinct on (id, name, host) tuple so identical successive
        // emissions don't drive any DataStore writes (KnownPhonesStore.touch
        // also throttles by lastSeen, but cutting off here saves the
        // suspend round-trip entirely).
        viewModelScope.launch {
            phoneDiscovery.phones
                .map { list ->
                    list.mapNotNull { p ->
                        val id = p.phoneId
                        if (id.isNullOrBlank()) null
                        else Triple(id, p.friendlyName ?: "", p.host ?: "")
                    }.toSet()
                }
                .distinctUntilChanged()
                .collect { tuples ->
                    val mode = connectionMode.value
                    if (mode != AppPreferences.CONNECTION_MODE_CAR_HOTSPOT) return@collect
                    tuples.forEach { (id, name, host) ->
                        knownPhonesStore.touch(
                            phoneId = id,
                            friendlyName = name.takeIf { it.isNotBlank() },
                            lastKnownIp = host.takeIf { it.isNotBlank() },
                        )
                    }
                }
        }
        // Clear `activePhoneId` whenever the session leaves STREAMING. The
        // ACTIVE badge in the chooser should disappear when we're no longer
        // talking to the phone we last dialed.
        viewModelScope.launch {
            sessionManager.sessionState.collect { state ->
                if (state == SessionState.IDLE) {
                    _activePhoneId.value = null
                }
            }
        }
        // Car Hotspot mode auto-reconnect: when we're idle and a phone
        // appears in discovery, kick off a connect. Throttled to one attempt
        // every [AUTO_RECONNECT_MIN_GAP_MS] so a flapping discovery flow
        // doesn't hammer connect() — sessionState transitions can briefly
        // dip back to IDLE during reconnect retries, which would otherwise
        // re-trigger this collector immediately.
        viewModelScope.launch {
            phoneDiscovery.phones
                .map { list -> list.any { it.isResolved } }
                .distinctUntilChanged()
                .collect { anyResolved ->
                    val mode = connectionMode.value
                    if (mode != AppPreferences.CONNECTION_MODE_CAR_HOTSPOT) return@collect
                    if (alwaysAskPhone.value) return@collect
                    // No default phone set → don't auto-connect; the user
                    // will pick from the chooser when they're ready.
                    if (defaultPhoneId.value.isBlank()) return@collect
                    if (!anyResolved) return@collect
                    // Bail before logging if a connect is already running or
                    // the session is already past IDLE — saves logging spam
                    // when discovery emits multiple times during sweep.
                    if (connectInFlight) return@collect
                    if (sessionManager.sessionState.value != SessionState.IDLE) return@collect
                    val now = SystemClock.elapsedRealtime()
                    if (now - lastAutoReconnectAttemptMs < AUTO_RECONNECT_MIN_GAP_MS) {
                        // Note: keep this at debug — under sweep flapping it
                        // can fire dozens of times in a few ms.
                        return@collect
                    }
                    lastAutoReconnectAttemptMs = now
                    OalLog.i(TAG, "Car Hotspot auto-reconnect: phone discovered while idle")
                    hasConnected = false
                    connect()
                }
        }
    }

    /** Whether the phone chooser overlay is showing. */
    val showPhoneChooser: StateFlow<Boolean> = _showPhoneChooser.asStateFlow()

    /** Show the phone chooser: disconnect, restart discovery showing all phones. */
    fun showPhoneChooser() {
        _showPhoneChooser.value = true
        sessionManager.stop()
        hasConnected = false
        // Temporarily clear the default filter so all phones appear in discovery,
        // but don't persist — the saved default stays unchanged.
        viewModelScope.launch {
            val savedDefault = sessionManager.getDefaultPhoneName()
            sessionManager.setDefaultPhoneName("")
            connect()
            // Restore after discovery starts (the chooser UI handles selection)
        }
    }

    /**
     * Show the Car Hotspot phone chooser. Does NOT disconnect the active
     * session — the user can browse and dismiss without interruption.
     * Switching is only triggered by an explicit pick of a different phone.
     */
    fun showCarHotspotChooser() {
        _showPhoneChooser.value = true
        // mDNS keeps running passively while in Car Hotspot mode; don't fire
        // a sweep here. The user can press the explicit "Scan" button in
        // the chooser if mDNS hasn't found their phone.
    }

    /** User explicitly requested a re-scan from inside the chooser. */
    fun rescanCarHotspotPhones() {
        kickSweep()
    }

    /**
     * Start a sweep, honoring the user's auto-vs-manual interface preference.
     * When manual is selected, only the configured interface is scanned;
     * when auto is on, the full preferred → fallback two-phase sweep runs.
     */
    private fun kickSweep() {
        viewModelScope.launch {
            val auto = try { preferences.carHotspotAutoInterface.first() } catch (_: Exception) { true }
            if (auto) {
                phoneDiscovery.startSweep()
            } else {
                val name = try { preferences.carHotspotInterfaceName.first() } catch (_: Exception) { "" }
                phoneDiscovery.startSweep(forcedInterfaceName = name.takeIf { it.isNotBlank() })
            }
        }
    }

    /**
     * User picked a phone from the Car Hotspot chooser. Persists the phone,
     * sets it as the default if no default exists yet, and triggers a
     * session reconnect to that phone's IP.
     *
     * Identity match is by `phone_id` (stable UUID), not `friendly_name`
     * (user-editable, not unique).
     *
     * The [carHotspotSwitching] flag tracks the actual session-state
     * transition: it stays true until the new session reaches STREAMING,
     * times out at 30s, or the user dismisses the projection.
     */
    fun selectCarHotspotPhone(phone: com.openautolink.app.transport.PhoneDiscovery.DiscoveredPhone) {
        _showPhoneChooser.value = false
        val phoneId = phone.phoneId
        val host = phone.host
        if (phoneId.isNullOrBlank() || host.isNullOrBlank()) {
            OalLog.w(TAG, "Cannot select phone — missing phone_id or host: $phone")
            return
        }
        viewModelScope.launch {
            // Persist into the known-phones list. Auto-promote to default
            // only if there's no default set yet.
            knownPhonesStore.upsert(
                KnownPhone(
                    phoneId = phoneId,
                    friendlyName = phone.friendlyName ?: "Phone-${phoneId.take(4)}",
                    lastSeenMs = System.currentTimeMillis(),
                    lastKnownIp = host,
                )
            )
            val currentDefault = preferences.defaultPhoneId.first()
            if (currentDefault.isBlank()) {
                preferences.setDefaultPhoneId(phoneId)
                OalLog.i(TAG, "Auto-promoted ${phone.friendlyName} to default phone")
            }

            // If this phone is already the active session, do nothing.
            if (_activePhoneId.value == phoneId) {
                OalLog.i(TAG, "Already connected to id=${phoneId.take(8)}; no switch needed")
                return@launch
            }

            OalLog.i(
                TAG,
                "Switching to ${phone.friendlyName} ($host:${phone.port}) src=${phone.source}",
            )
            _carHotspotSwitching.value = true
            _activePhoneId.value = phoneId
            sessionManager.stop()
            hasConnected = false
            connect(overrideIp = host)

            // Wait for the new session to settle. STREAMING means success;
            // any IDLE *after* we've seen at least one CONNECTING means the
            // attempt finished and bounced back without streaming (network
            // unreachable, handshake failed, etc.). 30s is the absolute
            // ceiling.
            val timeoutMs = 30_000L
            var sawConnecting = false
            val outcome = kotlinx.coroutines.withTimeoutOrNull(timeoutMs) {
                sessionManager.sessionState.first { state ->
                    if (state == SessionState.CONNECTING ||
                        state == SessionState.CONNECTED) sawConnecting = true
                    when {
                        state == SessionState.STREAMING -> true
                        state == SessionState.ERROR -> true
                        sawConnecting && state == SessionState.IDLE -> true
                        else -> false
                    }
                }
            }
            when (outcome) {
                null -> {
                    OalLog.w(TAG, "Switch to id=${phoneId.take(8)} timed out after ${timeoutMs}ms")
                    _activePhoneId.value = null
                }
                SessionState.STREAMING -> {
                    OalLog.i(TAG, "Switch to id=${phoneId.take(8)} succeeded")
                }
                else -> {
                    OalLog.w(
                        TAG,
                        "Switch to id=${phoneId.take(8)} failed: state settled to $outcome",
                    )
                    _activePhoneId.value = null
                }
            }
            _carHotspotSwitching.value = false
        }
    }

    /** Mark a phone as the auto-connect default and persist. */
    fun setDefaultPhoneId(phoneId: String) {
        viewModelScope.launch {
            preferences.setDefaultPhoneId(phoneId)
            OalLog.i(TAG, "Default phone set to id=${phoneId.take(8)}")
        }
    }

    /** Forget a known phone (also clears it as default if it was set). */
    fun forgetKnownPhone(phoneId: String) {
        viewModelScope.launch {
            knownPhonesStore.remove(phoneId)
        }
    }

    /** Toggle Behavior 2: always show chooser instead of auto-connecting. */
    fun setAlwaysAskPhone(enabled: Boolean) {
        viewModelScope.launch { preferences.setAlwaysAskPhone(enabled) }
    }

    /**
     * Resolve a usable phone for the Car Hotspot connect flow.
     *
     * Strategy: **warm cache → mDNS → sweep.** Each phase only runs if the
     * previous one didn't find the phone:
     *
     *   - **Phase 0 (warm cache):** parallel-probe last-known IPs of all
     *     known phones. Automotive APs almost always re-lease the same IP
     *     to the same MAC, so this typically returns in <500ms with no
     *     mDNS / sweep traffic at all.
     *   - **Phase 1 (mDNS):** cheap, passive, usually returns on AAOS 14+.
     *     Grace window of [MDNS_GRACE_MS].
     *   - **Phase 2 (sweep):** full TCP sweep of preferred → fallback
     *     interfaces. Only fires if both phase 0 and phase 1 came up
     *     empty.
     *
     * Preference order for which phone to pick out of the discovery flow:
     *   1. Default phone (matching [AppPreferences.defaultPhoneId]) when
     *      it has a resolved host.
     *   2. Any currently-discovered phone with a resolved host.
     *   3. null if nothing comes up before [timeoutMs] elapses.
     */
    private suspend fun resolveCarHotspotPhone(
        timeoutMs: Long,
    ): com.openautolink.app.transport.PhoneDiscovery.DiscoveredPhone? {
        // Make sure discovery is actually running. The init-block flow starts
        // it on connectionMode change, but the user might call connect()
        // before that emit lands.
        phoneDiscovery.start()

        val defaultId = try {
            preferences.defaultPhoneId.first().takeIf { it.isNotBlank() }
        } catch (_: Exception) { null }

        // Phase 0: warm cache hit. Build the target list with the default
        // phone's last-known IP first so the most-likely-to-succeed probe
        // gets to race in parallel with the others.
        val knownTargets: List<com.openautolink.app.transport.PhoneDiscovery.KnownTarget> = try {
            val knownNow = knownPhonesStore.phones.first()
            knownNow
                .mapNotNull { kp ->
                    val ip = kp.lastKnownIp ?: return@mapNotNull null
                    com.openautolink.app.transport.PhoneDiscovery.KnownTarget(
                        expectedPhoneId = kp.phoneId,
                        host = ip,
                    )
                }
                .sortedByDescending { it.expectedPhoneId == defaultId }
        } catch (_: Exception) { emptyList() }

        if (knownTargets.isNotEmpty()) {
            val cacheHit = kotlinx.coroutines.withTimeoutOrNull(WARM_CACHE_BUDGET_MS) {
                phoneDiscovery.probeKnown(knownTargets)
            } == true
            if (cacheHit) {
                // probeKnown added results into the discovery flow — read it
                // back through pickBestPhone so we honor the default-phone
                // preference if the default was actually one of the hits.
                val picked = pickBestPhone(phoneDiscovery.phones.value, defaultId)
                if (picked != null) {
                    OalLog.i(
                        TAG,
                        "Resolved via warm cache hit on ${picked.host} (${picked.friendlyName ?: picked.phoneId?.take(8)})",
                    )
                    return picked
                }
            }
        }

        // Phase 1: mDNS-only grace period. Cheapest, fastest, no socket
        // pressure on the network.
        val mdnsResult = kotlinx.coroutines.withTimeoutOrNull(MDNS_GRACE_MS) {
            phoneDiscovery.phones
                .map { list -> pickBestPhone(list, defaultId) }
                .first { it != null }
        }
        if (mdnsResult != null) {
            OalLog.i(TAG, "Resolved via mDNS in <${MDNS_GRACE_MS}ms")
            return mdnsResult
        }

        // Phase 2: kick a sweep and keep watching the same flow. mDNS keeps
        // running too — whichever fills in the host first wins.
        OalLog.i(TAG, "mDNS grace expired with no resolved phone; falling back to sweep")
        kickSweep()
        val remaining = (timeoutMs - WARM_CACHE_BUDGET_MS - MDNS_GRACE_MS).coerceAtLeast(1_000L)
        return kotlinx.coroutines.withTimeoutOrNull(remaining) {
            phoneDiscovery.phones
                .map { list -> pickBestPhone(list, defaultId) }
                .first { it != null }
        }
    }

    /**
     * Pick the most appropriate phone from the current discovery snapshot.
     * Prefers the default phone if it's currently resolved; otherwise the
     * first resolved phone in the list. Returns null if nothing is resolved.
     */
    private fun pickBestPhone(
        list: List<com.openautolink.app.transport.PhoneDiscovery.DiscoveredPhone>,
        defaultId: String?,
    ): com.openautolink.app.transport.PhoneDiscovery.DiscoveredPhone? {
        // Defensive: drop IPv6 link-local hosts that slipped through. They
        // aren't usable for TCP without a scope ID (e.g. %wlan0).
        val resolved = list.filter {
            it.isResolved && !it.host.isNullOrBlank() && !isUnusableHost(it.host)
        }
        if (resolved.isEmpty()) return null
        if (defaultId != null) {
            resolved.firstOrNull { it.phoneId == defaultId }?.let { return it }
        }
        return resolved.first()
    }

    private fun isUnusableHost(host: String): Boolean {
        // Cheap textual check — `fe80:` prefix covers IPv6 link-local. Avoids
        // creating an InetAddress just for filtering.
        return host.startsWith("fe80:", ignoreCase = true)
    }

    /** User selected a phone from the chooser — connect without changing default. */
    fun selectPhone(endpointId: String, phoneName: String) {
        _showPhoneChooser.value = false
        // Connect to the selected endpoint without saving as default
        sessionManager.connectToNearbyEndpoint(endpointId)
    }

    /**
     * Close the phone chooser without picking a new phone.
     *
     * In **Car Hotspot mode** this is purely a UI op — the chooser overlays
     * a live session and should never tear it down on dismiss. The user's
     * intent is "I changed my mind / closed the picker."
     *
     * In legacy **Nearby/phone-hotspot mode** the chooser was opened by
     * actively disconnecting (so the user could pick from the discovery
     * list), so dismissing has to restore the default phone and reconnect.
     */
    fun dismissPhoneChooser() {
        _showPhoneChooser.value = false
        if (connectionMode.value == AppPreferences.CONNECTION_MODE_CAR_HOTSPOT) {
            // Car Hotspot: chooser was opened over a live session. Do nothing.
            return
        }
        // Legacy path: chooser was opened with the session torn down — restore.
        viewModelScope.launch {
            val savedDefault = preferences.defaultPhoneName.first()
            sessionManager.setDefaultPhoneName(savedDefault)
            hasConnected = false
            connect()
        }
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

    private var fileLogToggleLock = Any()

    fun toggleFileLogging() {
        synchronized(fileLogToggleLock) {
            if (_fileLoggingActive.value) {
                // Stop
                logcatCapture?.stop()
                logcatCapture = null
                fileLogWriter?.stop()
                DiagnosticLog.fileLogWriter = null
                fileLogWriter = null
                _fileLoggingActive.value = false
                _fileLoggingPath.value = null
            } else {
                // Start
                val writer = FileLogWriter(getApplication())
                val path = writer.start()
                if (path != null) {
                    fileLogWriter = writer
                    DiagnosticLog.fileLogWriter = writer
                    _fileLoggingActive.value = true
                    _fileLoggingPath.value = path
                    // Write existing ring buffer entries so we have context
                    writer.writeExistingLogs(DiagnosticLog.localLogs.value)

                    // Optionally start logcat capture if enabled in settings
                    viewModelScope.launch {
                        val captureEnabled = preferences.logcatCaptureEnabled.first()
                        if (captureEnabled) {
                            val logDir = java.io.File(path).parentFile
                            if (logDir != null) {
                                val capture = LogcatCapture()
                                capture.start(logDir)
                                logcatCapture = capture
                            }
                        }
                    }
                }
            }
        }
    }

    /** Forward a touch event from the projection surface to the bridge. */
    fun onTouchEvent(event: MotionEvent, surfaceWidth: Int, surfaceHeight: Int) {
        // Use video stats dimensions — the phone renders AA at whatever resolution
        // it negotiated, and touch coordinates map to that render space.
        val stats = _videoStats.value
        val tw = if (stats.width > 0) stats.width else sessionManager.touchWidth.value
        val th = if (stats.height > 0) stats.height else sessionManager.touchHeight.value
        if (tw <= 0 || th <= 0) return
        if (event.actionMasked == MotionEvent.ACTION_DOWN) {
            val sx = event.x * tw / surfaceWidth
            val sy = event.y * th / surfaceHeight
            Log.d("TouchDebug", "surface=${surfaceWidth}x${surfaceHeight} touch=${tw}x${th} raw=(${event.x.toInt()},${event.y.toInt()}) scaled=(${sx.toInt()},${sy.toInt()})")
        }
        touchForwarder.onTouch(event, surfaceWidth, surfaceHeight, tw, th)
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
        // Stop file logging if active
        logcatCapture?.stop()
        fileLogWriter?.stop()
        DiagnosticLog.fileLogWriter = null
        sessionManager.stop()
        super.onCleared()
    }
}
