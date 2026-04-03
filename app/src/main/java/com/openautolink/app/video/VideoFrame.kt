package com.openautolink.app.video

/**
 * A single video frame received from the bridge via OAL protocol.
 * 16-byte header: payload_length(u32) + width(u16) + height(u16) + pts_ms(u32) + flags(u16) + reserved(u16)
 */
data class VideoFrame(
    val width: Int,
    val height: Int,
    val ptsMs: Long,
    val flags: Int,
    val data: ByteArray
) {
    val isKeyframe: Boolean get() = flags and FLAG_KEYFRAME != 0
    val isCodecConfig: Boolean get() = flags and FLAG_CODEC_CONFIG != 0
    val isEndOfStream: Boolean get() = flags and FLAG_END_OF_STREAM != 0

    companion object {
        const val HEADER_SIZE = 16
        const val FLAG_KEYFRAME = 0x0001
        const val FLAG_CODEC_CONFIG = 0x0002
        const val FLAG_END_OF_STREAM = 0x0004
    }

    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (other !is VideoFrame) return false
        return width == other.width && height == other.height &&
                ptsMs == other.ptsMs && flags == other.flags &&
                data.contentEquals(other.data)
    }

    override fun hashCode(): Int {
        var result = width
        result = 31 * result + height
        result = 31 * result + ptsMs.hashCode()
        result = 31 * result + flags
        result = 31 * result + data.contentHashCode()
        return result
    }
}

data class VideoStats(
    val fps: Float = 0f,
    val framesReceived: Long = 0,
    val framesDecoded: Long = 0,
    val framesDropped: Long = 0,
    val codec: String = "none",
    val decoderName: String = "",
    val isHardware: Boolean = false,
    val width: Int = 0,
    val height: Int = 0,
    val codecResets: Int = 0,
    val bitrateKbps: Float = 0f,
)

enum class DecoderState {
    IDLE,
    CONFIGURING,
    DECODING,
    PAUSED,
    ERROR
}
