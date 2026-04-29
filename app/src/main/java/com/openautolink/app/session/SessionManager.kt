package com.openautolink.app.session

import android.content.Context
import android.media.AudioManager
import android.os.SystemClock
import com.openautolink.app.diagnostics.OalLog
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
import com.openautolink.app.transport.AudioPurpose
import com.openautolink.app.transport.ConnectionState
import com.openautolink.app.transport.ControlMessage
import com.openautolink.app.transport.aasdk.AasdkSession
import com.openautolink.app.transport.aasdk.AasdkSdrConfig
import com.openautolink.app.transport.direct.AaNearbyManager
import com.openautolink.app.transport.usb.UsbConnectionManager
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
 * Session orchestrator -- connects component islands, manages lifecycle.
 * aasdk JNI mode -- native aasdk C++ handles AA protocol via Nearby transport.
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

    // aasdk JNI session -- native C++ handles AA protocol
    private var aasdkSession: AasdkSession? = null

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

    // Touch coordinate space — matches the SDR input channel touchscreen dimensions.
    // NOT the video codec output dimensions (which may differ due to phone auto-negotiation).
    private val _touchWidth = MutableStateFlow(1920)
    private val _touchHeight = MutableStateFlow(1080)
    val touchWidth: StateFlow<Int> = _touchWidth.asStateFlow()
    val touchHeight: StateFlow<Int> = _touchHeight.asStateFlow()

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

    // Current transport mode — UI uses this to show/hide Nearby-specific features
    private val _transportMode = MutableStateFlow("hotspot")
    val transportMode: StateFlow<String> = _transportMode.asStateFlow()

    // Multi-phone (only active in Nearby mode)
    private val _phoneName = MutableStateFlow<String?>(null)
    val phoneName: StateFlow<String?> = _phoneName.asStateFlow()
    val connectedPhoneName: StateFlow<String?> = AaNearbyManager.connectedPhoneName
    @Volatile private var _defaultPhoneName: String = ""

    /** Set the default phone name from preferences (called at session start). */
    fun setDefaultPhoneName(name: String) { _defaultPhoneName = name }

    /** Get the current default phone name. */
    fun getDefaultPhoneName(): String = _defaultPhoneName

    /** Clear the default phone — next connection will pick any phone. */
    fun clearDefaultPhone() {
        _defaultPhoneName = ""
        scope.launch {
            context?.let { AppPreferences.getInstance(it).setDefaultPhoneName("") }
        }
    }

    /** Switch phone: disconnect, restart discovery in chooser mode. */
    fun switchPhone() {
        stop()
    }

    /** Connect to a specific discovered Nearby endpoint by ID. */
    fun connectToNearbyEndpoint(endpointId: String) {
        aasdkSession?.nearbyManager?.connectToEndpoint(endpointId)
    }

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
        aaPixelAspect: Int = -1,
        aaTargetLayoutWidthDp: Int = 0,
        videoFps: Int = 60,
        driveSide: String = "left",
        hideClock: Boolean = false,
        hideSignal: Boolean = false,
        hideBattery: Boolean = false,
        volumeOffsetMedia: Int = 0,
        volumeOffsetNavigation: Int = 0,
        volumeOffsetAssistant: Int = 0,
        manualIpAddress: String? = null,
        safeAreaTop: Int = 0,
        safeAreaBottom: Int = 0,
        safeAreaLeft: Int = 0,
        safeAreaRight: Int = 0,
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

        // Create mic capture -- sends frames via AasdkSession
        _micCaptureManager?.release()
        _micCaptureManager = MicCaptureManager { frame ->
            aasdkSession?.let { session ->
                scope.launch { session.sendMicAudio(frame.data) }
            }
        }

        // Create GNSS forwarder (NMEA not used in direct mode -- LocationListener used instead)
        _gnssForwarder?.stop()
        _directLocationListener = null
        _gnssForwarder = context?.let { ctx ->
            GnssForwarderImpl(ctx) { _ -> /* NMEA not used in direct mode */ }
        }

        // Create vehicle data forwarder -- sends via AasdkSession
        _vehicleDataForwarder?.stop()
        _vehicleDataForwarder = context?.let { ctx ->
            VehicleDataForwarderImpl(
                ctx,
                sendMessage = { vd ->
                    val session = aasdkSession ?: return@VehicleDataForwarderImpl
                    vd.speedKmh?.let { session.sendSpeed((it / 3.6f * 1000).toInt()) }
                    vd.gearRaw?.let { session.sendGear(it) }
                    vd.parkingBrake?.let { session.sendParkingBrake(it) }
                    vd.nightMode?.let { session.sendNightMode(it) }
                    vd.driving?.let { session.sendDrivingStatus(it) }
                    if (vd.fuelLevelPct != null || vd.rangeKm != null) {
                        session.sendFuel(
                            vd.fuelLevelPct ?: 0,
                            ((vd.rangeKm ?: 0f) * 1000).toInt(),
                            vd.lowFuel ?: false
                        )
                    }
                    vd.rpmE3?.let { session.sendRpm(it) }
                    if (vd.evBatteryLevelWh != null || vd.evBatteryCapacityWh != null) {
                        DiagnosticLog.i("vem", "sendEnergyModel: level=${vd.evBatteryLevelWh?.toInt()}Wh cap=${vd.evBatteryCapacityWh?.toInt()}Wh range=${((vd.rangeKm ?: 0f) * 1000).toInt()}m charge=${vd.evChargeRateW?.toInt()}W")
                        session.sendEnergyModel(
                            vd.evBatteryLevelWh?.toInt() ?: 0,
                            vd.evBatteryCapacityWh?.toInt() ?: 0,
                            ((vd.rangeKm ?: 0f) * 1000).toInt(),
                            vd.evChargeRateW?.toInt() ?: 0
                        )
                    }
                },
                onIgnitionOn = { /* aasdk mode doesn't need ignition-based reconnect */ }
            )
        }

        // Create IMU forwarder -- sends via AasdkSession
        _imuForwarder?.stop()
        _imuForwarder = context?.let { ctx ->
            ImuForwarder(ctx) { imuData ->
                val session = aasdkSession ?: return@ImuForwarder
                imuData.accelXe3?.let { x ->
                    session.sendAccelerometer(x, imuData.accelYe3 ?: 0, imuData.accelZe3 ?: 0)
                }
                imuData.gyroRxe3?.let { rx ->
                    session.sendGyroscope(rx, imuData.gyroRye3 ?: 0, imuData.gyroRze3 ?: 0)
                }
                imuData.compassBearingE6?.let { b ->
                    session.sendCompass(b, imuData.compassPitchE6 ?: 0, imuData.compassRollE6 ?: 0)
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

        // Enable cluster service — always enable so Templates Host can discover it.
        // On non-AAOS devices Templates Host won't exist so the service simply won't bind.
        _clusterManager?.release()
        _clusterManager = context?.let { com.openautolink.app.cluster.ClusterManager(it) }
        _clusterManager?.setClusterEnabled(true)
        // Proactively launch cluster binding — Templates Host on GM doesn't auto-discover
        // the service via intent filter; it requires CarAppActivity to be launched first.
        _clusterManager?.launchClusterBinding()
        OalLog.i(TAG, "Cluster manager initialized and binding launched")

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
                aaWidthMargin, aaHeightMargin, aaPixelAspect, aaTargetLayoutWidthDp, videoFps,
                driveSide, hideClock, hideSignal, hideBattery, scalingMode,
                manualIpAddress,
                safeAreaTop, safeAreaBottom, safeAreaLeft, safeAreaRight)
        }
    }

    private fun startSession(
        directTransport: String, hotspotSsid: String, hotspotPassword: String,
        videoAutoNegotiate: Boolean = true, codec: String = "h264",
        aaResolution: String = "1080p", aaDpi: Int = 160,
        aaWidthMargin: Int = 0, aaHeightMargin: Int = 0, aaPixelAspect: Int = -1,
        aaTargetLayoutWidthDp: Int = 0, videoFps: Int = 60,
        driveSide: String = "left",
        hideClock: Boolean = false, hideSignal: Boolean = false, hideBattery: Boolean = false,
        scalingMode: String = "letterbox",
        manualIpAddress: String? = null,
        safeAreaTop: Int = 0, safeAreaBottom: Int = 0, safeAreaLeft: Int = 0, safeAreaRight: Int = 0,
    ) {
        aasdkSession?.stop()
        _transportMode.value = directTransport
        val ctx = context ?: return

        // Map resolution string to pixel dimensions
        val (resW, resH) = when (aaResolution) {
            "480p" -> 800 to 480
            "720p" -> 1280 to 720
            "1440p" -> 2560 to 1440
            "4k" -> 3840 to 2160
            else -> 1920 to 1080 // "1080p" default
        }

        // Get BT MAC — BluetoothAdapter.getAddress() returns 02:00:00:00:00:00
        // on Android 8+ due to privacy. Try Settings.Secure first, then adapter.
        // GM AAOS returns literal "None" for missing properties.
        var btMac = ""
        try {
            btMac = android.provider.Settings.Secure.getString(
                ctx.contentResolver, "bluetooth_address") ?: ""
        } catch (_: Exception) {}
        if (btMac.isEmpty() || btMac == "02:00:00:00:00:00"
            || btMac.equals("none", ignoreCase = true)) {
            btMac = ""
            try {
                @Suppress("MissingPermission")
                val btAdapter = android.bluetooth.BluetoothAdapter.getDefaultAdapter()
                val addr = btAdapter?.address ?: ""
                if (addr != "02:00:00:00:00:00"
                    && !addr.equals("none", ignoreCase = true)) btMac = addr
            } catch (_: Exception) {}
        }
        OalLog.i(TAG, "BT MAC for SDR: ${if (btMac.isNotEmpty()) btMac else "(none)"}")

        // Vehicle identity from VHAL
        val vd = _vehicleDataForwarder?.latestVehicleData?.value
        val driverPos = if (driveSide == "right") 1 else 0

        // Auto-compute pixel_aspect_ratio for non-16:9 displays (crop mode).
        //
        // pixel_aspect tells the phone "each pixel on the display is X/10000
        // times wider than tall". AA pre-shrinks its UI horizontally so that
        // when the decoder stretches the 16:9 video to fill the wider display,
        // the pre-shrunk circles expand back to round.
        //
        // Formula: pixel_aspect = (displayAR / videoAR) × 10000
        //   e.g. Blazer EV 2914×1134 at 1440p: (2.57 / 1.78) × 10000 = 14454
        //
        // Manual overrides for margins and pixel_aspect are respected if set.
        val computedWidthMargin: Int
        val computedHeightMargin: Int
        val computedPixelAspect: Int
        if (aaWidthMargin > 0 || aaHeightMargin > 0) {
            // Manual margin override
            computedWidthMargin = aaWidthMargin
            computedHeightMargin = aaHeightMargin
            computedPixelAspect = if (aaPixelAspect > 0) aaPixelAspect else 0
        } else if (aaPixelAspect > 0) {
            // Manual pixel_aspect override
            computedWidthMargin = 0
            computedHeightMargin = 0
            computedPixelAspect = aaPixelAspect
            OalLog.i(TAG, "Manual pixel_aspect=$aaPixelAspect")
        } else if (scalingMode == "crop") {
            // Auto-compute pixel_aspect from display geometry (like bridge-mode)
            val wm = ctx.getSystemService(android.content.Context.WINDOW_SERVICE) as android.view.WindowManager
            val bounds = wm.currentWindowMetrics.bounds
            val displayW = bounds.width().toFloat()
            val displayH = bounds.height().toFloat()
            val displayAr = displayW / displayH
            val videoAr = resW.toFloat() / resH.toFloat()
            if (kotlin.math.abs(displayAr - videoAr) / videoAr > 0.02f) {
                // Display AR differs from video AR — compensation needed
                val pa = ((displayAr / videoAr) * 10000).toInt()
                OalLog.i(TAG, "Auto pixel_aspect: display=${displayW.toInt()}x${displayH.toInt()} " +
                        "(${String.format("%.2f", displayAr)}:1) video=${resW}x${resH} " +
                        "(${String.format("%.2f", videoAr)}:1) → pixel_aspect=$pa")
                computedWidthMargin = 0
                computedHeightMargin = 0
                computedPixelAspect = pa
            } else {
                OalLog.i(TAG, "Auto pixel_aspect: display matches video AR, no compensation needed")
                computedWidthMargin = 0
                computedHeightMargin = 0
                computedPixelAspect = 0
            }
        } else {
            // Letterbox mode — no compensation needed
            computedWidthMargin = 0
            computedHeightMargin = 0
            computedPixelAspect = if (aaPixelAspect > 0) aaPixelAspect else 0
        }

        // Per-tier DPI: in manual mode (single tier), compute DPI from target dp width
        // so the user doesn't have to do the math. In auto-negotiate, C++ handles it.
        val computedTargetLayoutWidthDp: Int
        val effectiveDpi: Int
        if (aaTargetLayoutWidthDp > 0 && !videoAutoNegotiate) {
            // Manual mode with target: compute DPI for the selected tier
            effectiveDpi = maxOf((resW * 160) / aaTargetLayoutWidthDp, 80)
            computedTargetLayoutWidthDp = 0 // C++ doesn't need it — single tier
            OalLog.i(TAG, "Per-tier DPI (manual): ${resW}px / ${aaTargetLayoutWidthDp}dp → DPI $effectiveDpi")
        } else {
            effectiveDpi = aaDpi
            computedTargetLayoutWidthDp = aaTargetLayoutWidthDp
        }

        OalLog.i(TAG, "SDR AR config: scalingMode=$scalingMode marginW=$computedWidthMargin marginH=$computedHeightMargin pixelAspectE4=$computedPixelAspect")

        val session = AasdkSession(scope, ctx)
        session.transportMode = directTransport
        session.manualIpAddress = manualIpAddress
        session.sdrConfig = AasdkSdrConfig(
            videoWidth = resW,
            videoHeight = resH,
            videoFps = videoFps,
            videoDpi = effectiveDpi,
            marginWidth = computedWidthMargin,
            marginHeight = computedHeightMargin,
            pixelAspectE4 = computedPixelAspect,
            btMacAddress = btMac,
            vehicleMake = vd?.carMake ?: "OpenAutoLink",
            vehicleModel = vd?.carModel ?: "Direct",
            vehicleYear = vd?.carYear ?: "2024",
            driverPosition = driverPos,
            hideClock = hideClock,
            hideSignal = hideSignal,
            hideBattery = hideBattery,
            autoNegotiate = videoAutoNegotiate,
            videoCodec = codec,
            // realDensity removed — interferes with pixel_aspect_ratio_e4 on some AA versions
            safeAreaTop = safeAreaTop,
            safeAreaBottom = safeAreaBottom,
            safeAreaLeft = safeAreaLeft,
            safeAreaRight = safeAreaRight,
            targetLayoutWidthDp = computedTargetLayoutWidthDp,
        )
        _touchWidth.value = resW
        _touchHeight.value = resH

        // Multi-phone: only relevant in nearby mode
        if (directTransport == "nearby") {
            session.defaultPhoneName = _defaultPhoneName
            session.onPhoneConnected = { phoneName ->
                _phoneName.value = phoneName
                if (_defaultPhoneName.isEmpty()) {
                    _defaultPhoneName = phoneName
                    scope.launch {
                        val c = context ?: return@launch
                        AppPreferences.getInstance(c).setDefaultPhoneName(phoneName)
                        OalLog.i(TAG, "Default phone saved: $phoneName")
                    }
                }
            }
        }

        aasdkSession = session

        // Observe session state
        scope.launch {
            session.connectionState.collect { connState ->
                val newState = connState.toSessionState()
                _sessionState.value = newState
                _statusMessage.value = when (newState) {
                    SessionState.IDLE -> when (directTransport) {
                        "nearby" -> "Nearby: ${AaNearbyManager.status.value}"
                        "usb" -> "USB: ${UsbConnectionManager.status.value}"
                        else -> "TCP: connecting to phone hotspot..."
                    }
                    SessionState.CONNECTING -> "Phone connecting..."
                    SessionState.CONNECTED -> "Handshake..."
                    SessionState.STREAMING -> "Streaming"
                    SessionState.ERROR -> "Error"
                }
                if (newState == SessionState.STREAMING) {
                    startLocationForwarding(session)
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

        // Update decoder when phone negotiates codec type
        scope.launch {
            session.negotiatedCodecType.collect { codecType ->
                if (codecType > 0) {
                    (_videoDecoder as? com.openautolink.app.video.MediaCodecDecoder)
                        ?.setNegotiatedCodec(codecType)
                }
            }
        }

        // Collect audio frames
        scope.launch(audioDispatcher) {
            session.audioFrames.collect { frame ->
                _audioPlayer?.onAudioFrame(frame)
            }
        }

        // Observe Nearby status (only in nearby mode)
        if (directTransport == "nearby") {
            scope.launch {
                AaNearbyManager.status.collect { nearbyStatus ->
                    if (_sessionState.value == SessionState.IDLE) {
                        _statusMessage.value = "Nearby: $nearbyStatus"
                    }
                }
            }
        }

        // Observe USB status (only in usb mode)
        if (directTransport == "usb") {
            scope.launch {
                UsbConnectionManager.status.collect { usbStatus ->
                    if (_sessionState.value == SessionState.IDLE) {
                        _statusMessage.value = "USB: $usbStatus"
                    }
                }
            }
        }

        session.start()
        OalLog.i(TAG, "aasdk JNI session started ($directTransport transport)")
    }

    @android.annotation.SuppressLint("MissingPermission")
    private fun startLocationForwarding(session: AasdkSession) {
        stopDirectLocationForwarding()
        val ctx = context ?: return
        val lm = ctx.getSystemService(Context.LOCATION_SERVICE) as? android.location.LocationManager ?: return
        if (!lm.isProviderEnabled(android.location.LocationManager.GPS_PROVIDER)) {
            OalLog.w(TAG, "GPS provider not enabled")
            return
        }

        val listener = android.location.LocationListener { location ->
            session.sendGpsLocation(
                location.latitude, location.longitude, location.altitude,
                location.speed, location.bearing, location.time
            )
        }
        try {
            lm.requestLocationUpdates(
                android.location.LocationManager.GPS_PROVIDER,
                500L, 0f, listener, android.os.Looper.getMainLooper(),
            )
            _directLocationListener = listener
            OalLog.i(TAG, "GPS forwarding started")
        } catch (e: SecurityException) {
            OalLog.w(TAG, "GPS permission denied: ${e.message}")
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
        aasdkSession?.stop()
        aasdkSession = null
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
     * Reconnect the AA session with new SDR/settings without tearing down
     * component islands (audio, mic, GNSS, VHAL, IMU, cluster, diagnostics).
     *
     * Only the AA protocol session is restarted — the phone will reconnect
     * and renegotiate SDR with the new parameters. File logging, TCP log
     * streaming, telemetry, and all sensor forwarders stay alive.
     *
     * Falls back to full [start] if component islands haven't been initialized yet.
     */
    fun reconnect(
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
        aaPixelAspect: Int = -1,
        aaTargetLayoutWidthDp: Int = 0,
        videoFps: Int = 60,
        driveSide: String = "left",
        hideClock: Boolean = false,
        hideSignal: Boolean = false,
        hideBattery: Boolean = false,
        volumeOffsetMedia: Int = 0,
        volumeOffsetNavigation: Int = 0,
        volumeOffsetAssistant: Int = 0,
        manualIpAddress: String? = null,
        safeAreaTop: Int = 0,
        safeAreaBottom: Int = 0,
        safeAreaLeft: Int = 0,
        safeAreaRight: Int = 0,
    ) {
        // If islands were never initialized, do a full start
        if (_audioPlayer == null) {
            start(
                codecPreference, micSourcePreference, scalingMode, directTransport,
                hotspotSsid, hotspotPassword, videoAutoNegotiate, aaResolution,
                aaDpi, aaWidthMargin, aaHeightMargin, aaPixelAspect, aaTargetLayoutWidthDp, videoFps,
                driveSide, hideClock, hideSignal, hideBattery,
                volumeOffsetMedia, volumeOffsetNavigation, volumeOffsetAssistant,
                manualIpAddress, safeAreaTop, safeAreaBottom, safeAreaLeft, safeAreaRight,
            )
            return
        }

        OalLog.i(TAG, "Reconnecting AA session with new settings (minimal restart)")
        micSource = micSourcePreference

        // 1. Cancel old session observer coroutines
        observeJob?.cancel()
        observeJob = null
        decoderWatchJob?.cancel()
        decoderWatchJob = null
        keyframeWatchJob?.cancel()
        keyframeWatchJob = null
        callStateJob?.cancel()
        callStateJob = null

        // 2. Stop old AA session + location forwarding
        aasdkSession?.stop()
        aasdkSession = null
        stopDirectLocationForwarding()

        // 3. Flush video decoder (codec/scaling may have changed)
        _videoDecoder?.release()
        _videoDecoder = MediaCodecDecoder(codecPreference, scalingMode)
        _telemetryCollector?.videoDecoder = _videoDecoder

        // 4. Update audio volume offsets in-place (no release/recreate)
        (_audioPlayer as? AudioPlayerImpl)?.coordinator?.let { coord ->
            coord.volumeOffsetMedia = volumeOffsetMedia
            coord.volumeOffsetNavigation = volumeOffsetNavigation
            coord.volumeOffsetAssistant = volumeOffsetAssistant
        }

        // 5. Pause sensor forwarders — they'll restart when new session reaches STREAMING
        _vehicleDataForwarder?.stop()
        _imuForwarder?.stop()

        // 6. Clear stale navigation state
        _navigationDisplay.clear()
        ClusterNavigationState.clear()

        // 7. Update status — don't go to IDLE, just show reconnecting
        _statusMessage.value = "Reconnecting..."
        _phoneBatteryLevel.value = null
        _phoneBatteryCritical.value = false
        _voiceSessionActive.value = false
        _phoneSignalStrength.value = null

        // 8. Start new AA session with new SDR config
        observeJob = scope.launch {
            decoderWatchJob = launch { watchDecoderState() }
            keyframeWatchJob = launch { watchKeyframeNeeds() }
            callStateJob = launch { watchCallState() }

            startSession(directTransport, hotspotSsid, hotspotPassword,
                videoAutoNegotiate, codecPreference, aaResolution, aaDpi,
                aaWidthMargin, aaHeightMargin, aaPixelAspect, aaTargetLayoutWidthDp, videoFps,
                driveSide, hideClock, hideSignal, hideBattery, scalingMode,
                manualIpAddress,
                safeAreaTop, safeAreaBottom, safeAreaLeft, safeAreaRight)
        }
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

        OalLog.i(TAG, "System wake detected (${elapsed / 1000}s gap, state=$state)")
        DiagnosticLog.i("transport", "System wake detected (${elapsed / 1000}s gap)")
        // aasdk mode: the Nearby manager handles reconnection
        // No explicit force-reconnect needed
    }

    suspend fun requestKeyframe() {
        aasdkSession?.requestKeyframe()
    }

    private suspend fun syncLocalPreferences() {
        val ctx = context ?: return
        try {
            val prefs = AppPreferences.getInstance(ctx)
            ClusterNavigationState.distanceUnits = prefs.distanceUnits.first()
        } catch (e: Exception) {
            OalLog.w(TAG, "Failed to sync local preferences: ${e.message}")
        }
    }

    suspend fun sendControlMessage(message: ControlMessage) {
        val session = aasdkSession ?: return
        when (message) {
            is ControlMessage.Touch -> {
                if (message.pointers != null && message.pointers.isNotEmpty()) {
                    // Multi-touch: send all pointers via native multi-touch API
                    val ids = message.pointers.map { it.id }.toIntArray()
                    val xs = message.pointers.map { it.x }.toFloatArray()
                    val ys = message.pointers.map { it.y }.toFloatArray()
                    session.sendMultiTouchEvent(
                        message.action, message.actionIndex ?: 0,
                        ids, xs, ys
                    )
                } else {
                    val x = message.x ?: return
                    val y = message.y ?: return
                    session.sendTouchEvent(
                        message.action, message.pointerId ?: 0, x, y, 1
                    )
                }
            }
            is ControlMessage.Button -> session.sendKeyEvent(message.keycode, message.down)
            is ControlMessage.KeyframeRequest -> session.requestKeyframe()
            is ControlMessage.VehicleData -> {
                message.speedKmh?.let { session.sendSpeed((it / 3.6f * 1000).toInt()) }
                message.gearRaw?.let { session.sendGear(it) }
                message.parkingBrake?.let { session.sendParkingBrake(it) }
                message.nightMode?.let { session.sendNightMode(it) }
                message.driving?.let { session.sendDrivingStatus(it) }
            }
            is ControlMessage.Gnss -> {
                // GPS forwarded via LocationListener, not control messages
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
                OalLog.w(TAG, "Decoder error -- initiating recovery")
                _remoteDiagnostics?.log(DiagnosticLevel.ERROR, "video", "Decoder error -- recovery")
                _statusMessage.value = "Video error -- recovering..."
                recoverDecoder()
            }
        }
    }

    private suspend fun recoverDecoder() {
        delay(500)
        _videoDecoder?.let { decoder ->
            decoder.resume()
            requestKeyframe()
            OalLog.i(TAG, "Decoder recovery: resumed codec, requested keyframe")
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
                        OalLog.i(TAG, "Keyframe re-request #$attempt")
                    } else {
                        OalLog.w(TAG, "Keyframe re-request #$attempt (still waiting)")
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
            OalLog.d(TAG, "Call state: $state -- mic purpose: $purpose")
        }
    }

    private fun handleControlMessage(message: ControlMessage) {
        when (message) {
            is ControlMessage.PhoneConnected -> {
                _remoteDiagnostics?.log(DiagnosticLevel.INFO, "session", "Phone connected: ${message.phoneName}")
                _sessionState.value = SessionState.STREAMING
                _statusMessage.value = "Streaming"
                aasdkSession?.let { startLocationForwarding(it) }
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
            is ControlMessage.MediaPlaybackState -> {
                _mediaSessionManager?.updatePlaybackState(
                    playing = message.playing, positionMs = message.positionMs
                )
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
                OalLog.e(TAG, "Error ${message.code}: ${message.message}")
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

