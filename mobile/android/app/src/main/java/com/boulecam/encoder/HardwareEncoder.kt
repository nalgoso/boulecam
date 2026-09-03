package com.boulecam.encoder

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.view.Surface
import java.nio.ByteBuffer

class HardwareEncoder(
    private val width: Int = 1920,
    private val height: Int = 1080,
    private val fps: Int = 60,
    private val bitrateBps: Int = 8_000_000, // 8 Mbps
    private val onFrameEncoded: (isKeyFrame: Boolean, timestampUs: Long, naluData: ByteArray) -> Unit
) {
    private var mediaCodec: MediaCodec? = null
    var inputSurface: Surface? = null
        private set

    @Volatile private var isRunning = false
    private var drainThread: Thread? = null
    private var cachedSpsPps: ByteArray? = null

    fun start() {
        if (isRunning) return

        val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, bitrateBps)
            setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1) // 1 second keyframe interval

            // Low-Latency flags
            setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                setInteger(MediaFormat.KEY_LATENCY, 0)
                setInteger(MediaFormat.KEY_PRIORITY, 0) // Real-time priority
            }
        }

        mediaCodec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC).apply {
            configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            inputSurface = createInputSurface()
            start()
        }

        isRunning = true
        drainThread = Thread { drainEncoder() }.apply {
            priority = Thread.MAX_PRIORITY
            start()
        }
    }

    @Volatile private var isSuspended = false

    fun requestKeyFrame() {
        val bundle = Bundle().apply {
            putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
        }
        mediaCodec?.setParameters(bundle)
    }

    fun setSuspended(suspend: Boolean) {
        val wasSuspended = isSuspended
        isSuspended = suspend
        if (wasSuspended && !suspend) {
            requestKeyFrame()
        }
    }

    private fun drainEncoder() {
        val bufferInfo = MediaCodec.BufferInfo()
        val codec = mediaCodec ?: return

        while (isRunning) {
            val outputBufferIndex = try {
                codec.dequeueOutputBuffer(bufferInfo, 2000)
            } catch (e: Exception) {
                -1
            }

            if (outputBufferIndex >= 0) {
                val outputBuffer: ByteBuffer? = try {
                    codec.getOutputBuffer(outputBufferIndex)
                } catch (e: Exception) {
                    null
                }

                if (outputBuffer != null && bufferInfo.size > 0) {
                    outputBuffer.position(bufferInfo.offset)
                    outputBuffer.limit(bufferInfo.offset + bufferInfo.size)

                    val chunk = ByteArray(bufferInfo.size)
                    outputBuffer.get(chunk)

                    val isKeyFrame = (bufferInfo.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0
                    val isConfig = (bufferInfo.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0

                    if (isConfig) {
                        cachedSpsPps = chunk.clone()
                    }

                    val finalPayload = if (isKeyFrame && cachedSpsPps != null && !isConfig) {
                        val combined = ByteArray(cachedSpsPps!!.size + chunk.size)
                        System.arraycopy(cachedSpsPps!!, 0, combined, 0, cachedSpsPps!!.size)
                        System.arraycopy(chunk, 0, combined, cachedSpsPps!!.size, chunk.size)
                        combined
                    } else {
                        chunk
                    }

                    if (!isSuspended) {
                        onFrameEncoded(isKeyFrame || isConfig, bufferInfo.presentationTimeUs, finalPayload)
                    }
                }

                try {
                    codec.releaseOutputBuffer(outputBufferIndex, false)
                } catch (ignored: Exception) {}
            }
        }
    }

    fun stop() {
        isRunning = false
        drainThread?.join(500)
        drainThread = null

        try {
            mediaCodec?.stop()
            mediaCodec?.release()
        } catch (ignored: Exception) {}
        mediaCodec = null
        inputSurface = null
    }
}
