package com.openautolink.app.session

import android.content.Context
import android.media.AudioManager
import android.os.SystemClock
import android.util.Log
import com.openautolink.app.audio.AudioPlayer
import com.openautolink.app.audio.AudioPlayerImpl
import com.openautolink.app.audio.AudioStats
import com.openautolink.app.audio.CallState
import com.openautolink.app.audio.MicCaptureManager
import com.openautolink.app.cluster.ClusterNavigationState
import com.openautolink.app.data.AppPreferences
import com.openautolink.app.diagnostics.DiagnosticLevel
import com.openautolink.app.diagnostics.DiagnosticLog
import com.openautolink.app.diagnostics.RemoteDiagnostics
import com.openautolink.app.diagnostics.RemoteDiagnosticsImpl
import com.openautolink.app.diagnostics.TelemetryCollector
import com.openautolink.app.input.GnssForwarder
import com.openautolink.app.input.GnssForwarderImpl
import com.openautolink.app.input.ImuForwarder
import com.openautolink.app.input.VehicleDataForwarder
import com.openautolink.app.input.VehicleDataForwarderImpl
import com.openautolink.app.media.OalMediaBrowserService
import com.openautolink.app.media.OalMediaSessionManager
import com.openautolink.app.navigation.ManeuverState
import com.openautolink.app.navigation.NavigationDisplay
import com.openautolink.app.navigation.NavigationDisplayImpl
import com.openautolink.app.proto.Control
import com.openautolink.app.transport.AudioPurpose
import com.openautolink.app.transport.ConnectionState
import com.openautolink.app.transport.ControlMessage
import com.openautolink.app.transport.direct.AaMessageConverter
import com.openautolink.app.transport.direct.AaNearbyManager
import com.openautolink.app.transport.direct.DirectAaSession
import com.openautolink.app.transport.direct.DirectServiceDiscovery
import com.openautolink.app.video.DecoderState
import com.openautolink.app.video.MediaCodecDecoder
import com.openautolink.app.video.VideoDecoder
import com.openautolink.app.video.VideoStats
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.Job
import kotlinx.coroutines.asCoroutineDispatcher
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch

/**
 * Session orchestrator â€” connects component islands, manages lifecycle.
 * Direct mode only â€” speaks AA protocol directly to the phone.
 */
