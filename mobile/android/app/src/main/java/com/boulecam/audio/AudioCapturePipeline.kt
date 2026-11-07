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
        // Optimal low-latency buffer size: double the minimal hardware burst size
        val bufferSize = maxOf(minBufSize, 1920)

        var record: AudioRecord? = null

        val audioSource = when {
            ambient -> {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
                    MediaRecorder.AudioSource.UNPROCESSED
                } else {
                    MediaRecorder.AudioSource.VOICE_RECOGNITION
                }
            }
            else -> MediaRecorder.AudioSource.CAMCORDER
        }

        // Low-latency AudioRecord Builder for Android 8.0+ (API 26+)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            try {
                val format = AudioFormat.Builder()
                    .setEncoding(audioFormat)
                    .setSampleRate(sampleRate)
                    .setChannelMask(channelConfig)
                    .build()

                record = AudioRecord.Builder()
                    .setAudioSource(audioSource)
                    .setAudioFormat(format)
                    .setBufferSizeInBytes(bufferSize)
                    .build()
            } catch (e: Exception) {
                Log.w(TAG, "AudioRecord.Builder fallback: ${e.message}")
            }
        }

        // Fallback for standard instantiation if Builder or specific source is not available
        if (record == null || record.state != AudioRecord.STATE_INITIALIZED) {
            record?.release()
            try {
                record = AudioRecord(
                    audioSource,
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
                Log.i(TAG, "Ultra-low-latency AudioRecord active: $sampleRate Hz, Mono, 16-bit PCM (Ambient=$isAmbientMode)")
            } catch (e: Exception) {
                Log.e(TAG, "Failed to start AudioRecord", e)
                isRunning.set(false)
                return
            }
        }

        workerThread = Thread({
            // 10 ms ultra-low latency chunks: 48000 samples/sec * 2 bytes/sample * 0.010s = 960 bytes
            val chunkSizeBytes = 960
            val buffer = ByteArray(chunkSizeBytes)

            while (isRunning.get()) {
                val record = synchronized(recordLock) { audioRecord } ?: break
                val readBytes = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                    record.read(buffer, 0, chunkSizeBytes, AudioRecord.READ_BLOCKING)
                } else {
                    record.read(buffer, 0, chunkSizeBytes)
                }

                if (readBytes > 0) {
                    onAudioChunk(buffer, readBytes)
                } else if (readBytes < 0) {
                    try { Thread.sleep(5) } catch (ignored: InterruptedException) { break }
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
