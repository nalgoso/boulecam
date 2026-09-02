import Foundation
import VideoToolbox
import CoreMedia

public class H264HardwareEncoder {
    private var compressionSession: VTCompressionSession?
    private let width: Int32
    private let height: Int32
    private let fps: Int32
    private let bitrate: Int32
    
    public var onFrameEncoded: ((_ isKeyFrame: Bool, _ timestampUs: UInt64, _ naluData: Data) -> Void)?
    
    public init(width: Int32 = 1920, height: Int32 = 1080, fps: Int32 = 60, bitrate: Int32 = 8_000_000) {
        self.width = width
        self.height = height
        self.fps = fps
        self.bitrate = bitrate
        setupSession()
    }
    
    deinit {
        stop()
    }
    
    private func setupSession() {
        let callback: VTCompressionOutputCallback = { outputCallbackRefCon, _, status, flags, sampleBuffer in
            guard status == noErr, let sampleBuffer = sampleBuffer, let refCon = outputCallbackRefCon else { return }
            let encoder = Unmanaged<H264HardwareEncoder>.fromOpaque(refCon).takeUnretainedValue()
            encoder.processSampleBuffer(sampleBuffer, flags: flags)
        }
        
        let status = VTCompressionSessionCreate(
            allocator: kCFAllocatorDefault,
            width: width,
            height: height,
            codecType: kCMVideoCodecType_H264,
            encoderSpecification: nil,
            imageBufferAttributes: nil,
            compressedDataAllocator: nil,
            outputCallback: callback,
            refcon: Unmanaged.passUnretained(self).toOpaque(),
            compressionSessionOut: &compressionSession
        )
        
        guard status == noErr, let session = compressionSession else { return }
        
        // Configure Ultra Low Latency properties
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue)
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_ProfileLevel, value: kVTProfileLevel_H264_High_AutoLevel)
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_AverageBitRate, value: bitrate as CFNumber)
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_ExpectedFrameRate, value: fps as CFNumber)
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, value: 1.0 as CFNumber)
        VTSessionSetProperty(session, key: kVTCompressionPropertyKey_AllowFrameReordering, value: kCFBooleanFalse)
        
        VTCompressionSessionPrepareToEncodeFrames(session)
    }
    
    public func encode(pixelBuffer: CVPixelBuffer, presentationTime: CMTime) {
        guard let session = compressionSession else { return }
        VTCompressionSessionEncodeFrame(
            session,
            imageBuffer: pixelBuffer,
            presentationTimeStamp: presentationTime,
            duration: CMTime(value: 1, timescale: fps),
            frameProperties: nil,
            sourceFrameRefcon: nil,
            infoFlagsOut: nil
        )
    }
    
    private func processSampleBuffer(_ sampleBuffer: CMSampleBuffer, flags: VTEncodeInfoFlags) {
        let isKeyFrame = !CFDictionaryContainsKey(
            unsafeBitCast(CFArrayGetValueAtIndex(CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, createIfNecessary: true), 0), to: CFDictionary.self),
            unsafeBitCast(kCMSampleAttachmentKey_NotSync, to: UnsafeRawPointer.self)
        )
        
        guard let dataBuffer = CMSampleBufferGetDataBuffer(sampleBuffer) else { return }
        
        var lengthAtOffset: Int = 0
        var totalLength: Int = 0
        var dataPointer: UnsafeMutablePointer<Int8>?
        
        if CMBlockBufferGetDataPointer(dataBuffer, atOffset: 0, lengthAtOffsetOut: &lengthAtOffset, totalLengthOut: &totalLength, dataPointerOut: &dataPointer) == noErr,
           let dataPointer = dataPointer {
            
            var bufferOffset = 0
            let avcCHeaderLength = 4
            var payload = Data()
            
            // Extract SPS/PPS header for Key Frames
            if isKeyFrame, let formatDesc = CMSampleBufferGetFormatDescription(sampleBuffer) {
                var spsCount = 0
                var spsPointer: UnsafePointer<UInt8>?
                var spsLength = 0
                CMVideoFormatDescriptionGetH264ParameterSetAtIndex(formatDesc, parameterSetIndex: 0, parameterSetPointerOut: &spsPointer, parameterSetLengthOut: &spsLength, parameterSetCountOut: &spsCount, nalUnitHeaderLengthOut: nil)
                
                var ppsPointer: UnsafePointer<UInt8>?
                var ppsLength = 0
                CMVideoFormatDescriptionGetH264ParameterSetAtIndex(formatDesc, parameterSetIndex: 1, parameterSetPointerOut: &ppsPointer, parameterSetLengthOut: &ppsLength, parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil)
                
                let startCode = Data([0x00, 0x00, 0x00, 0x01])
                if let sps = spsPointer, spsLength > 0 {
                    payload.append(startCode)
                    payload.append(sps, count: spsLength)
                }
                if let pps = ppsPointer, ppsLength > 0 {
                    payload.append(startCode)
                    payload.append(pps, count: ppsLength)
                }
            }
            
            // Convert AVCC length prefixes into Annex-B 0x00000001 NAL headers
            while bufferOffset < totalLength - avcCHeaderLength {
                var naluLength: UInt32 = 0
                memcpy(&naluLength, dataPointer + bufferOffset, avcCHeaderLength)
                naluLength = CFSwapInt32BigToHost(naluLength)
                
                payload.append(contentsOf: [0x00, 0x00, 0x00, 0x01])
                let naluData = Data(bytes: dataPointer + bufferOffset + avcCHeaderLength, count: Int(naluLength))
                payload.append(naluData)
                
                bufferOffset += Int(avcCHeaderLength + Int(naluLength))
            }
            
            let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
            let timestampUs = UInt64(CMTimeGetSeconds(pts) * 1_000_000)
            
            onFrameEncoded?(isKeyFrame, timestampUs, payload)
        }
    }
    
    public func stop() {
        if let session = compressionSession {
            VTCompressionSessionInvalidate(session)
            compressionSession = nil
        }
    }
}
