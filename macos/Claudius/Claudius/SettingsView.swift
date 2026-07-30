import AppKit
import ServiceManagement
import SwiftUI

struct SettingsView: View {
    @Bindable var controller: CompanionController
    @State private var name: String = AppPreferences.name
    @State private var port: String = String(AppPreferences.port)
    @State private var secret: String = AppPreferences.secret
    @State private var claudeBinary: String = AppPreferences.claudeBinary
    @State private var openAtLogin: Bool = SMAppService.mainApp.status == .enabled
    @State private var savedFlash = false

    var body: some View {
        Form {
            Section("Companion") {
                TextField("mDNS name", text: $name)
                    .help("Defaults to this Mac’s hostname; must match the screen’s companion name")
                TextField("WebSocket port", text: $port)
                SecureField("Shared secret (optional)", text: $secret)
                TextField("Claude binary", text: $claudeBinary)
            }

            Section("Startup") {
                Toggle("Open at Login", isOn: $openAtLogin)
                    .onChange(of: openAtLogin) { _, enabled in
                        setLoginItem(enabled)
                    }
            }

            Section {
                Button("Save") {
                    save()
                }
                .keyboardShortcut(.defaultAction)
                if savedFlash {
                    Text("Saved")
                        .foregroundStyle(.secondary)
                }
            }
        }
        .formStyle(.grouped)
        .padding()
        .frame(width: 420, height: 380)
        .onAppear {
            name = AppPreferences.name
            port = String(AppPreferences.port)
            secret = AppPreferences.secret
            claudeBinary = AppPreferences.claudeBinary
            openAtLogin = SMAppService.mainApp.status == .enabled
        }
    }

    private func save() {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        AppPreferences.name = trimmed.isEmpty ? AppPreferences.defaultHostName() : trimmed
        name = AppPreferences.name
        if let p = Int(port), p > 0, p < 65536 {
            AppPreferences.port = p
        }
        AppPreferences.secret = secret
        AppPreferences.claudeBinary = claudeBinary.trimmingCharacters(in: .whitespacesAndNewlines)
        savedFlash = true
        switch controller.store.runState {
        case .running:
            controller.restartIfRunning()
        case .stopped, .error:
            controller.start()
        }
        Task {
            try? await Task.sleep(nanoseconds: 1_500_000_000)
            savedFlash = false
        }
    }

    private func setLoginItem(_ enabled: Bool) {
        do {
            if enabled {
                try SMAppService.mainApp.register()
            } else {
                try SMAppService.mainApp.unregister()
            }
        } catch {
            controller.store.lastError = "Login item: \(error.localizedDescription)"
            openAtLogin = SMAppService.mainApp.status == .enabled
        }
    }
}
