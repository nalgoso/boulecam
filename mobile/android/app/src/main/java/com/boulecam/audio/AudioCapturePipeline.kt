package com.boulecam.audio

import android.annotation.SuppressLint
import android.content.Context
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioRecord
import android.media.MediaRecorder
import android.media.MicrophoneDirection
import android.media.MicrophoneInfo
import android.os.Build
import android.util.Log
import java.util.concurrent.atomic.AtomicBoolean

class AudioCapturePipeline(
    private val context: Context,
    private val sampleRate: Int = 48000,
    private val onAudioChunk: (ByteArray, Int, Int) -> Unit // (buffer, size, channels)
) {
    private val TAG = "BouleCamAudio"
    private val isRunning = AtomicBoolean(false)
    private var audioRecord: AudioRecord? = null
    private var workerThread: Thread? = null

    private var isStereo: Boolean = false
    private var selectedCapsule: Int = 0 // 0 = Auto, 1 = Bottom, 2 = Back, 3 = Front
    private var isBeamforming: Boolean = false

    fun setStereo(enabled: Boolean) {
        if (isStereo != enabled) {
            isStereo = enabled
            if (isRunning.get()) {
                restart()
            }
        }
    }

    fun setCapsule(capsule: Int) {
        if (selectedCapsule != capsule) {
            selectedCapsule = capsule
            applyMicrophoneConfiguration()
        }
    }

    fun setBeamforming(enabled: Boolean) {
        if (isBeamforming != enabled) {
            isBeamforming = enabled
            applyMicrophoneConfiguration()
        }
    }

    fun getChannels(): Int = if (isStereo) 2 else 1
    fun getCapsule(): Int = selectedCapsule
    fun isBeamformingActive(): Boolean = isBeamforming

    private fun restart() {
        stop()
        start()
    }

    @SuppressLint("MissingPermission")
    fun start() {
        if (isRunning.getAndSet(true)) return

        val channels = if (isStereo) 2 else 1
        val channelConfig = if (isStereo) AudioFormat.CHANNEL_IN_STEREO else AudioFormat.CHANNEL_IN_MONO
        val audioFormat = AudioFormat.ENCODING_PCM_16BIT
        val minBufSize = AudioRecord.getMinBufferSize(sampleRate, channelConfig, audioFormat)
        val bufferSize = maxOf(minBufSize, 4096 * channels)

        // Prioritize UNPROCESSED (Raw sensor, no aggressive manufacturer compression/AGC)
        val audioSources = mutableListOf<Int>()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            audioSources.add(MediaRecorder.AudioSource.UNPROCESSED)
        }
        audioSources.add(MediaRecorder.AudioSource.VOICE_RECOGNITION)
        audioSources.add(MediaRecorder.AudioSource.CAMCORDER)
        audioSources.add(MediaRecorder.AudioSource.MIC)

        var createdRecord: AudioRecord? = null
        for (source in audioSources) {
            try {
                val record = AudioRecord(source, sampleRate, channelConfig, audioFormat, bufferSize)
                if (record.state == AudioRecord.STATE_INITIALIZED) {
                    createdRecord = record
                    Log.i(TAG, "AudioRecord initialized using source: $source, channels: $channels, rate: $sampleRate Hz")
                    break
                } else {
                    record.release()
                }
            } catch (e: Exception) {
                Log.w(TAG, "Audio source $source unavailable: ${e.message}")
            }
        }

        if (createdRecord == null) {
            Log.e(TAG, "AudioRecord could not be initialized with any audio source")
            isRunning.set(false)
            return
        }

        audioRecord = createdRecord
        applyMicrophoneConfiguration()

        try {
            audioRecord?.startRecording()
            Log.i(TAG, "AudioRecord recording started: $sampleRate Hz, ${if (isStereo) "Stereo" else "Mono"}, 16-bit PCM")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to start AudioRecord", e)
            isRunning.set(false)
            return
        }

        workerThread = Thread({
            // 10 ms chunks: 48000 samples/sec * 2 bytes/sample * channels * 0.01s
            // Mono: 480 samples * 2 bytes = 960 bytes
            // Stereo: 480 samples * 4 bytes = 1920 bytes
            val chunkSamples = 480
            val chunkSizeBytes = chunkSamples * channels * 2
            val buffer = ByteArray(chunkSizeBytes)

            while (isRunning.get()) {
                val readBytes = audioRecord?.read(buffer, 0, chunkSizeBytes) ?: -1
                if (readBytes > 0) {
                    onAudioChunk(buffer, readBytes, channels)
                } else if (readBytes < 0) {
                    try { Thread.sleep(5) } catch (ignored: InterruptedException) { break }
                }
            }
        }, "BouleCamAudioCapture").apply {
            priority = Thread.MAX_PRIORITY
            start()
        }
    }

    private fun applyMicrophoneConfiguration() {
        val record = audioRecord ?: return

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            try {
                // Direction: 0=Auto, 1=Bottom, 2=Back, 3=Front
                val direction = when (selectedCapsule) {
                    2 -> MicrophoneDirection.MIC_DIRECTION_AWAY_FROM_USER // Back / Camera mic
                    3 -> MicrophoneDirection.MIC_DIRECTION_TOWARDS_USER   // Front / Selfie mic
                    else -> MicrophoneDirection.MIC_DIRECTION_UNSPECIFIED // Auto / Bottom
                }
                record.setPreferredMicrophoneDirection(direction)

                // Beamforming: 1.0f = narrow polar pattern (beamformed), 0.0f = omni
                val zoom = if (isBeamforming) 1.0f else 0.0f
                record.setPreferredMicrophoneFieldDimension(zoom)

                Log.i(TAG, "Applied mic configuration: capsule=$selectedCapsule (dir=$direction), beamforming=$isBeamforming")
            } catch (e: Exception) {
                Log.w(TAG, "setPreferredMicrophoneDirection not supported on this device: ${e.message}")
            }
        }

        // Check for specific input device match (API 23+)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            try {
                val audioManager = context.getSystemService(Context.AUDIO_SERVICE) as? AudioManager
                val inputDevices = audioManager?.getDevices(AudioManager.GET_DEVICES_INPUTS)
                if (!inputDevices.isNullOrEmpty()) {
                    val targetDevice = inputDevices.firstOrNull { it.isSource }
                    if (targetDevice != null) {
                        record.setPreferredDevice(targetDevice)
                    }
                }
            } catch (e: Exception) {
                Log.w(TAG, "setPreferredDevice error: ${e.message}")
            }
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
