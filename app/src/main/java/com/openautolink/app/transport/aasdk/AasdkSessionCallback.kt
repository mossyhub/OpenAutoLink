package com.openautolink.app.transport.aasdk

/**
 * Callback interface for AA session events from native aasdk.
 *
 * Implemented by [AasdkSession] and dispatched to the appropriate
 * component island (video, audio, navigation, etc.).
 *
 * All methods are called from the native io_service thread.
 * Implementations must be lightweight — post to coroutines for heavy work.
 */
interface AasdkSessionCallback {

    /** AA session fully established (handshake + SDR complete, channels opening). */
    fun onSessionStarted()

    /** AA session ended. [reason] describes why (user disconnect, error, etc.). */
    fun onSessionStopped(reason: String)

    /**
     * Video frame received from phone.
     * @param data Raw codec data (H.264/H.265/VP9 NAL units)
     * @param timestampUs Presentation timestamp in microseconds
     * @param width Video width (from setup)
     * @param height Video height (from setup)
     */
    fun onVideoFrame(data: ByteArray, timestampUs: Long, width: Int, height: Int)

    /**
     * Audio frame received from phone.
     * @param data Raw PCM audio data
     * @param purpose Audio purpose: 0=media, 1=speech/nav, 2=system/alert
     * @param sampleRate Sample rate in Hz
     * @param channels Number of audio channels
     */
    fun onAudioFrame(data: ByteArray, purpose: Int, sampleRate: Int, channels: Int)

    /**
     * Phone requests mic open/close.
     * @param open true = start capturing mic, false = stop
     */
    fun onMicRequest(open: Boolean)

    /** Navigation status update (serialized protobuf). */
    fun onNavigationStatus(protoData: ByteArray)

    /** Navigation next turn event (serialized protobuf). */
    fun onNavigationTurn(protoData: ByteArray)

    /** Navigation distance to next turn (serialized protobuf). */
    fun onNavigationDistance(protoData: ByteArray)

    /** Media metadata update (track info). */
    fun onMediaMetadata(title: String, artist: String, album: String, albumArt: ByteArray?)

    /**
     * Media playback state update.
     * @param state 0=stopped, 1=playing, 2=paused
     * @param positionMs Current playback position in milliseconds
     */
    fun onMediaPlayback(state: Int, positionMs: Long)

    /**
     * Phone status update.
     * @param signalStrength Signal strength (0-5)
     * @param callState 0=idle, 1=ringing, 2=active
     */
    fun onPhoneStatus(signalStrength: Int, callState: Int)

    /**
     * Phone battery update.
     * @param level Battery percentage (0-100)
     * @param charging Whether the phone is charging
     */
    fun onPhoneBattery(level: Int, charging: Boolean)

    /** Voice session state change (Google Assistant active/inactive). */
    fun onVoiceSession(active: Boolean)

    /**
     * Audio focus request from phone.
     * @param focusType 1=GAIN, 2=GAIN_TRANSIENT, 3=RELEASE
     */
    fun onAudioFocusRequest(focusType: Int)

    /** Error from the native session. */
    fun onError(message: String)
}
