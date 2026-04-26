package com.openautolink.app.transport.aasdk

/**
 * JNI bridge to the native aasdk C++ library.
 *
 * This is the single entry point for all native calls. The native library
 * manages a single aasdk session that speaks the AA wire protocol directly
 * to the phone via a transport pipe (Nearby Connections stream).
 *
 * Threading: All native methods are thread-safe. The C++ side uses
 * boost::asio::io_service with strand-based serialization.
 */
object AasdkNative {

    init {
        System.loadLibrary("openautolink-jni")
    }

    // -- Session lifecycle --

    /** Create a new native aasdk session. Must be called before start. */
    @JvmStatic
    external fun nativeCreateSession()

    /**
     * Start the AA session over the given transport pipe.
     * @param transportPipe provides readBytes/writeBytes for the Nearby stream
     * @param callback receives AA events (video, audio, nav, etc.)
     * @param sdrConfig service discovery response configuration
     */
    @JvmStatic
    external fun nativeStartSession(
        transportPipe: AasdkTransportPipe,
        callback: AasdkSessionCallback,
        sdrConfig: AasdkSdrConfig
    )

    /** Stop and destroy the native session. */
    @JvmStatic
    external fun nativeStopSession()

    // -- Input (app → phone) --

    @JvmStatic
    external fun nativeSendTouchEvent(
        action: Int, pointerId: Int, x: Float, y: Float, pointerCount: Int
    )

    @JvmStatic
    external fun nativeSendKeyEvent(keyCode: Int, isDown: Boolean)

    @JvmStatic
    external fun nativeSendGpsLocation(
        lat: Double, lon: Double, alt: Double,
        speed: Float, bearing: Float, timestampMs: Long
    )

    @JvmStatic
    external fun nativeSendVehicleSensor(sensorType: Int, data: ByteArray)

    @JvmStatic
    external fun nativeSendMicAudio(data: ByteArray)

    @JvmStatic
    external fun nativeRequestKeyframe()

    @JvmStatic
    external fun nativeIsStreaming(): Boolean
}
