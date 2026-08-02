import SwiftUI

struct PadView: View {
    let label: String
    let lit: Bool
    let subtitle: String
    let dimmed: Bool
    var compact = false

    private var side: CGFloat { compact ? 62 : 108 }

    var body: some View {
        VStack(spacing: 4) {
            Text(label)
                .font(.system(size: compact ? 26 : 40, weight: .heavy, design: .rounded))
            Text(subtitle)
                .font(.system(size: compact ? 8 : 10, design: .monospaced))
                .foregroundStyle(lit ? Color.white.opacity(0.9) : .secondary)
                .lineLimit(1)
                .minimumScaleFactor(0.7)
        }
        .frame(width: side, height: compact ? 66 : 104)
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
        content
        #if os(macOS)
            .frame(minWidth: 640, minHeight: 520)
        #endif
    }

    @ViewBuilder
    private var content: some View {
        #if os(iOS)
        NavigationStack {
            VStack(spacing: 0) {
                status
                Divider()
                display
                Divider()
                history
            }
            .navigationTitle("AIRSTEP Bridge")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button { showSettings = true } label: { Image(systemName: "slider.horizontal.3") }
                }
            }
            .sheet(isPresented: $showSettings) {
                NavigationStack {
                    settingsForm
                        .navigationTitle("Einstellungen")
                        .toolbar {
                            ToolbarItem(placement: .confirmationAction) {
                                Button("Fertig") { showSettings = false }
                            }
                        }
                }
            }
        }
        #else
        VStack(spacing: 0) {
            status
            Divider()
            display
            Divider()
            if showSettings { settingsForm } else { history }
        }
        #endif
    }

    private var status: some View {
        HStack(spacing: 10) {
            Circle()
                .fill(bridge.connected ? .green : .orange)
                .frame(width: 10, height: 10)
            Text(bridge.connected ? "Pedal verbunden" : "warte auf Pedal — AIRSTEP in Mode 1 einschalten")
                .font(.callout)
                .lineLimit(2)
            Spacer()
            if let e = bridge.lastError {
                Text(e).font(.caption).foregroundStyle(Color.red).lineLimit(2)
            }
            Image(systemName: bridge.midiReady ? "pianokeys" : "exclamationmark.triangle")
                .foregroundStyle(bridge.midiReady ? Color.green : Color.red)
            #if os(macOS)
            Toggle("Einstellungen", isOn: $showSettings)
                .toggleStyle(.button).font(.caption)
            #endif
        }
        .padding(10)
    }

    private var display: some View {
        VStack(spacing: 12) {
            #if os(iOS)
            let compact = true
            #else
            let compact = false
            #endif
            HStack(spacing: compact ? 8 : 12) {
                ForEach(Settings.switches, id: \.self) { s in
                    let c = settings.config(s)
                    PadView(label: s,
                            lit: bridge.flash == s,
                            subtitle: subtitle(for: s, c),
                            dimmed: !c.enabled,
                            compact: compact)
                }
            }
            HStack(spacing: 8) {
                Text("Schalter E").font(.caption).foregroundStyle(.secondary)
                Circle()
                    .fill(bridge.driveOn ? Color.green : Color.gray.opacity(0.3))
                    .frame(width: 9, height: 9)
                Text(bridge.driveOn ? "an" : "aus").font(.caption).foregroundStyle(.secondary)
                Text("— entspricht der LED am Pedal")
                    .font(.caption2).foregroundStyle(.tertiary)
            }
        }
        .padding(.vertical, 18)
        .frame(maxWidth: .infinity)
    }

    private var history: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text("Letzte Ereignisse")
                .font(.caption).foregroundStyle(.secondary)
                .padding(.horizontal, 12).padding(.top, 8)
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 3) {
                    ForEach(bridge.recent) { a in
                        HStack(spacing: 8) {
                            Text(time(a.time))
                                .font(.system(.caption2, design: .monospaced))
                                .foregroundStyle(.secondary).frame(width: 72, alignment: .leading)
                            Text(a.sw)
                                .font(.system(.body, design: .rounded)).bold().frame(width: 18)
                            Text(a.detail)
                                .font(.system(.caption2, design: .monospaced))
                                .frame(width: 140, alignment: .leading)
                            Text("→ \(a.midi)")
                                .font(.system(.caption2, design: .monospaced))
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

    private var settingsForm: some View {
        Form {
            ForEach(Settings.switches, id: \.self) { s in
                Section("Fußschalter \(s)") {
                    let b = settings.binding(s)
                    Toggle("Aktiv", isOn: b.enabled)
                    Picker("Typ", selection: b.kind) {
                        ForEach(MessageKind.allCases) { Text($0.rawValue).tag($0) }
                    }
                    Stepper(settings.config(s).splitToggle
                            ? "Nummer „an\" \(settings.config(s).number)"
                            : "Nummer \(settings.config(s).number)",
                            value: b.number, in: 0...127)
                    Stepper("Kanal \(settings.config(s).channel)", value: b.channel, in: 1...16)
                    if settings.config(s).kind == .cc && !settings.config(s).splitToggle {
                        Toggle("127/0 nach An-Aus-Zustand", isOn: b.followToggle)
                    }
                    if s == "E" {
                        Toggle("Getrennte Nachrichten für an/aus", isOn: b.splitToggle)
                        if settings.config(s).splitToggle {
                            Stepper("Nummer „aus\" \(settings.config(s).offNumber)",
                                    value: b.offNumber, in: 0...127)
                            Text("E hält am Pedal einen eigenen An-Aus-Zustand und zeigt ihn auf der LED. Getrennt gesendet kannst du z. B. Play und Pause binden — die LED wird dann zur Statusanzeige.")
                                .font(.caption).foregroundStyle(.secondary)
                        }
                    }
                }
            }
            #if os(iOS)
            Section("Hintergrund") {
                Toggle("Aktiv halten (Stille abspielen)", isOn: Binding(
                    get: { settings.keepAwake },
                    set: { settings.keepAwake = $0; bridge.applyKeepAwake() }))
                Text("Hält die App als Audio-App am Leben und verhindert das Sperren des Bildschirms. Nur nötig, wenn iOS die Verbindung im Hintergrund trennt.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            #endif
            Section {
                Button("Auf Standard zurücksetzen") { settings.resetToDefaults() }
                Text("A–D senden den Preset-Wechsel, E den Drive-Toggle des Pedals.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        #if os(macOS)
        .formStyle(.grouped)
        #endif
    }

    private func subtitle(for s: String, _ c: SwitchConfig) -> String {
        guard c.enabled else { return "aus" }
        if c.splitToggle { return "\(c.kind.rawValue) \(c.number)/\(c.offNumber)" }
        return "\(c.kind.rawValue) \(c.number)"
    }

    private func time(_ d: Date) -> String {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss.SS"
        return f.string(from: d)
    }
}
