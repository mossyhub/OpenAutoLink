package com.openautolink.app.transport.aasdk

/**
 * Configuration for the AA Service Discovery Response (SDR).
 *
 * Passed to native code which builds the protobuf SDR that tells
 * the phone what this head unit supports.
 *
 * Fields are accessed from C++ via JNI GetIntField/GetObjectField.
 */
class AasdkSdrConfig(
    /** Video width in pixels (e.g. 1920). */
    @JvmField val videoWidth: Int = 1920,

    /** Video height in pixels (e.g. 1080). */
    @JvmField val videoHeight: Int = 1080,

    /** Video frames per second (e.g. 60). */
    @JvmField val videoFps: Int = 60,

    /** Video DPI for UI scaling (e.g. 160). */
    @JvmField val videoDpi: Int = 160,

    /** Stable inset width margin in pixels. */
    @JvmField val marginWidth: Int = 0,

    /** Stable inset height margin in pixels. */
    @JvmField val marginHeight: Int = 0,

    /** Pixel aspect ratio × 10000 (0 = square pixels). */
    @JvmField val pixelAspectE4: Int = 0,

    /** Bluetooth MAC address of the car (for BT pairing service). */
    @JvmField val btMacAddress: String = "",

    /** Vehicle make (e.g. "Chevrolet"). */
    @JvmField val vehicleMake: String = "OpenAutoLink",

    /** Vehicle model (e.g. "Blazer EV"). */
    @JvmField val vehicleModel: String = "Direct",

    /** Vehicle year (e.g. "2024"). */
    @JvmField val vehicleYear: String = "2024",

    /** Driver position: 0=left, 1=right. */
    @JvmField val driverPosition: Int = 0,

    /** Hide clock in AA status bar. */
    @JvmField val hideClock: Boolean = false,

    /** Hide signal strength in AA status bar. */
    @JvmField val hideSignal: Boolean = false,

    /** Hide battery indicator in AA status bar. */
    @JvmField val hideBattery: Boolean = false,
)
