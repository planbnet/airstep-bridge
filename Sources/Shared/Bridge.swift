import Foundation
import CoreBluetooth
import CoreMIDI
#if os(iOS)
import AVFoundation
import UIKit
#endif

// The Mac/iPad impersonates a Spark 40 amp so an AIRSTEP Spk Edition in Mode 1
// connects to it and reports footswitch presses. Each press is turned into a
// configurable MIDI message on a virtual CoreMIDI source named "AIRSTEP Bridge".

let SPARK_SERVICE = CBUUID(string: "FFC0")
let SPARK_WRITE = CBUUID(string: "FFC1")
let SPARK_NOTIFY = CBUUID(string: "FFC2")

struct Activity: Identifiable {
    let id = UUID()
    let time: Date
    let sw: String
    let detail: String
    let midi: String
}

@MainActor
final class Bridge: NSObject, ObservableObject {
    @Published var connected = false
    @Published var bleState = "startet"
    @Published var midiReady = false
    @Published var flash: String? = nil
    @Published var driveOn = false
    @Published var recent: [Activity] = []
    @Published var lastError: String? = nil

    let settings: Settings

    private var manager: CBPeripheralManager!
    private var midiClient = MIDIClientRef()
    private var midiSource = MIDIEndpointRef()
    private var lastEffectAt = Date.distantPast
    private let burstWindow: TimeInterval = 0.3
    private var flashTask: Task<Void, Never>? = nil

    #if os(iOS)
    private var engine: AVAudioEngine? = nil
    private var silence: AVAudioPlayerNode? = nil
    #endif

    init(settings: Settings) {
        self.settings = settings
        super.init()
    }

    func boot() {
        setupMIDI()
        // Restoration identifier lets iOS relaunch us for peripheral events.
        manager = CBPeripheralManager(
            delegate: self, queue: nil,
            options: [CBPeripheralManagerOptionRestoreIdentifierKey: "airstep.bridge.peripheral"])
        applyKeepAwake()
    }

    // MARK: - Background keep-alive (iOS)

    func applyKeepAwake() {
        #if os(iOS)
        UIApplication.shared.isIdleTimerDisabled = settings.keepAwake
        if settings.keepAwake { startSilentAudio() } else { stopSilentAudio() }
        #endif
    }

    #if os(iOS)
    private func startSilentAudio() {
        guard engine == nil else { return }
        do {
            let session = AVAudioSession.sharedInstance()
            try session.setCategory(.playback, mode: .default, options: [.mixWithOthers])
            try session.setActive(true)
            let e = AVAudioEngine()
            let node = AVAudioPlayerNode()
            e.attach(node)
            let fmt = e.outputNode.inputFormat(forBus: 0)
            e.connect(node, to: e.mainMixerNode, format: fmt)
            guard let buf = AVAudioPCMBuffer(pcmFormat: fmt, frameCapacity: 4096) else { return }
            buf.frameLength = 4096   // all zeroes: silence
            try e.start()
            node.scheduleBuffer(buf, at: nil, options: .loops)
            node.volume = 0
            node.play()
            engine = e
            silence = node
        } catch {
            lastError = "Hintergrund-Audio: \(error.localizedDescription)"
        }
    }

    private func stopSilentAudio() {
        silence?.stop()
        engine?.stop()
        silence = nil
        engine = nil
        try? AVAudioSession.sharedInstance().setActive(false)
    }
    #endif

    // MARK: - MIDI

    private func setupMIDI() {
        guard MIDIClientCreate("AIRSTEP Bridge" as CFString, nil, nil, &midiClient) == noErr,
              MIDISourceCreate(midiClient, "AIRSTEP Bridge" as CFString, &midiSource) == noErr
        else { lastError = "Virtuelle MIDI-Quelle konnte nicht erstellt werden"; return }
        MIDIObjectSetIntegerProperty(midiSource, kMIDIPropertyUniqueID, 0x41535442)
        midiReady = true
    }

    private func raw(_ bytes: [UInt8]) {
        guard midiSource != 0 else { return }
        var p = MIDIPacket()
        p.timeStamp = 0
        p.length = UInt16(bytes.count)
        withUnsafeMutableBytes(of: &p.data) { buf in
            for (i, b) in bytes.enumerated() { buf[i] = b }
        }
        var list = MIDIPacketList(numPackets: 1, packet: p)
        MIDIReceived(midiSource, &list)
    }

    private func emit(_ sw: String, toggleOn: Bool?) -> String {
        let c = settings.config(sw)
        guard c.enabled else { return "aus" }
        let ch = UInt8(max(1, min(16, c.channel)) - 1)
        let n = UInt8(max(0, min(127, c.number)))
        switch c.kind {
        case .note:
            raw([0x90 | ch, n, 0x7F])
            raw([0x80 | ch, n, 0x00])
            return "Note \(n) · Ch \(ch + 1)"
        case .cc:
            let v: UInt8 = c.followToggle ? ((toggleOn ?? true) ? 127 : 0) : 127
            raw([0xB0 | ch, n, v])
            return "CC \(n) = \(v) · Ch \(ch + 1)"
        case .program:
            raw([0xC0 | ch, n])
            return "Program \(n) · Ch \(ch + 1)"
        }
    }

