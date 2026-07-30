import AppKit
import SwiftUI

@main
struct ClaudiusApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate

    var body: some Scene {
        MenuBarExtra {
            MenuBarView(controller: appDelegate.controller)
        } label: {
            MenuBarLabel(store: appDelegate.controller.store)
        }
        .menuBarExtraStyle(.menu)

        Settings {
            SettingsView(controller: appDelegate.controller)
        }
    }
}

private struct MenuBarLabel: View {
    @Bindable var store: StatusStore

    var body: some View {
        Label("Claudius", systemImage: store.menuBarSymbol)
    }
}

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    let controller = CompanionController()

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)
        controller.start()
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        false
    }

    func applicationWillTerminate(_ notification: Notification) {
        // Ensure WS close frames flush even for Cmd+Q / Dock Quit.
        controller.shutdown()
    }
}
