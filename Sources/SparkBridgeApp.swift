import SwiftUI
import CoreBluetooth
import CoreMIDI

// AIRSTEP Bridge — turns an AIRSTEP Spk Edition into a macOS MIDI controller.
//
// The Mac pretends to be a Spark 40 amp (BLE service FFC0). The pedal, in its
// normal Mode 1, connects to us and reports footswitch presses; each one is
// decoded and sent out on a virtual CoreMIDI source called "AIRSTEP Bridge".
// No real amp, no HID pairing, no Looper Mode needed.
//
// Mode 1 mapping (verified against labelled captures):
//   A..D -> hardware preset 0..3   (01 38)
//   E    -> drive on/off toggle    (01 15, "Booster")

let SPARK_SERVICE = CBUUID(string: "FFC0")
let SPARK_WRITE = CBUUID(string: "FFC1")
let SPARK_NOTIFY = CBUUID(string: "FFC2")

// MARK: - Configuration

enum MessageKind: String, Codable, CaseIterable, Identifiable {
    case note = "Note"
    case cc = "CC"
    case program = "Program"
    var id: String { rawValue }
}

struct SwitchConfig: Codable, Equatable {
    var enabled = true
    var kind: MessageKind = .note
    var number: Int = 60
    var channel: Int = 1          // 1...16
    /// CC only: send 127/0 following the pedal's toggle state instead of 127 each time.
    var followToggle = false
}

final class Settings: ObservableObject {
    @Published var configs: [String: SwitchConfig] {
        didSet { save() }
    }
    static let switches = ["A", "B", "C", "D", "E"]
    private let key = "airstep.bridge.configs"

    init() {
        if let data = UserDefaults.standard.data(forKey: key),
           let decoded = try? JSONDecoder().decode([String: SwitchConfig].self, from: data) {
            configs = decoded
        } else {
            var d: [String: SwitchConfig] = [:]
            for (i, s) in Settings.switches.enumerated() {
                d[s] = SwitchConfig(enabled: true, kind: .note, number: 60 + i, channel: 1,
                                    followToggle: s == "E")
            }
            configs = d
        }
    }

    private func save() {
        if let data = try? JSONEncoder().encode(configs) {
            UserDefaults.standard.set(data, forKey: key)
        }
    }

    func config(_ s: String) -> SwitchConfig { configs[s] ?? SwitchConfig() }
    func binding(_ s: String) -> Binding<SwitchConfig> {
        Binding(get: { self.config(s) }, set: { self.configs[s] = $0 })
    }
}

// MARK: - Activity

struct Activity: Identifiable {
    let id = UUID()
    let time: Date
    let sw: String
    let detail: String
    let midi: String
}

// MARK: - Bridge

@MainActor
final class Bridge: NSObject, ObservableObject {
    @Published var connected = false
    @Published var bleState = "starting"
    @Published var midiReady = false
    @Published var flash: String? = nil            // switch currently lit
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

    init(settings: Settings) {
        self.settings = settings
        super.init()
    }

    func boot() {
        setupMIDI()
        manager = CBPeripheralManager(delegate: self, queue: nil)
    }

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

    /// Emit whatever the given switch is configured to send. Returns a description.
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
            // One press can produce several 01 15 frames; collapse them.
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
}

