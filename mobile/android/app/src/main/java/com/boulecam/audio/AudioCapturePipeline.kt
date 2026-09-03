package com.boulecam.audio

import android.annotation.SuppressLint
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.util.Log
import java.util.concurrent.atomic.AtomicBoolean

class AudioCapturePipeline(
    private val sampleRate: Int = 48000,
    private val onAudioChunk: (ByteArray, Int) -> Unit
) {
    private val TAG = "BouleCamAudio"
    private val isRunning = AtomicBoolean(false)
    private var audioRecord: AudioRecord? = null
    private var workerThread: Thread? = null

    @SuppressLint("MissingPermission")
    fun start() {
        if (isRunning.getAndSet(true)) return

        val channelConfig = AudioFormat.CHANNEL_IN_MONO
        val audioFormat = AudioFormat.ENCODING_PCM_16BIT
        val minBufSize = AudioRecord.getMinBufferSize(sampleRate, channelConfig, audioFormat)
        val bufferSize = maxOf(minBufSize, 4096)

        try {
            // Prefer CAMCORDER source as it uses noise cancellation optimized for video recording
            audioRecord = AudioRecord(
                MediaRecorder.AudioSource.CAMCORDER,
                sampleRate,
                channelConfig,
                audioFormat,
                bufferSize
            )

            if (audioRecord?.state != AudioRecord.STATE_INITIALIZED) {
                audioRecord?.release()
                audioRecord = AudioRecord(
                    MediaRecorder.AudioSource.MIC,
                    sampleRate,
                    channelConfig,
                    audioFormat,
                    bufferSize
                )
            }

            if (audioRecord?.state != AudioRecord.STATE_INITIALIZED) {
                Log.e(TAG, "AudioRecord could not be initialized")
                isRunning.set(false)
                return
            }

            audioRecord?.startRecording()
            Log.i(TAG, "AudioRecord started: $sampleRate Hz, Mono, 16-bit PCM")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to start AudioRecord", e)
            isRunning.set(false)
            return
        }

        workerThread = Thread({
            // 20 ms chunks: 48000 samples/sec * 2 bytes/sample * 0.02s = 1920 bytes
            val chunkSizeBytes = 1920
            val buffer = ByteArray(chunkSizeBytes)

            while (isRunning.get()) {
                val readBytes = audioRecord?.read(buffer, 0, chunkSizeBytes) ?: -1
                if (readBytes > 0) {
                    onAudioChunk(buffer, readBytes)
                } else if (readBytes < 0) {
                    try { Thread.sleep(10) } catch (ignored: InterruptedException) { break }
                }
            }
        }, "BouleCamAudioCapture").apply {
            priority = Thread.MAX_PRIORITY
            start()
        }
    }

    fun stop() {
        if (!isRunning.getAndSet(false)) return
        try {
            workerThread?.interrupt()
            workerThread = null
            audioRecord?.stop()
            audioRecord?.release()
            audioRecord = null
            Log.i(TAG, "AudioRecord stopped")
        } catch (e: Exception) {
            Log.e(TAG, "Error stopping AudioRecord", e)
        }
    }
}
