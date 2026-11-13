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
    @Volatile private var cachedSpsPps: ByteArray? = null

    fun getCachedSpsPps(): ByteArray? = cachedSpsPps

    private fun extractSpsPpsFromFormat(format: MediaFormat?) {
        if (format == null) return
        try {
            val csd0 = format.getByteBuffer("csd-0") ?: return
            val csd0Dup = csd0.duplicate()
            val spsBytes = ByteArray(csd0Dup.remaining())
            csd0Dup.get(spsBytes)

            val csd1 = format.getByteBuffer("csd-1")
            if (csd1 != null) {
                val csd1Dup = csd1.duplicate()
                val ppsBytes = ByteArray(csd1Dup.remaining())
                csd1Dup.get(ppsBytes)
                val combined = ByteArray(spsBytes.size + ppsBytes.size)
                System.arraycopy(spsBytes, 0, combined, 0, spsBytes.size)
                System.arraycopy(ppsBytes, 0, combined, spsBytes.size, ppsBytes.size)
                if (combined.isNotEmpty()) {
                    cachedSpsPps = combined
                }
            } else if (spsBytes.isNotEmpty()) {
                cachedSpsPps = spsBytes
            }
        } catch (_: Exception) {}
    }

    private fun chunkStartsWithSps(data: ByteArray): Boolean {
        if (data.size >= 5) {
            if (data[0].toInt() == 0 && data[1].toInt() == 0 && data[2].toInt() == 1) {
                return (data[3].toInt() and 0x1F) == 7
            }
            if (data[0].toInt() == 0 && data[1].toInt() == 0 && data[2].toInt() == 0 && data[3].toInt() == 1) {
                return (data[4].toInt() and 0x1F) == 7
            }
        }
        return false
    }

    private fun extractLeadingSpsPpsFromAnnexB(data: ByteArray): ByteArray? {
        if (!chunkStartsWithSps(data)) return null
        var i = 0
        var idrOffset = -1
        while (i + 4 < data.size) {
            if (data[i].toInt() == 0 && data[i + 1].toInt() == 0) {
                if (data[i + 2].toInt() == 1) {
                    val nalType = data[i + 3].toInt() and 0x1F
                    if (nalType == 5) {
                        idrOffset = i
                        break
                    }
                    i += 3
                    continue
                } else if (data[i + 2].toInt() == 0 && data[i + 3].toInt() == 1) {
                    val nalType = data[i + 4].toInt() and 0x1F
                    if (nalType == 5) {
                        idrOffset = i
                        break
                    }
                    i += 4
                    continue
                }
            }
            i++
        }
        return if (idrOffset > 0) data.copyOfRange(0, idrOffset) else null
    }

    fun start() {
        if (isRunning) return

        val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, bitrateBps)
            setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1) // 1 second keyframe interval for instant multi-camera switching & recovery

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

            if (outputBufferIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                try {
                    extractSpsPpsFromFormat(codec.outputFormat)
                } catch (_: Exception) {}
                continue
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
                    } else if (isKeyFrame && cachedSpsPps == null) {
                        extractLeadingSpsPpsFromAnnexB(chunk)?.let {
                            cachedSpsPps = it
                        }
                    }

                    val spsPps = cachedSpsPps
                    val finalPayload = if (isKeyFrame && spsPps != null && !isConfig && !chunkStartsWithSps(chunk)) {
                        val combined = ByteArray(spsPps.size + chunk.size)
                        System.arraycopy(spsPps, 0, combined, 0, spsPps.size)
                        System.arraycopy(chunk, 0, combined, spsPps.size, chunk.size)
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
