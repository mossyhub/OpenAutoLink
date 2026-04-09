package com.openautolink.app.audio

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.os.Process
import android.util.Log
import com.openautolink.app.transport.AudioPurpose
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong

/**
 * One AudioTrack + AudioRingBuffer per audio purpose.
 * - Ring buffer absorbs TCP/network jitter (500ms capacity)
 * - Dedicated URGENT_AUDIO drain thread writes at steady 10ms intervals
 * - Pre-fill 80ms before calling AudioTrack.play()
 * - Non-blocking feedPcm() — TCP thread never blocks on AudioTrack
 */
class AudioPurposeSlot(
    val purpose: AudioPurpose,
    val sampleRate: Int,
    val channelCount: Int,
    private val bufferDurationMs: Int = 500
) {
    companion object {
        private const val TAG = "AudioPurposeSlot"
        /** Drain thread writes 10ms chunks to AudioTrack. */
        private const val DRAIN_INTERVAL_MS = 10L
        /** Pre-fill 80ms before starting playback to absorb initial jitter. */
        private const val PREFILL_MS = 80
    }

    private var audioTrack: AudioTrack? = null
    private var ringBuffer: AudioRingBuffer? = null
    private var drainThread: Thread? = null

    private val active = AtomicBoolean(false)
    private val released = AtomicBoolean(false)
    private val pausedByFocusLoss = AtomicBoolean(false)
    private val draining = AtomicBoolean(false)
    private val prefilled = AtomicBoolean(false)

    val framesWritten = AtomicLong(0)
    val underrunCount = AtomicLong(0)

    /** Bytes per millisecond for this format. */
    private val bytesPerMs: Int get() = sampleRate * channelCount * 2 / 1000

    fun initialize() {
        if (released.get()) return

        val channelMask = if (channelCount == 2)
            AudioFormat.CHANNEL_OUT_STEREO else AudioFormat.CHANNEL_OUT_MONO

        val minBufSize = AudioTrack.getMinBufferSize(
            sampleRate, channelMask, AudioFormat.ENCODING_PCM_16BIT
        )
        // Use 4x min buffer — same as app_v1 production
        val trackBufSize = maxOf(minBufSize * 4, 16384)

        audioTrack = AudioTrack.Builder()
            .setAudioAttributes(buildAudioAttributes(purpose))
            .setAudioFormat(AudioFormat.Builder()
                .setSampleRate(sampleRate)
                .setChannelMask(channelMask)
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .build())
            .setBufferSizeInBytes(trackBufSize)
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()

        // Ring buffer: 500ms capacity for jitter absorption
        val ringCapacity = bytesPerMs * bufferDurationMs
        ringBuffer = AudioRingBuffer(maxOf(ringCapacity, 16384))

        Log.d(TAG, "Initialized $purpose: ${sampleRate}Hz ${channelCount}ch, track=${trackBufSize}B, ring=${ringBuffer!!.capacity}B")
    }

    fun start() {
        if (released.get() || active.get()) return
        val track = audioTrack ?: return
        active.set(true)
        prefilled.set(false)

        // Start drain thread — writes ring buffer to AudioTrack at steady pace
        startDrainThread(track)

        Log.d(TAG, "$purpose started")
    }

    fun stop() {
        pausedByFocusLoss.set(false)
        if (!active.getAndSet(false)) return
        stopDrainThread()
        audioTrack?.pause()
        audioTrack?.flush()
        ringBuffer?.clear()
        prefilled.set(false)
        Log.d(TAG, "$purpose stopped")
    }

    fun pause() {
        if (!active.getAndSet(false)) return
        pausedByFocusLoss.set(true)
        stopDrainThread()
        audioTrack?.pause()
        Log.d(TAG, "$purpose paused")
    }

    fun resume() {
        if (released.get() || active.get()) return
        if (!pausedByFocusLoss.getAndSet(false)) return
        val track = audioTrack ?: return
        active.set(true)
        startDrainThread(track)
        Log.d(TAG, "$purpose resumed")
    }

    val isPausedByFocus: Boolean get() = pausedByFocusLoss.get()

    /**
     * Write PCM into ring buffer. Non-blocking — never stalls the TCP thread.
     * The drain thread will pick it up and write to AudioTrack.
     */
    fun feedPcm(data: ByteArray) {
        if (!active.get()) return
        ringBuffer?.write(data) ?: return
    }

    fun setVolume(volume: Float) {
        audioTrack?.setVolume(volume.coerceIn(0f, 1f))
    }

    fun release() {
        if (released.getAndSet(true)) return
        stop()
        audioTrack?.release()
        audioTrack = null
        ringBuffer = null
        Log.d(TAG, "$purpose released")
    }

    val isActive: Boolean get() = active.get()
    val ringBufferAvailable: Int get() = ringBuffer?.available ?: 0
    val ringBufferCapacity: Int get() = ringBuffer?.capacity ?: 0

    /**
     * Drain thread: reads from ring buffer and writes to AudioTrack at steady 10ms intervals.
     * Runs at URGENT_AUDIO priority. AudioTrack.write() paces to hardware clock.
     */
    private fun startDrainThread(track: AudioTrack) {
        if (draining.getAndSet(true)) return

        drainThread = Thread({
            Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)
            val chunkBytes = bytesPerMs * DRAIN_INTERVAL_MS.toInt()
            val buf = ByteArray(chunkBytes)
            val prefillBytes = bytesPerMs * PREFILL_MS

            while (draining.get()) {
                val ring = ringBuffer ?: break

                // Pre-fill: wait until enough data arrives before starting playback
                if (!prefilled.get()) {
                    if (ring.available >= prefillBytes) {
                        prefilled.set(true)
                        track.play()
                    } else {
                        try { Thread.sleep(DRAIN_INTERVAL_MS) } catch (_: InterruptedException) { break }
                        continue
                    }
                }

                val read = ring.read(buf, 0, chunkBytes)
                if (read > 0) {
                    track.write(buf, 0, read)
                    framesWritten.addAndGet(read.toLong() / (channelCount * 2))
                } else {
                    // Underrun — ring is empty, AudioTrack plays silence
                    underrunCount.incrementAndGet()
                    try { Thread.sleep(DRAIN_INTERVAL_MS) } catch (_: InterruptedException) { break }
                }
            }
        }, "AudioDrain-$purpose").also {
            it.isDaemon = true
            it.start()
        }
    }

    private fun stopDrainThread() {
        draining.set(false)
        drainThread?.interrupt()
        drainThread?.join(200)
        drainThread = null
    }

    private fun buildAudioAttributes(purpose: AudioPurpose): AudioAttributes {
        val usage = when (purpose) {
            AudioPurpose.MEDIA -> AudioAttributes.USAGE_MEDIA
            AudioPurpose.NAVIGATION -> AudioAttributes.USAGE_ASSISTANCE_NAVIGATION_GUIDANCE
            AudioPurpose.ASSISTANT -> AudioAttributes.USAGE_ASSISTANT
            AudioPurpose.PHONE_CALL -> AudioAttributes.USAGE_VOICE_COMMUNICATION
            AudioPurpose.ALERT -> AudioAttributes.USAGE_NOTIFICATION_RINGTONE
        }
        val contentType = when (purpose) {
            AudioPurpose.MEDIA -> AudioAttributes.CONTENT_TYPE_MUSIC
            AudioPurpose.PHONE_CALL -> AudioAttributes.CONTENT_TYPE_SPEECH
            AudioPurpose.ASSISTANT -> AudioAttributes.CONTENT_TYPE_SPEECH
            AudioPurpose.NAVIGATION -> AudioAttributes.CONTENT_TYPE_SPEECH
            AudioPurpose.ALERT -> AudioAttributes.CONTENT_TYPE_SONIFICATION
        }
        return AudioAttributes.Builder()
            .setUsage(usage)
            .setContentType(contentType)
            .build()
    }
}