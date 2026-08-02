import SwiftUI

@main
struct AirstepBridgeApp: App {
    @StateObject private var bridge: Bridge

    init() {
        _bridge = StateObject(wrappedValue: Bridge(settings: Settings()))
    }

    var body: some Scene {
        WindowGroup("AIRSTEP Bridge") {
            ContentView(bridge: bridge, settings: bridge.settings)
                .onAppear { bridge.boot() }
        }
    }
}
