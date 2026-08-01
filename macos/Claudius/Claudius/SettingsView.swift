import AppKit
import ServiceManagement
import SwiftUI

struct SettingsView: View {
    @Bindable var controller: CompanionController
    @State private var name: String = AppPreferences.name
    @State private var port: String = String(AppPreferences.port)
    @State private var secret: String = ""
    @State private var claudeBinary: String = AppPreferences.claudeBinary
    @State private var openAtLogin: Bool = SMAppService.mainApp.status == .enabled
    @State private var savedFlash = false
    @State private var saveError: String?
    /// True when the Keychain read failed — the blank field is unknown, not empty.
    @State private var secretLoadFailed = false

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
                if let saveError {
                    Text(saveError)
                        .foregroundStyle(.red)
                } else if savedFlash {
                    Text("Saved")
                        .foregroundStyle(.secondary)
                }
            }
        }
        .formStyle(.grouped)
        .padding()
        .frame(width: 420, height: 380)
        .onAppear { reload() }
    }

    private func reload() {
        name = AppPreferences.name
        port = String(AppPreferences.port)
        claudeBinary = AppPreferences.claudeBinary
        openAtLogin = SMAppService.mainApp.status == .enabled
        do {
            secret = try AppPreferences.loadSecret()
            secretLoadFailed = false
            saveError = nil
        } catch {
            // Don't show an empty field as if no secret were configured.
            secret = ""
            secretLoadFailed = true
            saveError = error.localizedDescription
        }
    }

    private func save() {
        guard let p = Int(port), (1...65535).contains(p) else {
            savedFlash = false
            saveError = "Port must be between 1 and 65535"
            return
        }
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        AppPreferences.name = trimmed.isEmpty ? AppPreferences.defaultHostName() : trimmed
        name = AppPreferences.name
        AppPreferences.port = p
        AppPreferences.claudeBinary = claudeBinary.trimmingCharacters(in: .whitespacesAndNewlines)

        if secretLoadFailed, secret.isEmpty {
            // The field is blank because the read failed, not because there is no
            // secret — writing it back would silently disable authentication.
            savedFlash = false
            saveError = "Settings saved; shared secret left unchanged (Keychain unreadable)"
        } else {
            do {
                try AppPreferences.setSecret(secret)
            } catch {
                // Never claim "Saved" for a write that didn't land.
                savedFlash = false
                saveError = error.localizedDescription
                return
            }
            secretLoadFailed = false
            saveError = nil
            savedFlash = true
        }
        // start() stops any running instance first, so this covers both
        // "apply changes" and "recover from an error state".
        Task { await controller.start() }
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