class SessionManager(
    externalScope: CoroutineScope,
    private val context: Context? = null,
    private val audioManager: AudioManager? = null
) {

    companion object {
        private const val TAG = "SessionManager"

        @Volatile
        private var instance: SessionManager? = null

        fun getInstance(scope: CoroutineScope, context: Context, audioManager: AudioManager): SessionManager {
            return instance ?: synchronized(this) {
                instance ?: SessionManager(scope, context, audioManager).also { instance = it }
            }
        }

        fun instanceOrNull(): SessionManager? = instance
    }

    private val scope = CoroutineScope(SupervisorJob() + kotlinx.coroutines.Dispatchers.Main)

    // Direct mode session â€” speaks AA wire protocol directly to phone
    private var directSession: DirectAaSession? = null

    // Dedicated single-threaded dispatcher for video decode
    private val videoDispatcher = java.util.concurrent.Executors.newSingleThreadExecutor { r ->
        Thread(r, "VideoDecodeInput").apply { isDaemon = true }
    }.asCoroutineDispatcher()

    // Dedicated dispatcher for audio
    private val audioDispatcher = java.util.concurrent.Executors.newSingleThreadExecutor { r ->
        Thread(r, "AudioFrameInput").apply {
            isDaemon = true
            priority = Thread.MAX_PRIORITY
        }
    }.asCoroutineDispatcher()

    private val _sessionState = MutableStateFlow(SessionState.IDLE)
    val sessionState: StateFlow<SessionState> = _sessionState.asStateFlow()

    private val _statusMessage = MutableStateFlow("Ready")
    val statusMessage: StateFlow<String> = _statusMessage.asStateFlow()

    // Video decoder
    private var _videoDecoder: VideoDecoder? = null
    val videoDecoder: VideoDecoder? get() = _videoDecoder
    val videoStats: StateFlow<VideoStats>? get() = _videoDecoder?.stats
    val decoderState: StateFlow<DecoderState>? get() = _videoDecoder?.decoderState

    // Audio player
    private var _audioPlayer: AudioPlayer? = null
    val audioPlayer: AudioPlayer? get() = _audioPlayer
    val audioStats: StateFlow<AudioStats>? get() = _audioPlayer?.stats

    // Mic capture
    private var _micCaptureManager: MicCaptureManager? = null
    private var micSource: String = "car"

    val callState: StateFlow<CallState>? get() = _audioPlayer?.callState

    // GNSS forwarder
    private var _gnssForwarder: GnssForwarder? = null

    // Vehicle data forwarder
    private var _vehicleDataForwarder: VehicleDataForwarder? = null
    val vehicleData: StateFlow<ControlMessage.VehicleData>?
        get() = _vehicleDataForwarder?.latestVehicleData

    // IMU forwarder
    private var _imuForwarder: ImuForwarder? = null

    // Direct mode location listener
    private var _directLocationListener: android.location.LocationListener? = null

    // Navigation display
    private val _navigationDisplay: NavigationDisplay = NavigationDisplayImpl()
    val navigationDisplay: NavigationDisplay get() = _navigationDisplay

    // Diagnostics
    private var _remoteDiagnostics: RemoteDiagnosticsImpl? = null
    val remoteDiagnostics: RemoteDiagnostics? get() = _remoteDiagnostics
    private var _telemetryCollector: TelemetryCollector? = null

    val currentManeuver: StateFlow<ManeuverState?>
        get() = _navigationDisplay.currentManeuver

    // Phone battery
    private val _phoneBatteryLevel = MutableStateFlow<Int?>(null)
    val phoneBatteryLevel: StateFlow<Int?> = _phoneBatteryLevel.asStateFlow()
    private val _phoneBatteryCritical = MutableStateFlow(false)
    val phoneBatteryCritical: StateFlow<Boolean> = _phoneBatteryCritical.asStateFlow()

    // Voice session
    private val _voiceSessionActive = MutableStateFlow(false)
    val voiceSessionActive: StateFlow<Boolean> = _voiceSessionActive.asStateFlow()

    // Phone signal
    private val _phoneSignalStrength = MutableStateFlow<Int?>(null)
    val phoneSignalStrength: StateFlow<Int?> = _phoneSignalStrength.asStateFlow()

    // WiFi frequency (from Nearby's underlying WiFi Direct)
    val wifiFrequencyMhz: StateFlow<Int> = AaNearbyManager.wifiFrequencyMhz

    // Media session
    private var _mediaSessionManager: OalMediaSessionManager? = null

    // Cluster manager
    private var _clusterManager: com.openautolink.app.cluster.ClusterManager? = null

    private var observeJob: Job? = null
    private var decoderWatchJob: Job? = null
    private var keyframeWatchJob: Job? = null
    private var callStateJob: Job? = null

    // Track last known active time for sleep/wake detection
    private var lastActiveTimestamp = SystemClock.elapsedRealtime()

    fun start(
        codecPreference: String = "h264",
        micSourcePreference: String = "car",
        scalingMode: String = "letterbox",
        directTransport: String = "nearby",
        hotspotSsid: String = "",
        hotspotPassword: String = "",
        videoAutoNegotiate: Boolean = true,
        aaResolution: String = "1080p",
        aaDpi: Int = 160,
        aaWidthMargin: Int = 0,
        aaHeightMargin: Int = 0,
        aaPixelAspect: Int = 0,
        videoFps: Int = 60,
        driveSide: String = "left",
        hideClock: Boolean = false,
        hideSignal: Boolean = false,
        hideBattery: Boolean = false,
        volumeOffsetMedia: Int = 0,
        volumeOffsetNavigation: Int = 0,
        volumeOffsetAssistant: Int = 0,
    ) {
        micSource = micSourcePreference
        observeJob?.cancel()

        // Create video decoder
        _videoDecoder?.release()
        _videoDecoder = MediaCodecDecoder(codecPreference, scalingMode)

        // Create audio player
        _audioPlayer?.release()
        _audioPlayer = audioManager?.let { AudioPlayerImpl(it) }
        _audioPlayer?.initialize()
        // Apply volume offsets to the audio coordinator
        (_audioPlayer as? AudioPlayerImpl)?.coordinator?.let { coord ->
            coord.volumeOffsetMedia = volumeOffsetMedia
            coord.volumeOffsetNavigation = volumeOffsetNavigation
            coord.volumeOffsetAssistant = volumeOffsetAssistant
        }

        // Create mic capture â€” sends frames via DirectAaSession
        _micCaptureManager?.release()
        _micCaptureManager = MicCaptureManager { frame ->
            directSession?.let { session ->
                scope.launch { session.sendMicAudio(frame.data) }
            }
        }

        // Create GNSS forwarder (NMEA not used in direct mode â€” LocationListener used instead)
        _gnssForwarder?.stop()
        _directLocationListener = null
        _gnssForwarder = context?.let { ctx ->
            GnssForwarderImpl(ctx) { _ -> /* NMEA not used in direct mode */ }
        }

        // Create vehicle data forwarder â€” sends via DirectAaSession
        _vehicleDataForwarder?.stop()
        _vehicleDataForwarder = context?.let { ctx ->
            VehicleDataForwarderImpl(
                ctx,
                sendMessage = { vehicleData ->
                    val session = directSession ?: return@VehicleDataForwarderImpl
                    scope.launch {
                        session.sendMessage(AaMessageConverter.vehicleDataToProto(vehicleData))
                        AaMessageConverter.buildVemSensorBatch(vehicleData)?.let {
                            session.sendMessage(it)
                        }
                    }
                },
                onIgnitionOn = { /* direct mode doesn't need ignition-based reconnect */ }
            )
        }

        // Create IMU forwarder â€” sends via DirectAaSession
        _imuForwarder?.stop()
        _imuForwarder = context?.let { ctx ->
            ImuForwarder(ctx) { imuData ->
                val session = directSession ?: return@ImuForwarder
                scope.launch {
                    session.sendMessage(AaMessageConverter.vehicleDataToProto(imuData))
                }
            }
        }

        // Create media session for AAOS system UI integration
        _mediaSessionManager?.release()
        _mediaSessionManager = context?.let { OalMediaSessionManager(it) }
        _mediaSessionManager?.initialize()
        _mediaSessionManager?.getSessionToken()?.let { token ->
            OalMediaBrowserService.updateSessionToken(token)
        }

        // Enable cluster service
        _clusterManager?.release()
        _clusterManager = context?.let { com.openautolink.app.cluster.ClusterManager(it) }
        _clusterManager?.setClusterEnabled(true)
        _clusterManager?.launchClusterBinding()

        // Create diagnostics (local-only)
        _telemetryCollector?.stop()
        _remoteDiagnostics = RemoteDiagnosticsImpl()
        DiagnosticLog.instance = _remoteDiagnostics
        _telemetryCollector = TelemetryCollector(scope, _remoteDiagnostics!!, _sessionState)
        _telemetryCollector?.videoDecoder = _videoDecoder
        _telemetryCollector?.audioPlayer = _audioPlayer
        _telemetryCollector?.start()

        observeJob = scope.launch {
            // Watch for decoder errors
            decoderWatchJob?.cancel()
            decoderWatchJob = launch { watchDecoderState() }

            // Watch for IDR starvation
            keyframeWatchJob?.cancel()
            keyframeWatchJob = launch { watchKeyframeNeeds() }

            // Watch call state for mic purpose routing
            callStateJob?.cancel()
            callStateJob = launch { watchCallState() }

            // Start direct mode session
            startSession(directTransport, hotspotSsid, hotspotPassword,
                videoAutoNegotiate, codecPreference, aaResolution, aaDpi,
                aaWidthMargin, aaHeightMargin, aaPixelAspect, videoFps,
                driveSide, hideClock, hideSignal, hideBattery)
        }
    }

    private fun startSession(
        directTransport: String, hotspotSsid: String, hotspotPassword: String,
        videoAutoNegotiate: Boolean = true, codec: String = "h264",
        aaResolution: String = "1080p", aaDpi: Int = 160,
        aaWidthMargin: Int = 0, aaHeightMargin: Int = 0, aaPixelAspect: Int = 0, videoFps: Int = 60,
        driveSide: String = "left",
        hideClock: Boolean = false, hideSignal: Boolean = false, hideBattery: Boolean = false,
    ) {
        directSession?.stop()
        val ctx = context ?: return
        val session = DirectAaSession(scope, ctx)

        // Get the actual projection surface dimensions for pixel_aspect auto-computation.
        // Use the full display bounds — in projection mode the app is always fullscreen
        // (immersive mode hides system bars). The pixel_aspect tells AA about the
        // physical display shape so it renders UI correctly on wide displays.
        // DO NOT subtract system bar insets: getInsetsIgnoringVisibility() returns
        // non-zero values even when bars are hidden, which would incorrectly shrink
        // the display dimensions and produce wrong pixel_aspect values.
        val wm = ctx.getSystemService(Context.WINDOW_SERVICE) as android.view.WindowManager
        val metrics = wm.currentWindowMetrics
        val displayW = metrics.bounds.width()
        val displayH = metrics.bounds.height()
        Log.i(TAG, "Display for pixel_aspect: ${displayW}x${displayH}")

        // Map resolution string to pixel dimensions
        val (resW, resH) = when (aaResolution) {
            "480p" -> 800 to 480
            "720p" -> 1280 to 720
            "1440p" -> 2560 to 1440
            "4k" -> 3840 to 2160
            else -> 1920 to 1080 // "1080p" default
        }

        // Map codec preference to ServiceDiscovery codec string
        val sdCodec = if (videoAutoNegotiate) "auto" else when (codec) {
            "h265" -> "H.265"
            "vp9" -> "VP9"
            else -> "H.264"
        }

        session.videoConfig = DirectServiceDiscovery.VideoConfig(
            width = resW, height = resH, fps = videoFps, dpi = aaDpi,
            codec = sdCodec,
            marginWidth = aaWidthMargin, marginHeight = aaHeightMargin,
            pixelAspectE4 = aaPixelAspect,
            displayWidth = displayW, displayHeight = displayH,
        )
        // Set vehicle identity from VHAL data or defaults
        val vd = _vehicleDataForwarder?.latestVehicleData?.value
        val driverPos = if (driveSide == "right") Control.DriverPosition.DRIVER_POSITION_RIGHT
            else Control.DriverPosition.DRIVER_POSITION_LEFT
        session.vehicleIdentity = DirectServiceDiscovery.VehicleIdentity(
            make = vd?.carMake ?: "OpenAutoLink",
            model = vd?.carModel ?: "Direct",
            year = vd?.carYear ?: "2024",
            driverPosition = driverPos,
        )
        // Set AA UI hide flags
        session.hideClock = hideClock
        session.hideSignal = hideSignal
        session.hideBattery = hideBattery

        // Bluetooth service — get car's BT MAC for phone pairing
        try {
            @Suppress("MissingPermission")
            val btAdapter = android.bluetooth.BluetoothAdapter.getDefaultAdapter()
            val btMac = btAdapter?.address ?: ""
            session.btMacAddress = btMac
            if (btMac.isNotEmpty() && btMac != "02:00:00:00:00:00") {
                Log.i(TAG, "Bluetooth MAC for AA: $btMac")
            }
        } catch (e: Exception) {
            Log.w(TAG, "Could not get BT MAC: ${e.message}")
        }

        // AAC audio — reduces bandwidth ~10x vs PCM over WiFi
        session.useAacAudio = true

        session.hotspotSsid = hotspotSsid
        session.hotspotPassword = hotspotPassword
        session.directTransport = directTransport
        directSession = session

        // Observe session state
        scope.launch {
            session.connectionState.collect { connState ->
                val newState = connState.toSessionState()
                _sessionState.value = newState
                _statusMessage.value = when (newState) {
                    SessionState.IDLE -> {
                        when (directTransport) {
                            "nearby" -> "Nearby: ${AaNearbyManager.status.value}"
                            "native" -> "Native: BT+WiFi Direct | TCP:5288"
                            "hotspot" -> "Hotspot: BT+TCP:5288"
                            else -> "Waiting for phone..."
                        }
                    }
                    SessionState.CONNECTING -> "Phone connecting..."
                    SessionState.CONNECTED -> "Handshake..."
                    SessionState.STREAMING -> "Streaming"
                    SessionState.ERROR -> "Error"
                }
                if (newState == SessionState.STREAMING) {
                    startDirectLocationForwarding(session)
                    _vehicleDataForwarder?.start()
                    _imuForwarder?.start()
                }
            }
        }

        // Observe control messages
        scope.launch {
            session.controlMessages.collect { message ->
                lastActiveTimestamp = SystemClock.elapsedRealtime()
                handleControlMessage(message)
            }
        }

        // Collect video frames
        scope.launch(videoDispatcher) {
            session.videoFrames.collect { frame ->
                _videoDecoder?.onFrame(frame)
            }
        }

        // Collect audio frames
        scope.launch(audioDispatcher) {
            session.audioFrames.collect { frame ->
                _audioPlayer?.onAudioFrame(frame)
            }
        }

        // Observe Nearby status
        scope.launch {
            AaNearbyManager.status.collect { nearbyStatus ->
                if (_sessionState.value == SessionState.IDLE) {
                    _statusMessage.value = "Nearby: $nearbyStatus"
                }
            }
        }

        session.start()
        Log.i(TAG, "Direct mode started (transport=$directTransport)")
    }

    @android.annotation.SuppressLint("MissingPermission")
    private fun startDirectLocationForwarding(session: DirectAaSession) {
        stopDirectLocationForwarding()
        val ctx = context ?: return
        val lm = ctx.getSystemService(Context.LOCATION_SERVICE) as? android.location.LocationManager ?: return
        if (!lm.isProviderEnabled(android.location.LocationManager.GPS_PROVIDER)) {
            Log.w(TAG, "GPS provider not enabled")
            return
        }

        val listener = android.location.LocationListener { location ->
            scope.launch {
                session.sendMessage(AaMessageConverter.locationToProto(location))
            }
        }
        try {
            lm.requestLocationUpdates(
                android.location.LocationManager.GPS_PROVIDER,
                500L, 0f, listener, android.os.Looper.getMainLooper(),
            )
            _directLocationListener = listener
            Log.i(TAG, "GPS forwarding started")
        } catch (e: SecurityException) {
            Log.w(TAG, "GPS permission denied: ${e.message}")
        }
    }

    private fun stopDirectLocationForwarding() {
        _directLocationListener?.let { listener ->
            val ctx = context ?: return
            val lm = ctx.getSystemService(Context.LOCATION_SERVICE) as? android.location.LocationManager
            lm?.removeUpdates(listener)
        }
        _directLocationListener = null
    }

    fun stop() {
        observeJob?.cancel()
        observeJob = null
        decoderWatchJob?.cancel()
        decoderWatchJob = null
        keyframeWatchJob?.cancel()
        keyframeWatchJob = null
        callStateJob?.cancel()
        callStateJob = null
        directSession?.stop()
        directSession = null
        stopDirectLocationForwarding()
        _videoDecoder?.release()
        _videoDecoder = null
        _audioPlayer?.release()
        _audioPlayer = null
        _micCaptureManager?.release()
        _micCaptureManager = null
        _gnssForwarder?.stop()
        _gnssForwarder = null
        _vehicleDataForwarder?.stop()
        _vehicleDataForwarder = null
        _imuForwarder?.stop()
        _imuForwarder = null
        _navigationDisplay.clear()
        ClusterNavigationState.clear()
        _mediaSessionManager?.release()
        _mediaSessionManager = null
        _clusterManager?.release()
        _clusterManager = null
        _telemetryCollector?.stop()
        _telemetryCollector = null
        DiagnosticLog.instance = null
        _remoteDiagnostics = null
        _sessionState.value = SessionState.IDLE
        _statusMessage.value = "Disconnected"
        _phoneBatteryLevel.value = null
        _phoneBatteryCritical.value = false
        _voiceSessionActive.value = false
        _phoneSignalStrength.value = null
    }

    /**
     * Called from Activity.onResume() to detect system sleep/wake.
     */
    fun onSystemWake() {
        val now = SystemClock.elapsedRealtime()
        val elapsed = now - lastActiveTimestamp
        lastActiveTimestamp = now
        if (elapsed < 10_000) return
        val state = _sessionState.value
        if (state == SessionState.IDLE) return

        Log.i(TAG, "System wake detected (${elapsed / 1000}s gap, state=$state)")
        DiagnosticLog.i("transport", "System wake detected (${elapsed / 1000}s gap)")
        // Direct mode: the server socket is still listening, phone will reconnect
        // No explicit force-reconnect needed â€” DirectAaSession accepts new connections
    }

    suspend fun requestKeyframe() {
        directSession?.requestKeyframe()
    }

    private suspend fun syncLocalPreferences() {
        val ctx = context ?: return
        try {
            val prefs = AppPreferences.getInstance(ctx)
            ClusterNavigationState.distanceUnits = prefs.distanceUnits.first()
        } catch (e: Exception) {
            Log.w(TAG, "Failed to sync local preferences: ${e.message}")
        }
    }

    suspend fun sendControlMessage(message: ControlMessage) {
        val session = directSession ?: return
        when (message) {
            is ControlMessage.Touch -> session.sendMessage(
                AaMessageConverter.touchToProto(message)
            )
            is ControlMessage.VehicleData -> {
                session.sendMessage(AaMessageConverter.vehicleDataToProto(message))
                AaMessageConverter.buildVemSensorBatch(message)?.let {
                    session.sendMessage(it)
                }
            }
            is ControlMessage.Button -> session.sendMessage(
                AaMessageConverter.buttonToProto(message)
            )
            is ControlMessage.KeyframeRequest -> session.requestKeyframe()
            is ControlMessage.Gnss -> {
                // Direct mode: phone has its own GPS via hotspot
            }
            else -> {}
        }
    }

    fun ensureClusterAlive() {
        _clusterManager?.ensureAlive()
    }

    private suspend fun watchDecoderState() {
        while (_videoDecoder == null) { delay(500) }
        _videoDecoder?.decoderState?.collect { state ->
            if (state == DecoderState.ERROR) {
                Log.w(TAG, "Decoder error â€” initiating recovery")
                _remoteDiagnostics?.log(DiagnosticLevel.ERROR, "video", "Decoder error â€” recovery")
                _statusMessage.value = "Video error â€” recovering..."
                recoverDecoder()
            }
        }
    }

    private suspend fun recoverDecoder() {
        delay(500)
        _videoDecoder?.let { decoder ->
            decoder.resume()
            requestKeyframe()
            Log.i(TAG, "Decoder recovery: resumed codec, requested keyframe")
        }
    }

    private suspend fun watchKeyframeNeeds() {
        while (_videoDecoder == null) { delay(500) }
        val decoder = _videoDecoder ?: return
        decoder.needsKeyframe.collect { needed ->
            if (needed) {
                var attempt = 0
                while (decoder.needsKeyframe.value) {
                    attempt++
                    requestKeyframe()
                    if (attempt == 1) {
                        Log.i(TAG, "Keyframe re-request #$attempt")
                    } else {
                        Log.w(TAG, "Keyframe re-request #$attempt (still waiting)")
                        _remoteDiagnostics?.log(DiagnosticLevel.WARN, "video",
                            "Keyframe re-request #$attempt")
                    }
                    delay(2000)
                }
            }
        }
    }

    private suspend fun watchCallState() {
        val player = _audioPlayer ?: return
        player.callState.collect { state ->
            val purpose = when (state) {
                CallState.IN_CALL -> AudioPurpose.PHONE_CALL
                else -> AudioPurpose.ASSISTANT
            }
            _micCaptureManager?.setMicPurpose(purpose)
            Log.d(TAG, "Call state: $state â€” mic purpose: $purpose")
        }
    }

    private fun handleControlMessage(message: ControlMessage) {
        when (message) {
            is ControlMessage.PhoneConnected -> {
                _remoteDiagnostics?.log(DiagnosticLevel.INFO, "session", "Phone connected: ${message.phoneName}")
                _sessionState.value = SessionState.STREAMING
                _statusMessage.value = "Streaming"
                directSession?.let { startDirectLocationForwarding(it) }
                _vehicleDataForwarder?.start()
                _imuForwarder?.start()
            }
            is ControlMessage.PhoneDisconnected -> {
                _remoteDiagnostics?.log(DiagnosticLevel.INFO, "session", "Phone disconnected: ${message.reason}")
                _gnssForwarder?.stop()
                _vehicleDataForwarder?.stop()
                _imuForwarder?.stop()
                stopDirectLocationForwarding()
                _navigationDisplay.clear()
                ClusterNavigationState.clear()
            }
            is ControlMessage.NavState -> {
                _navigationDisplay.onNavState(message)
                _navigationDisplay.currentManeuver.value?.let { maneuver ->
                    ClusterNavigationState.update(maneuver)
                }
            }
            is ControlMessage.NavStateClear -> {
                _navigationDisplay.clear()
                ClusterNavigationState.clear()
            }
            is ControlMessage.MediaMetadata -> {
                _mediaSessionManager?.updateMetadata(
                    title = message.title, artist = message.artist, album = message.album,
                    durationMs = message.durationMs, albumArtBase64 = message.albumArtBase64
                )
                if (message.playing != null) {
                    _mediaSessionManager?.updatePlaybackState(
                        playing = message.playing, positionMs = message.positionMs ?: 0
                    )
                }
            }
            is ControlMessage.AudioStart -> {
                _remoteDiagnostics?.log(DiagnosticLevel.INFO, "audio",
                    "Audio start: purpose=${message.purpose}, rate=${message.sampleRate}")
                _audioPlayer?.startPurpose(message.purpose, message.sampleRate, message.channels)
            }
            is ControlMessage.AudioStop -> {
                _audioPlayer?.stopPurpose(message.purpose)
            }
            is ControlMessage.MicStart -> {
                DiagnosticLog.i("mic", "MicStart: rate=${message.sampleRate}, source=$micSource")
                if (micSource == "car") {
                    _micCaptureManager?.start(message.sampleRate)
                }
            }
            is ControlMessage.MicStop -> {
                _micCaptureManager?.stop()
            }
            is ControlMessage.Error -> {
                Log.e(TAG, "Error ${message.code}: ${message.message}")
                _statusMessage.value = "Error: ${message.message}"
            }
            is ControlMessage.PhoneBattery -> {
                _phoneBatteryLevel.value = message.level
                _phoneBatteryCritical.value = message.critical
            }
            is ControlMessage.VoiceSession -> {
                _voiceSessionActive.value = message.started
            }
            is ControlMessage.PhoneStatus -> {
                _phoneSignalStrength.value = message.signalStrength
            }
            else -> {}
        }
    }
}
