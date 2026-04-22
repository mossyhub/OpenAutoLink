package com.openautolink.app.transport.direct

import com.google.protobuf.MessageLite
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.nio.ByteBuffer

/**
 * AA wire protocol message — the fundamental unit of communication.
 *
 * Wire format (4-byte header + 2-byte msg type + protobuf payload):
 * ```
 * [0]     Channel (1 byte)
 * [1]     Flags (1 byte) — 0x0b for data, 0x0f for control on non-CTR channels
 * [2..3]  Length (2 bytes, big-endian) — includes MsgType size (2)
 * [4..5]  MsgType (2 bytes, big-endian)
 * [6..]   Protobuf payload
 * ```
 */
class AaMessage(
    val channel: Int,
    val flags: Int,
    val type: Int,
    val payload: ByteArray,      // protobuf payload only (no header, no type)
    val payloadOffset: Int = 0,
    val payloadLength: Int = payload.size,
) {
    /** Total wire size including header + type + payload. */
    val wireSize: Int get() = HEADER_SIZE + TYPE_SIZE + payloadLength

    val isVideo: Boolean get() = channel == AaChannel.VIDEO
    val isAudio: Boolean get() = AaChannel.isAudio(channel)

    override fun toString(): String =
        "${AaChannel.name(channel)} type=$type flags=0x${flags.toString(16)} payload=${payloadLength}B"

    companion object {
        const val HEADER_SIZE = 4
        const val TYPE_SIZE = 2
        const val MAX_PAYLOAD = 16384  // 16KB — AA frames are small (except video)

        /** Build flags byte for a channel + message type combination. */
        fun flags(channel: Int, type: Int): Int {
            // On non-control channels, control-range messages get flag 0x0f
            return if (channel != AaChannel.CONTROL && AaMsgType.isControl(type)) 0x0f else 0x0b
        }

        /** Create an AaMessage from a protobuf object. */
        fun fromProto(channel: Int, type: Int, proto: MessageLite): AaMessage {
            val payload = proto.toByteArray()
            return AaMessage(channel, flags(channel, type), type, payload)
        }

        /** Create a raw AaMessage (non-protobuf, e.g., version request). */
        fun raw(channel: Int, type: Int, payload: ByteArray = ByteArray(0)): AaMessage {
            return AaMessage(channel, flags(channel, type), type, payload)
        }
    }
}

/**
 * Encodes/decodes AA wire protocol messages on a TCP stream.
 *
 * Thread safety: This class is NOT thread-safe. Use separate instances for
 * read and write, or synchronize externally. The read side is typically on
 * a dedicated IO thread; the write side is dispatched from coroutines.
 */
class AaWireCodec {

    /**
     * Write a single AA message to the output stream.
     * Thread-safe if the underlying OutputStream is thread-safe (or synchronized externally).
     */
    fun encode(msg: AaMessage, out: OutputStream) {
        val header = ByteArray(AaMessage.HEADER_SIZE + AaMessage.TYPE_SIZE)
        header[0] = msg.channel.toByte()
        header[1] = msg.flags.toByte()
        // Length = type size (2) + payload length
        val length = AaMessage.TYPE_SIZE + msg.payloadLength
        header[2] = (length shr 8).toByte()
        header[3] = (length and 0xFF).toByte()
        header[4] = (msg.type shr 8).toByte()
        header[5] = (msg.type and 0xFF).toByte()
        out.write(header)
        if (msg.payloadLength > 0) {
            out.write(msg.payload, msg.payloadOffset, msg.payloadLength)
        }
        out.flush()
    }

    /**
     * Read a single AA message from the input stream.
     * Blocks until a complete message is available.
     * @throws IOException on stream error or EOF
     */
    fun decode(input: InputStream): AaMessage {
        // Read 4-byte header
        val header = readFully(input, AaMessage.HEADER_SIZE)
        val channel = header[0].toInt() and 0xFF
        val flags = header[1].toInt() and 0xFF
        val length = ((header[2].toInt() and 0xFF) shl 8) or (header[3].toInt() and 0xFF)

        if (length < AaMessage.TYPE_SIZE) {
            throw IOException("AA message length too small: $length")
        }
        if (length > AaMessage.MAX_PAYLOAD + AaMessage.TYPE_SIZE) {
            throw IOException("AA message length too large: $length")
        }

        // Read type + payload
        val body = readFully(input, length)
        val type = ((body[0].toInt() and 0xFF) shl 8) or (body[1].toInt() and 0xFF)
        val payloadLength = length - AaMessage.TYPE_SIZE

        return AaMessage(
            channel = channel,
            flags = flags,
            type = type,
            payload = body,
            payloadOffset = AaMessage.TYPE_SIZE,
            payloadLength = payloadLength,
        )
    }

    /**
     * Write the raw version request (special format: 4 bytes, no type field).
     * `00 01 00 01` for version request, or `00 02 00 00` per HURev.
     */
    fun writeVersionRequest(out: OutputStream) {
        // Channel 0, flags 0x01 (version), length 0x0001
        out.write(byteArrayOf(0x00, 0x01, 0x00, 0x01))
        out.flush()
    }

    /**
     * Read the version response (4-byte header, variable payload).
     * Returns the major and minor version from the response.
     */
    fun readVersionResponse(input: InputStream): Pair<Int, Int> {
        val header = readFully(input, AaMessage.HEADER_SIZE)
        val channel = header[0].toInt() and 0xFF
        val flags = header[1].toInt() and 0xFF
        val length = ((header[2].toInt() and 0xFF) shl 8) or (header[3].toInt() and 0xFF)

        if (channel != 0 || flags != 2) {
            throw IOException("Expected version response, got channel=$channel flags=$flags")
        }

        val body = readFully(input, length)
        // Version response: type(2) + status(varint) + major(2) + minor(2)
        // Parse protobuf: status=field1, major=field2, minor=field3
        val type = ((body[0].toInt() and 0xFF) shl 8) or (body[1].toInt() and 0xFF)
        if (type != AaMsgType.VERSION_RESPONSE) {
            throw IOException("Expected VERSION_RESPONSE(2), got type=$type")
        }

        // Parse the protobuf payload for major/minor version
        // Simple manual varint parsing — no generated class needed
        return try {
            var major = 1
            var minor = 7
            var pos = AaMessage.TYPE_SIZE
            while (pos < body.size) {
                val tag = body[pos].toInt() and 0xFF
                pos++
                val fieldNum = tag shr 3
                val wireType = tag and 0x07
                if (wireType == 0) { // varint
                    var value = 0
                    var shift = 0
                    while (pos < body.size) {
                        val b = body[pos].toInt() and 0xFF
                        pos++
                        value = value or ((b and 0x7F) shl shift)
                        if (b and 0x80 == 0) break
                        shift += 7
                    }
                    when (fieldNum) {
                        2 -> major = value
                        3 -> minor = value
                    }
                }
            }
            Pair(major, minor)
        } catch (_: Exception) {
            Pair(1, 7)
        } catch (e: Exception) {
            Pair(1, 6) // Default: version 1.6
        }
    }

    private fun readFully(input: InputStream, length: Int): ByteArray {
        val buf = ByteArray(length)
        var offset = 0
        while (offset < length) {
            val n = input.read(buf, offset, length - offset)
            if (n < 0) throw IOException("EOF: expected $length bytes, got $offset")
            offset += n
        }
        return buf
    }
}
