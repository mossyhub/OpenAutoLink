package com.openautolink.app.transport.aasdk

import android.util.Log
import com.openautolink.app.audio.AudioFrame
import com.openautolink.app.transport.AudioPurpose
import com.openautolink.app.transport.ConnectionState
import com.openautolink.app.transport.ControlMessage
import com.openautolink.app.video.VideoFrame
import kotlinx.coroutines.CoroutineScope
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

/**
 * Kotlin-side AA session backed by native aasdk via JNI.
 *
 * Replaces [com.openautolink.app.transport.direct.DirectAaSession] with a
 * native aasdk implementation. Uses the same output flows (videoFrames,
 * audioFrames, connectionState) so SessionManager wiring is unchanged.
 *
 * Data flow:
 *   Nearby streams → AasdkTransportPipe → JNI → aasdk C++ → JNI callbacks
 *     → AasdkSession → SharedFlow → SessionManager → VideoDecoder/AudioPlayer
 */
class AasdkSession(
    private val scope: CoroutineScope
) : AasdkSessionCallback {

    companion object {
        private const val TAG = "AasdkSession"
    }

    // -- Output flows (consumed by SessionManager) --

    private val _connectionState = MutableStateFlow(ConnectionState.DISCONNECTED)
    val connectionState: StateFlow<ConnectionState> = _connectionState.asStateFlow()

    private val _videoFrames = MutableSharedFlow<VideoFrame>(extraBufferCapacity = 30)
    val videoFrames: SharedFlow<VideoFrame> = _videoFrames.asSharedFlow()

    private val _audioFrames = MutableSharedFlow<AudioFrame>(extraBufferCapacity = 60)
    val audioFrames: SharedFlow<AudioFrame> = _audioFrames.asSharedFlow()

    private val _controlMessages = MutableSharedFlow<ControlMessage>(extraBufferCapacity = 64)
    val controlMessages: Flow<ControlMessage> = _controlMessages.asSharedFlow()

    // Callbacks for SessionManager to wire
    var onMicOpenRequested: ((Boolean) -> Unit)? = null
    var onNavigationStatusUpdate: ((ByteArray) -> Unit)? = null
    var onNavigationTurnUpdate: ((ByteArray) -> Unit)? = null
    var onNavigationDistanceUpdate: ((ByteArray) -> Unit)? = null
    var onMediaMetadataUpdate: ((String, String, String, ByteArray?) -> Unit)? = null
    var onMediaPlaybackUpdate: ((Int, Long) -> Unit)? = null
    var onPhoneStatusUpdate: ((Int, Int) -> Unit)? = null
    var onPhoneBatteryUpdate: ((Int, Boolean) -> Unit)? = null
    var onVoiceSessionUpdate: ((Boolean) -> Unit)? = null
    var onAudioFocusUpdate: ((Int) -> Unit)? = null
    var onErrorCallback: ((String) -> Unit)? = null

    private var transportPipe: AasdkTransportPipe? = null

    /**
     * Start the AA session over Nearby streams.
     */
    fun start(
        inputStream: InputStream,
        outputStream: OutputStream,
        config: AasdkSdrConfig
    ) {
        Log.i(TAG, "Starting aasdk session: ${config.videoWidth}x${config.videoHeight}@${config.videoFps}")

        _connectionState.value = ConnectionState.CONNECTING

        // Create transport pipe from Nearby streams
        transportPipe = AasdkTransportPipe(inputStream, outputStream)

        // Create and start native session
        AasdkNative.nativeCreateSession()
        AasdkNative.nativeStartSession(transportPipe!!, this, config)
    }

    fun stop() {
        Log.i(TAG, "Stopping aasdk session")
        AasdkNative.nativeStopSession()
        transportPipe?.close()
        transportPipe = null
        _connectionState.value = ConnectionState.DISCONNECTED
    }

    // -- Input forwarding (app → phone via native) --

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

    fun sendMicAudio(data: ByteArray) {
        AasdkNative.nativeSendMicAudio(data)
    }

    fun requestKeyframe() {
        AasdkNative.nativeRequestKeyframe()
    }

    // -- AasdkSessionCallback implementation (called from native thread) --

    override fun onSessionStarted() {
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
