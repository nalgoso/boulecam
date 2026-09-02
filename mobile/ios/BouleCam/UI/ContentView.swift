import SwiftUI

struct ContentView: View {
    @State private var isStreaming = false
    @State private var fps = 60
    @State private var resolution = "1080p60"
    
    private let camera = CameraManager()
    private let encoder = H264HardwareEncoder(width: 1920, height: 1080, fps: 60)
    private let sender = StreamSender(host: "127.0.0.1", port: 8088)
    
    var body: some View {
        ZStack {
            Color.black.edgesIgnoringSafeArea(.all)
            
            VStack {
                Spacer()
                
                VStack(alignment: .leading, spacing: 8) {
                    HStack {
                        Circle()
                            .fill(isStreaming ? Color.green : Color.orange)
                            .frame(width: 12, height: 12)
                        Text(isStreaming ? "STREAMING (Connected)" : "WAITING FOR PC")
                            .font(.headline)
                            .foregroundColor(.white)
                    }
                    
                    Text("Target: \(resolution) | Low-Latency H.264")
                        .font(.subheadline)
                        .foregroundColor(.gray)
                    
                    Text("Connect via Lightning / USB-C Cable or Wi-Fi")
                        .font(.caption)
                        .foregroundColor(.gray)
                }
                .padding()
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(Color(white: 0.15).opacity(0.85))
                .cornerRadius(16)
                .padding()
            }
        }
        .onAppear {
            setupPipeline()
        }
        .onDisappear {
            camera.stopSession()
            encoder.stop()
            sender.stop()
        }
    }
    
    private func setupPipeline() {
        sender.onConnectionStatusChanged = { connected in
            DispatchQueue.main.async {
                self.isStreaming = connected
            }
        }
        sender.start()
        
        encoder.onFrameEncoded = { isKeyFrame, timestampUs, naluData in
            sender.sendFrame(isKeyFrame: isKeyFrame, timestampUs: timestampUs, naluData: naluData)
        }
        
        camera.onSampleBuffer = { pixelBuffer, pts in
            encoder.encode(pixelBuffer: pixelBuffer, presentationTime: pts)
        }
        camera.startSession()
    }
}
