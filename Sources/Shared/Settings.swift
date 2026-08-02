import SwiftUI

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
    static let switches = ["A", "B", "C", "D", "E"]

    @Published var configs: [String: SwitchConfig] { didSet { save() } }

    /// iOS only: play silence so the system keeps the app running when it is not
    /// in the foreground. Off by default — the bluetooth-peripheral background
    /// mode alone is usually enough.
    @Published var keepAwake: Bool {
        didSet { UserDefaults.standard.set(keepAwake, forKey: Self.keepAwakeKey) }
    }

    private static let key = "airstep.bridge.configs"
    private static let keepAwakeKey = "airstep.bridge.keepAwake"

    init() {
        keepAwake = UserDefaults.standard.bool(forKey: Self.keepAwakeKey)
        if let data = UserDefaults.standard.data(forKey: Self.key),
           let decoded = try? JSONDecoder().decode([String: SwitchConfig].self, from: data) {
            configs = decoded
        } else {
            var d: [String: SwitchConfig] = [:]
            for (i, s) in Settings.switches.enumerated() {
                d[s] = SwitchConfig(enabled: true, kind: .note, number: 60 + i, channel: 1)
            }
            configs = d
        }
    }

    private func save() {
        if let data = try? JSONEncoder().encode(configs) {
            UserDefaults.standard.set(data, forKey: Self.key)
        }
    }

    func config(_ s: String) -> SwitchConfig { configs[s] ?? SwitchConfig() }

    func binding(_ s: String) -> Binding<SwitchConfig> {
        Binding(get: { self.config(s) }, set: { self.configs[s] = $0 })
    }

    func resetToDefaults() {
        var d: [String: SwitchConfig] = [:]
        for (i, s) in Settings.switches.enumerated() {
            d[s] = SwitchConfig(enabled: true, kind: .note, number: 60 + i, channel: 1)
        }
        configs = d
    }
}
