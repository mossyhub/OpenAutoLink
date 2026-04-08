package com.openautolink.app.transport

import android.net.Network
import android.util.Log
import com.openautolink.app.audio.AudioFrame
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.flowOn
import kotlinx.coroutines.isActive
import java.io.DataInputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import kotlin.coroutines.coroutineContext

/**
 * TCP audio channel — reads/writes OAL binary audio frames to/from the bridge (port 5289).
 * Bidirectional: bridge→app (playback, direction=0), app→bridge (mic, direction=1).
 *
 * 8-byte header:
 *   direction(u8) + purpose(u8) + sample_rate(u16le) + channels(u8) + payload_length(u24le)
 */
class TcpAudioChannel {

    companion object {
        private const val TAG = "TcpAudioChannel"
        private const val HEADER_SIZE = 8
    }

    private var socket: Socket? = null
    private var input: DataInputStream? = null
    private var output: OutputStream? = null

    val isConnected: Boolean get() = socket?.isConnected == true && socket?.isClosed == false

    fun connect(host: String, port: Int, timeoutMs: Int = 5000, network: Network? = null) {
        close()
        val s = Socket()
        network?.bindSocket(s)
        s.tcpNoDelay = true
        s.keepAlive = true
        s.soTimeout = 0 // Block waiting for frames
        s.connect(InetSocketAddress(host, port), timeoutMs)
        socket = s
        input = DataInputStream(s.getInputStream().buffered(65536))
        output = s.getOutputStream().buffered(65536)
        Log.i(TAG, "Connected to audio channel at $host:$port")
    }

    /**
     * Read playback audio frames as a Flow. Blocks on IO waiting for next frame.
     * Emits AudioFrame for each complete frame read from the TCP stream.
     */
    fun receiveFrames(): Flow<AudioFrame> = flow {
        val stream = input ?: throw IllegalStateException("Not connected")
        val headerBuf = ByteArray(HEADER_SIZE)

        while (coroutineContext.isActive) {
            // Read the 8-byte header
            stream.readFully(headerBuf)

            val direction = headerBuf[0].toInt() and 0xFF
            val purposeByte = headerBuf[1].toInt() and 0xFF
            val sampleRate = readU16LE(headerBuf, 2)
            val channels = headerBuf[4].toInt() and 0xFF
            val payloadLength = readU24LE(headerBuf, 5)

            val purpose = AudioFrame.purposeFromByte(purposeByte)
            if (purpose == null) {
                // Unknown purpose — skip payload
                Log.w(TAG, "Unknown audio purpose byte: $purposeByte, skipping $payloadLength bytes")
                if (payloadLength > 0) {
                    stream.skipBytes(payloadLength)
                }
                continue
            }

            if (payloadLength > AudioFrame.MAX_PAYLOAD_SIZE) {
                Log.e(TAG, "Audio payload too large: $payloadLength bytes, dropping connection")
                break
            }

            if (payloadLength == 0) {
                // Empty frame (e.g., silence marker)
                emit(AudioFrame(direction, purpose, sampleRate, channels, ByteArray(0)))
                continue
            }

            val payload = ByteArray(payloadLength)
            stream.readFully(payload)

            emit(AudioFrame(direction, purpose, sampleRate, channels, payload))
        }
    }.flowOn(Dispatchers.IO)

    /**
     * Send a mic audio frame to the bridge (direction=1).
     * Must be called from a coroutine or a dedicated thread.
     */
    @Synchronized
    fun sendFrame(frame: AudioFrame) {
        val out = output ?: return
        val header = ByteArray(HEADER_SIZE)
        header[0] = frame.direction.toByte()
        header[1] = AudioFrame.purposeToByte(frame.purpose).toByte()
        writeU16LE(header, 2, frame.sampleRate)
        header[4] = frame.channels.toByte()
        writeU24LE(header, 5, frame.data.size)

        out.write(header)
        if (frame.data.isNotEmpty()) {
            out.write(frame.data)
        }
        out.flush()
    }

    fun close() {
        try { output?.close() } catch (_: Exception) {}
        try { input?.close() } catch (_: Exception) {}
        try { socket?.close() } catch (_: Exception) {}
        output = null
        input = null
        socket = null
    }

    private fun readU16LE(buf: ByteArray, offset: Int): Int {
        return (buf[offset].toInt() and 0xFF) or
                ((buf[offset + 1].toInt() and 0xFF) shl 8)
    }

    private fun readU24LE(buf: ByteArray, offset: Int): Int {
        return (buf[offset].toInt() and 0xFF) or
                ((buf[offset + 1].toInt() and 0xFF) shl 8) or
                ((buf[offset + 2].toInt() and 0xFF) shl 16)
    }

    private fun writeU16LE(buf: ByteArray, offset: Int, value: Int) {
        buf[offset] = (value and 0xFF).toByte()
        buf[offset + 1] = ((value shr 8) and 0xFF).toByte()
    }

    private fun writeU24LE(buf: ByteArray, offset: Int, value: Int) {
        buf[offset] = (value and 0xFF).toByte()
        buf[offset + 1] = ((value shr 8) and 0xFF).toByte()
        buf[offset + 2] = ((value shr 16) and 0xFF).toByte()
    }
}