    private func fire(_ sw: String, detail: String, toggleOn: Bool? = nil) {
        let midi = emit(sw, toggleOn: toggleOn)
        recent.insert(Activity(time: Date(), sw: sw, detail: detail, midi: midi), at: 0)
        if recent.count > 12 { recent.removeLast(recent.count - 12) }
        flash = sw
        flashTask?.cancel()
        flashTask = Task { @MainActor in
            try? await Task.sleep(nanoseconds: 220_000_000)
            if !Task.isCancelled { flash = nil }
        }
    }

    fileprivate func handle(_ bytes: [UInt8]) {
        guard let frame = parseSparkFrame(bytes) else { return }
        switch classify(frame) {
        case .preset(let idx):
            guard idx >= 0, idx < 4 else { return }
            fire(Settings.switches[idx], detail: "Preset \(idx)")
        case .effect(let name, let on):
            let now = Date()
            let continuation = now.timeIntervalSince(lastEffectAt) <= burstWindow
            lastEffectAt = now
            guard !continuation else { return }
            if let on { driveOn = on }
            fire("E", detail: "\(name) \(on.map { $0 ? "an" : "aus" } ?? "toggle")",
                 toggleOn: on ?? driveOn)
        case .query, .unknown:
            return
        }
    }

    private func publishService(_ m: CBPeripheralManager) {
        let w = CBMutableCharacteristic(type: SPARK_WRITE,
                                        properties: [.write, .writeWithoutResponse],
                                        value: nil, permissions: [.writeable])
        let n = CBMutableCharacteristic(type: SPARK_NOTIFY,
                                        properties: [.notify, .read],
                                        value: nil, permissions: [.readable])
        let svc = CBMutableService(type: SPARK_SERVICE, primary: true)
        svc.characteristics = [w, n]
        m.add(svc)
    }
}

extension Bridge: CBPeripheralManagerDelegate {
    nonisolated func peripheralManagerDidUpdateState(_ m: CBPeripheralManager) {
        Task { @MainActor in
            switch m.state {
            case .poweredOn:
                bleState = "bereit"
                lastError = nil
                publishService(m)
            case .poweredOff:
                bleState = "Bluetooth ist aus"
                connected = false
                lastError = "Bluetooth einschalten"
            case .unauthorized:
                bleState = "keine Berechtigung"
                lastError = "Bluetooth-Zugriff für die App erlauben"
            case .unsupported:
                bleState = "nicht unterstützt"
                lastError = "Dieses Gerät kann keine BLE-Peripherie bereitstellen"
            default:
                bleState = "…"
            }
        }
    }

    nonisolated func peripheralManager(_ m: CBPeripheralManager,
                                       willRestoreState dict: [String: Any]) {
        // Woken by the system for peripheral events; services are restored for us.
    }

    nonisolated func peripheralManager(_ m: CBPeripheralManager, didAdd s: CBService, error: Error?) {
        Task { @MainActor in
            if let error { lastError = "Service: \(error.localizedDescription)"; return }
            m.startAdvertising([
                CBAdvertisementDataLocalNameKey: "Spark 40 BLE",
                CBAdvertisementDataServiceUUIDsKey: [SPARK_SERVICE],
            ])
        }
    }

    nonisolated func peripheralManagerDidStartAdvertising(_ m: CBPeripheralManager, error: Error?) {
        Task { @MainActor in
            if let error { lastError = "Advertising: \(error.localizedDescription)" }
        }
    }

    nonisolated func peripheralManager(_ m: CBPeripheralManager, central: CBCentral,
                                       didSubscribeTo c: CBCharacteristic) {
        Task { @MainActor in connected = true }
    }

    nonisolated func peripheralManager(_ m: CBPeripheralManager, central: CBCentral,
                                       didUnsubscribeFrom c: CBCharacteristic) {
        Task { @MainActor in connected = false }
    }

    nonisolated func peripheralManager(_ m: CBPeripheralManager, didReceiveRead r: CBATTRequest) {
        r.value = Data()
        m.respond(to: r, withResult: .success)
    }

    nonisolated func peripheralManager(_ m: CBPeripheralManager, didReceiveWrite rs: [CBATTRequest]) {
        let payloads = rs.map { [UInt8]($0.value ?? Data()) }
        if let first = rs.first { m.respond(to: first, withResult: .success) }
        Task { @MainActor in for p in payloads { handle(p) } }
    }
}
