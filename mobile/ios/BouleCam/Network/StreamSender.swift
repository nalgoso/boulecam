import Foundation
import Network

public class StreamSender {
    private var connection: NWConnection?
    private let host: NWEndpoint.Host
    private let port: NWEndpoint.Port
    private let queue = DispatchQueue(label: "com.boulecam.networkQueue", qos: .userInteractive)
    
    private var sequenceNumber: UInt32 = 0
    public var onConnectionStatusChanged: ((Bool) -> Void)?
    
    public init(host: String = "127.0.0.1", port: UInt16 = 8088) {
        self.host = NWEndpoint.Host(host)
        self.port = NWEndpoint.Port(rawValue: port) ?? 8088
    }
    
    public func start() {
        let tcpOptions = NWProtocolTCP.Options()
        tcpOptions.noDelay = true
        tcpOptions.enableFastOpen = true
        
        let params = NWParameters(tls: nil, tcp: tcpOptions)
        params.prohibitExpensivePaths = false
        
        let conn = NWConnection(host: host, port: port, using: params)
        self.connection = conn
        
        conn.stateUpdateHandler = { [weak self] state in
            switch state {
            case .ready:
                self?.sendHandshake()
                self?.onConnectionStatusChanged?(true)
            case .failed, .cancelled:
                self?.onConnectionStatusChanged?(false)
                DispatchQueue.global().asyncAfter(deadline: .now() + 2.0) {
                    self?.start()
                }
            default:
                break
            }
        }
        
        conn.start(queue: queue)
    }
    
    private func sendHandshake() {
        var packet = Data()
        var magic: UInt32 = 0x4243414D.littleEndian
        var type: UInt8 = 0x01 // BOULECAM_PKT_HANDSHAKE_REQ
        var vMaj: UInt8 = 1
        var vMin: UInt8 = 0
        var codec: UInt8 = 1 // H.264
        var width: UInt32 = UInt32(1920).littleEndian
        var height: UInt32 = UInt32(1080).littleEndian
        var fps: UInt32 = UInt32(60).littleEndian
        var bitrate: UInt32 = UInt32(8000000).littleEndian
        
        packet.append(Data(bytes: &magic, count: 4))
        packet.append(Data(bytes: &type, count: 1))
        packet.append(Data(bytes: &vMaj, count: 1))
        packet.append(Data(bytes: &vMin, count: 1))
        packet.append(Data(bytes: &codec, count: 1))
        packet.append(Data(bytes: &width, count: 4))
        packet.append(Data(bytes: &height, count: 4))
        packet.append(Data(bytes: &fps, count: 4))
        packet.append(Data(bytes: &bitrate, count: 4))
        
        var devNameBytes = [UInt8](repeating: 0, count: 64)
        let nameStr = "iPhone (iOS)"
        let utf8 = Array(nameStr.utf8)
        for i in 0..<min(utf8.count, 63) { devNameBytes[i] = utf8[i] }
        packet.append(Data(devNameBytes))
        
        connection?.send(content: packet, completion: .idempotent)
    }
    
    public func sendFrame(isKeyFrame: Bool, timestampUs: UInt64, naluData: Data) {
        var header = Data()
        var magic: UInt32 = 0x4243414D.littleEndian
        var type: UInt8 = 0x10 // BOULECAM_PKT_FRAME_DATA
        var key: UInt8 = isKeyFrame ? 1 : 0
        var rot: UInt16 = 0
        var seq: UInt32 = sequenceNumber.littleEndian
        sequenceNumber &+= 1
        var ts: UInt64 = timestampUs.littleEndian
        var size: UInt32 = UInt32(naluData.count).littleEndian
        
        header.append(Data(bytes: &magic, count: 4))
        header.append(Data(bytes: &type, count: 1))
        header.append(Data(bytes: &key, count: 1))
        header.append(Data(bytes: &rot, count: 2))
        header.append(Data(bytes: &seq, count: 4))
        header.append(Data(bytes: &ts, count: 8))
        header.append(Data(bytes: &size, count: 4))
        
        var fullPacket = Data()
        fullPacket.append(header)
        fullPacket.append(naluData)
        
        connection?.send(content: fullPacket, completion: .idempotent)
    }
    
    public func stop() {
        connection?.cancel()
        connection = nil
    }
}
