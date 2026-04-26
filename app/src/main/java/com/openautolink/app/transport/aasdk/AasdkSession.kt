package com.openautolink.app.transport.aasdk

import android.content.Context
import android.util.Log
import com.openautolink.app.audio.AudioFrame
import com.openautolink.app.diagnostics.OalLog
import com.openautolink.app.transport.AudioPurpose
import com.openautolink.app.transport.ConnectionState
import com.openautolink.app.transport.ControlMessage
import com.openautolink.app.transport.direct.AaNearbyManager
import com.openautolink.app.video.VideoFrame
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import java.io.InputStream
import java.io.OutputStream
import java.net.Socket

/**
 * AA session backed by native aasdk via JNI.
 *
 * Replaces DirectAaSession — same Nearby transport, but the AA wire protocol
 * is handled by the proven aasdk C++ library instead of the Kotlin port.
 *
 * Data flow:
 *   Phone ←→ Nearby (companion app) ←→ AaNearbyManager ←→ streams
 *     → AasdkTransportPipe → JNI → aasdk C++ → JNI callbacks
 *     → AasdkSession flows → SessionManager → VideoDecoder/AudioPlayer
 */
class AasdkSession(
    private val scope: CoroutineScope,
    private val context: Context,
) : AasdkSessionCallback {

    companion object {
        private const val TAG = "AasdkSession"
    }

    // -- Output flows (same interface as DirectAaSession) --

    private val _connectionState = MutableStateFlow(ConnectionState.DISCONNECTED)
    val connectionState: StateFlow<ConnectionState> = _connectionState.asStateFlow()

    private val _videoFrames = MutableSharedFlow<VideoFrame>(extraBufferCapacity = 30)
    val videoFrames: SharedFlow<VideoFrame> = _videoFrames.asSharedFlow()

    private val _audioFrames = MutableSharedFlow<AudioFrame>(extraBufferCapacity = 60)
    val audioFrames: SharedFlow<AudioFrame> = _audioFrames.asSharedFlow()

    private val _controlMessages = MutableSharedFlow<ControlMessage>(extraBufferCapacity = 64)
    val controlMessages: Flow<ControlMessage> = _controlMessages.asSharedFlow()

    // -- Config (set before start(), same as DirectAaSession) --

    var sdrConfig = AasdkSdrConfig()

    /** Default phone name for Nearby auto-connect */
    var defaultPhoneName: String = ""

    /** Callback when a phone connects via Nearby */
    var onPhoneConnected: ((String) -> Unit)? = null

    // Nearby manager — reuse the existing one from direct mode
    private var _nearbyManager: AaNearbyManager? = null
    val nearbyManager: AaNearbyManager? get() = _nearbyManager

    private var transportPipe: AasdkTransportPipe? = null

    // -- Lifecycle --

    fun start() {
        _connectionState.value = ConnectionState.DISCONNECTED
        OalLog.i(TAG, "Starting aasdk session (Nearby transport)")

        // Start Nearby discovery — when phone connects, we get streams
        _nearbyManager?.stop()
        _nearbyManager = AaNearbyManager(context, scope) { nearbySocket ->
            scope.launch(Dispatchers.IO) {
                OalLog.i(TAG, "Nearby socket ready — starting aasdk native session")
                handleConnection(nearbySocket)
            }
        }
        _nearbyManager?.defaultPhoneName = defaultPhoneName
        _nearbyManager?.onPhoneConnected = onPhoneConnected
        _nearbyManager?.start()
    }

    private fun handleConnection(socket: Socket) {
        _connectionState.value = ConnectionState.CONNECTING

        val input = socket.getInputStream()
        val output = socket.getOutputStream()

        OalLog.i(TAG, "Starting native aasdk session: ${sdrConfig.videoWidth}x${sdrConfig.videoHeight}")

        transportPipe = AasdkTransportPipe(input, output)

        AasdkNative.nativeCreateSession()
        AasdkNative.nativeStartSession(transportPipe!!, this, sdrConfig)
    }

    fun stop() {
        OalLog.i(TAG, "Stopping aasdk session")
        _nearbyManager?.stop()
        _nearbyManager = null
        AasdkNative.nativeStopSession()
        transportPipe?.close()
        transportPipe = null
        _connectionState.value = ConnectionState.DISCONNECTED
    }

    // -- Input forwarding (app → phone via native aasdk) --

    fun sendTouchEvent(action: Int, pointerId: Int, x: Float, y: Float, pointerCount: Int) {
        AasdkNative.nativeSendTouchEvent(action, pointerId, x, y, pointerCount)
    }

    fun sendKeyEvent(keyCode: Int, isDown: Boolean) {
        AasdkNative.nativeSendKeyEvent(keyCode, isDown)
    }

    fun sendGpsLocation(lat: Double, lon: Double, alt: Double,
                        speed: Float, bearing: Float, timestampMs: Long) {
        AasdkNative.nativeSendGpsLocation(lat, lon, alt, speed, bearing, timestampMs)
    }

    fun sendVehicleSensor(sensorType: Int, data: ByteArray) {
        AasdkNative.nativeSendVehicleSensor(sensorType, data)
    }

    fun sendSpeed(speedMmPerS: Int) = AasdkNative.nativeSendSpeed(speedMmPerS)
    fun sendGear(gear: Int) = AasdkNative.nativeSendGear(gear)
    fun sendParkingBrake(engaged: Boolean) = AasdkNative.nativeSendParkingBrake(engaged)
    fun sendNightMode(night: Boolean) = AasdkNative.nativeSendNightMode(night)
    fun sendDrivingStatus(moving: Boolean) = AasdkNative.nativeSendDrivingStatus(moving)
    fun sendFuel(levelPct: Int, rangeM: Int, lowFuel: Boolean) = AasdkNative.nativeSendFuel(levelPct, rangeM, lowFuel)
    fun sendAccelerometer(xE3: Int, yE3: Int, zE3: Int) = AasdkNative.nativeSendAccelerometer(xE3, yE3, zE3)
    fun sendGyroscope(rxE3: Int, ryE3: Int, rzE3: Int) = AasdkNative.nativeSendGyroscope(rxE3, ryE3, rzE3)
    fun sendCompass(bearingE6: Int, pitchE6: Int, rollE6: Int) = AasdkNative.nativeSendCompass(bearingE6, pitchE6, rollE6)
    fun sendRpm(rpmE3: Int) = AasdkNative.nativeSendRpm(rpmE3)

    fun sendMicAudio(data: ByteArray) {
        AasdkNative.nativeSendMicAudio(data)
    }

    fun requestKeyframe() {
        AasdkNative.nativeRequestKeyframe()
    }

    // -- AasdkSessionCallback (called from native thread → dispatch to flows) --

    override fun onSessionStarted() {
        OalLog.i(TAG, "AA session started (native)")
        scope.launch {
            _connectionState.value = ConnectionState.CONNECTED
            _controlMessages.emit(ControlMessage.PhoneConnected(phoneName = ""))
        }
    }

    override fun onSessionStopped(reason: String) {
        OalLog.i(TAG, "AA session stopped: $reason")
        scope.launch {
            _connectionState.value = ConnectionState.DISCONNECTED
            _controlMessages.emit(ControlMessage.PhoneDisconnected(reason = reason))
        }
    }

    override fun onVideoFrame(data: ByteArray, timestampUs: Long, width: Int, height: Int) {
        val frame = VideoFrame(
            data = data,
            timestampUs = timestampUs,
            width = width,
            height = height,
            isKeyFrame = false
        )
        _videoFrames.tryEmit(frame)
    }

    override fun onAudioFrame(data: ByteArray, purpose: Int, sampleRate: Int, channels: Int) {
        val audioPurpose = when (purpose) {
            0 -> AudioPurpose.MEDIA
            1 -> AudioPurpose.NAVIGATION
            2 -> AudioPurpose.ALERT
            3 -> AudioPurpose.ASSISTANT
            4 -> AudioPurpose.CALL
            else -> AudioPurpose.MEDIA
        }
        val frame = AudioFrame(
            data = data,
            purpose = audioPurpose,
            sampleRate = sampleRate,
            channels = channels
        )
        _audioFrames.tryEmit(frame)
    }

    override fun onMicRequest(open: Boolean) {
        scope.launch {
            if (open) _controlMessages.emit(ControlMessage.MicStart(sampleRate = 16000))
            else _controlMessages.emit(ControlMessage.MicStop)
        }
    }

    override fun onNavigationStatus(status: Int) {
        scope.launch {
            if (status != 1) { // not ACTIVE
                _controlMessages.emit(ControlMessage.NavStateClear)
            }
        }
    }

    override fun onNavigationTurn(maneuver: String, road: String, iconPng: ByteArray?) {
        scope.launch {
            val iconBase64 = iconPng?.let { android.util.Base64.encodeToString(it, android.util.Base64.NO_WRAP) }
            _controlMessages.emit(ControlMessage.NavState(
                maneuver = maneuver,
                distanceMeters = null,
                road = road,
                etaSeconds = null,
                navImageBase64 = iconBase64
            ))
        }
    }

    override fun onNavigationDistance(distanceMeters: Int, etaSeconds: Int,
                                      displayDistance: String?, displayUnit: String?) {
        scope.launch {
            _controlMessages.emit(ControlMessage.NavState(
                maneuver = null,
                distanceMeters = distanceMeters,
                road = null,
                etaSeconds = etaSeconds,
                displayDistance = displayDistance,
                displayDistanceUnit = displayUnit
            ))
        }
    }

    override fun onMediaMetadata(title: String, artist: String, album: String, albumArt: ByteArray?) {
        scope.launch {
            _controlMessages.emit(ControlMessage.MediaMetadata(
                title = title, artist = artist, album = album,
                albumArtBase64 = null, playing = null, positionMs = null, durationMs = null
            ))
        }
    }

    override fun onMediaPlayback(state: Int, positionMs: Long) {
        scope.launch {
            val playing = state == 1 // PLAYING
            _controlMessages.emit(ControlMessage.MediaMetadata(
                title = null, artist = null, album = null,
                durationMs = null, positionMs = positionMs,
                playing = playing, albumArtBase64 = null
            ))
        }
    }

    override fun onPhoneStatus(signalStrength: Int, callState: Int) {
        scope.launch {
            _controlMessages.emit(ControlMessage.PhoneStatus(signalStrength = signalStrength))
        }
    }

    override fun onPhoneBattery(level: Int, charging: Boolean) {
        scope.launch {
            _controlMessages.emit(ControlMessage.PhoneBattery(level = level, critical = level < 10))
        }
    }

    override fun onVoiceSession(active: Boolean) {
        scope.launch {
            _controlMessages.emit(ControlMessage.VoiceSession(started = active))
        }
    }

    override fun onAudioFocusRequest(focusType: Int) {
        // Audio focus is handled by the native layer — always grants
    }

    override fun onError(message: String) {
        OalLog.e(TAG, "Native error: $message")
        scope.launch {
            _controlMessages.emit(ControlMessage.Error(code = -1, message = message))
        }
    }
}
        Log.i(TAG, "AA session started")
        scope.launch { _connectionState.value = ConnectionState.CONNECTED }
    }

    override fun onSessionStopped(reason: String) {
        Log.i(TAG, "AA session stopped: $reason")
        scope.launch { _connectionState.value = ConnectionState.DISCONNECTED }
    }

    override fun onVideoFrame(data: ByteArray, timestampUs: Long, width: Int, height: Int) {
        val frame = VideoFrame(
            data = data,
            timestampUs = timestampUs,
            width = width,
            height = height,
            isKeyFrame = false // Native side should set FLAG_KEYFRAME; detect from NAL
        )
        _videoFrames.tryEmit(frame)
    }

    override fun onAudioFrame(data: ByteArray, purpose: Int, sampleRate: Int, channels: Int) {
        val audioPurpose = when (purpose) {
            0 -> AudioPurpose.MEDIA
            1 -> AudioPurpose.NAVIGATION
            2 -> AudioPurpose.ALERT
            3 -> AudioPurpose.ASSISTANT
            4 -> AudioPurpose.CALL
            else -> AudioPurpose.MEDIA
        }
        val frame = AudioFrame(
            data = data,
            purpose = audioPurpose,
            sampleRate = sampleRate,
            channels = channels
        )
        _audioFrames.tryEmit(frame)
    }

    override fun onMicRequest(open: Boolean) {
        onMicOpenRequested?.invoke(open)
    }

    override fun onNavigationStatus(protoData: ByteArray) {
        onNavigationStatusUpdate?.invoke(protoData)
    }

    override fun onNavigationTurn(protoData: ByteArray) {
        onNavigationTurnUpdate?.invoke(protoData)
    }

    override fun onNavigationDistance(protoData: ByteArray) {
        onNavigationDistanceUpdate?.invoke(protoData)
    }

    override fun onMediaMetadata(title: String, artist: String, album: String, albumArt: ByteArray?) {
        onMediaMetadataUpdate?.invoke(title, artist, album, albumArt)
    }

    override fun onMediaPlayback(state: Int, positionMs: Long) {
        onMediaPlaybackUpdate?.invoke(state, positionMs)
    }

    override fun onPhoneStatus(signalStrength: Int, callState: Int) {
        onPhoneStatusUpdate?.invoke(signalStrength, callState)
    }

    override fun onPhoneBattery(level: Int, charging: Boolean) {
        onPhoneBatteryUpdate?.invoke(level, charging)
    }

    override fun onVoiceSession(active: Boolean) {
        onVoiceSessionUpdate?.invoke(active)
    }

    override fun onAudioFocusRequest(focusType: Int) {
        onAudioFocusUpdate?.invoke(focusType)
    }

    override fun onError(message: String) {
        Log.e(TAG, "Native error: $message")
        onErrorCallback?.invoke(message)
    }
}