extension Bridge: CBPeripheralManagerDelegate {
    nonisolated func peripheralManagerDidUpdateState(_ m: CBPeripheralManager) {
        Task { @MainActor in
            switch m.state {
            case .poweredOn:
                bleState = "bereit"
                lastError = nil
                let w = CBMutableCharacteristic(type: SPARK_WRITE,
                                                properties: [.write, .writeWithoutResponse],
                                                value: nil, permissions: [.writeable])
                let n = CBMutableCharacteristic(type: SPARK_NOTIFY,
                                                properties: [.notify, .read],
                                                value: nil, permissions: [.readable])
                let svc = CBMutableService(type: SPARK_SERVICE, primary: true)
                svc.characteristics = [w, n]
                m.add(svc)
            case .poweredOff:
                bleState = "Bluetooth ist aus"
                connected = false
                lastError = "Bluetooth am Mac einschalten"
            case .unauthorized:
                bleState = "keine Berechtigung"
                lastError = "Bluetooth-Zugriff für die App erlauben"
            default:
                bleState = "…"
            }
        }
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

// MARK: - UI

struct PadView: View {
    let label: String
    let lit: Bool
    let subtitle: String
    let dimmed: Bool

    var body: some View {
        VStack(spacing: 6) {
            Text(label)
                .font(.system(size: 40, weight: .heavy, design: .rounded))
            Text(subtitle)
                .font(.system(size: 10, design: .monospaced))
                .foregroundStyle(lit ? Color.white.opacity(0.9) : .secondary)
                .lineLimit(1)
        }
        .frame(width: 108, height: 104)
        .background(
            RoundedRectangle(cornerRadius: 14)
                .fill(lit ? Color.accentColor : Color.gray.opacity(dimmed ? 0.10 : 0.18))
        )
        .foregroundStyle(lit ? Color.white : (dimmed ? Color.secondary : Color.primary))
        .overlay(
            RoundedRectangle(cornerRadius: 14)
                .stroke(lit ? Color.accentColor : Color.clear, lineWidth: 3)
                .shadow(color: lit ? Color.accentColor.opacity(0.7) : .clear, radius: 10)
        )
        .animation(.easeOut(duration: 0.08), value: lit)
    }
}

struct ContentView: View {
    @ObservedObject var bridge: Bridge
    @ObservedObject var settings: Settings
    @State private var showSettings = false

    var body: some View {
        VStack(spacing: 0) {
            status
            Divider()
            display
            Divider()
            if showSettings { settingsPane } else { history }
        }
        .frame(minWidth: 640, minHeight: showSettings ? 620 : 520)
    }

    private var status: some View {
        HStack(spacing: 12) {
            Circle()
                .fill(bridge.connected ? .green : .orange)
                .frame(width: 10, height: 10)
            Text(bridge.connected ? "Pedal verbunden" : "warte auf Pedal — AIRSTEP in Mode 1 einschalten")
                .font(.callout)
            Spacer()
            if let e = bridge.lastError {
                Text(e).font(.caption).foregroundStyle(Color.red)
            }
            Image(systemName: bridge.midiReady ? "pianokeys" : "exclamationmark.triangle")
                .foregroundStyle(bridge.midiReady ? Color.green : Color.red)
            Text("AIRSTEP Bridge").font(.caption).foregroundStyle(.secondary)
            Toggle("Einstellungen", isOn: $showSettings)
                .toggleStyle(.button)
                .font(.caption)
        }
        .padding(10)
    }

    private var display: some View {
        VStack(spacing: 14) {
            HStack(spacing: 12) {
                ForEach(Settings.switches, id: \.self) { s in
                    let c = settings.config(s)
                    PadView(label: s,
                            lit: bridge.flash == s,
                            subtitle: c.enabled ? "\(c.kind.rawValue) \(c.number)" : "aus",
                            dimmed: !c.enabled)
                }
            }
            HStack(spacing: 8) {
                Text("Drive")
                    .font(.caption).foregroundStyle(.secondary)
                Circle()
                    .fill(bridge.driveOn ? Color.green : Color.gray.opacity(0.3))
                    .frame(width: 9, height: 9)
                Text(bridge.driveOn ? "an" : "aus")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 22)
        .frame(maxWidth: .infinity)
    }

    private var history: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text("Letzte Ereignisse").font(.caption).foregroundStyle(.secondary)
                .padding(.horizontal, 12).padding(.top, 8)
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 3) {
                    ForEach(bridge.recent) { a in
                        HStack(spacing: 10) {
                            Text(time(a.time))
                                .font(.system(.caption, design: .monospaced))
                                .foregroundStyle(.secondary).frame(width: 76, alignment: .leading)
                            Text(a.sw)
                                .font(.system(.body, design: .rounded)).bold()
                                .frame(width: 20)
                            Text(a.detail)
                                .font(.system(.caption, design: .monospaced))
                                .frame(width: 150, alignment: .leading)
                            Text("→ \(a.midi)")
                                .font(.system(.caption, design: .monospaced))
                                .foregroundStyle(.secondary)
                            Spacer()
                        }
                        .padding(.horizontal, 12)
                    }
                }
                .padding(.vertical, 6)
            }
        }
    }

    private var settingsPane: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 10) {
                Text("MIDI pro Fußschalter")
                    .font(.headline).padding(.bottom, 2)
                ForEach(Settings.switches, id: \.self) { s in
                    let b = settings.binding(s)
                    HStack(spacing: 10) {
                        Toggle("", isOn: b.enabled).labelsHidden()
                        Text(s).font(.system(.title3, design: .rounded)).bold().frame(width: 18)
                        Picker("", selection: b.kind) {
                            ForEach(MessageKind.allCases) { Text($0.rawValue).tag($0) }
                        }
                        .labelsHidden().frame(width: 100)
                        Stepper(value: b.number, in: 0...127) {
                            Text("Nr. \(settings.config(s).number)")
                                .font(.system(.caption, design: .monospaced)).frame(width: 66, alignment: .leading)
                        }
                        Stepper(value: b.channel, in: 1...16) {
                            Text("Ch \(settings.config(s).channel)")
                                .font(.system(.caption, design: .monospaced)).frame(width: 52, alignment: .leading)
                        }
                        if settings.config(s).kind == .cc {
                            Toggle("An/Aus-Zustand", isOn: b.followToggle).font(.caption)
                        }
                        Spacer()
                    }
                    .disabled(!settings.config(s).enabled && false)
                }
                Divider().padding(.vertical, 4)
                Text("A–D senden den jeweiligen Preset-Wechsel, E den Drive-Toggle. Bei CC schickt „An/Aus-Zustand\" 127 bzw. 0 passend zum Pedal, sonst immer 127.")
                    .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
            .padding(14)
        }
    }

    private func time(_ d: Date) -> String {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss.SS"
        return f.string(from: d)
    }
}

@main
struct SparkBridgeApp: App {
    @StateObject private var settings = Settings()
    @StateObject private var bridge: Bridge

    init() {
        let s = Settings()
        _settings = StateObject(wrappedValue: s)
        _bridge = StateObject(wrappedValue: Bridge(settings: s))
    }

    var body: some Scene {
        WindowGroup("AIRSTEP Bridge") {
            ContentView(bridge: bridge, settings: bridge.settings)
                .onAppear { bridge.boot() }
        }
    }
}
