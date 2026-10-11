package com.boulecam.audio

import android.annotation.SuppressLint
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.os.Build
import android.util.Log
import java.util.concurrent.atomic.AtomicBoolean

class AudioCapturePipeline(
    private val sampleRate: Int = 48000,
    private val onAudioChunk: (ByteArray, Int) -> Unit
) {
    private val TAG = "BouleCamAudio"
    private val isRunning = AtomicBoolean(false)
    private var isAmbientMode = false
    private var audioRecord: AudioRecord? = null
    private var workerThread: Thread? = null
    private val recordLock = Any()

    @SuppressLint("MissingPermission")
    fun setAmbientMode(ambient: Boolean) {
        if (isAmbientMode == ambient) return
        isAmbientMode = ambient
        Log.i(TAG, "Audio mode updated: ambient=$ambient")
        if (isRunning.get()) {
            synchronized(recordLock) {
                try {
                    audioRecord?.stop()
                    audioRecord?.release()
                    audioRecord = createAudioRecord(ambient)
                    audioRecord?.startRecording()
                    Log.i(TAG, "AudioRecord hot-swapped to ambient=$ambient")
                } catch (e: Exception) {
                    Log.e(TAG, "Error switching AudioRecord mode", e)
                }
            }
        }
    }

    @SuppressLint("MissingPermission")
    private fun createAudioRecord(ambient: Boolean): AudioRecord? {
        val channelConfig = AudioFormat.CHANNEL_IN_MONO
        val audioFormat = AudioFormat.ENCODING_PCM_16BIT
        val minBufSize = AudioRecord.getMinBufferSize(sampleRate, channelConfig, audioFormat)
        val bufferSize = maxOf(minBufSize, 4096)

        var record: AudioRecord? = null

        if (ambient) {
            // Ambient / Congregation mode: try UNPROCESSED (raw, disables AGC & aggressive compression)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
                try {
                    record = AudioRecord(
                        MediaRecorder.AudioSource.UNPROCESSED,
                        sampleRate,
                        channelConfig,
                        audioFormat,
                        bufferSize
                    )
                } catch (ignored: Exception) {}
            }
            if (record?.state != AudioRecord.STATE_INITIALIZED) {
                record?.release()
                // Fallback to VOICE_RECOGNITION (also disables AGC on most phones)
                try {
                    record = AudioRecord(
                        MediaRecorder.AudioSource.VOICE_RECOGNITION,
                        sampleRate,
                        channelConfig,
                        audioFormat,
                        bufferSize
                    )
                } catch (ignored: Exception) {}
            }
        } else {
            // Main mic mode: CAMCORDER (optimized for speech and video recording)
            try {
                record = AudioRecord(
                    MediaRecorder.AudioSource.CAMCORDER,
                    sampleRate,
                    channelConfig,
                    audioFormat,
                    bufferSize
                )
            } catch (ignored: Exception) {}
        }

        // Final fallback to MIC if preferred source failed
        if (record?.state != AudioRecord.STATE_INITIALIZED) {
            record?.release()
            try {
                record = AudioRecord(
                    MediaRecorder.AudioSource.MIC,
                    sampleRate,
                    channelConfig,
                    audioFormat,
                    bufferSize
                )
            } catch (ignored: Exception) {}
        }

        return record
    }

    @SuppressLint("MissingPermission")
    fun start() {
        if (isRunning.getAndSet(true)) return

        synchronized(recordLock) {
            audioRecord = createAudioRecord(isAmbientMode)
            if (audioRecord?.state != AudioRecord.STATE_INITIALIZED) {
                Log.e(TAG, "AudioRecord could not be initialized")
                isRunning.set(false)
                return
            }

            try {
                audioRecord?.startRecording()
                Log.i(TAG, "AudioRecord started: $sampleRate Hz, Mono, 16-bit PCM (Ambient=$isAmbientMode)")
            } catch (e: Exception) {
                Log.e(TAG, "Failed to start AudioRecord", e)
                isRunning.set(false)
                return
            }
        }

        workerThread = Thread({
            // 20 ms chunks: 48000 samples/sec * 2 bytes/sample * 0.02s = 1920 bytes
            val chunkSizeBytes = 1920
            val buffer = ByteArray(chunkSizeBytes)

            while (isRunning.get()) {
                val record = synchronized(recordLock) { audioRecord }
                val readBytes = record?.read(buffer, 0, chunkSizeBytes) ?: -1
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
            synchronized(recordLock) {
                audioRecord?.stop()
                audioRecord?.release()
                audioRecord = null
            }
            Log.i(TAG, "AudioRecord stopped")
        } catch (e: Exception) {
            Log.e(TAG, "Error stopping AudioRecord", e)
        }
    }
}
