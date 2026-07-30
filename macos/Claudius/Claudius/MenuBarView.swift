import SwiftUI

struct MenuBarView: View {
    @Bindable var controller: CompanionController
    @Environment(\.openSettings) private var openSettings

    var body: some View {
        let store = controller.store

        Text(store.statusLine)

        if store.claudeMissing {
            Text("claude CLI not found on PATH")
        }

        if let err = store.lastError, !err.isEmpty {
            Text(err)
        }

        Divider()

        if store.sessions.isEmpty {
            Text("No Claude sessions")
        } else {
            ForEach(store.sessions, id: \.listID) { session in
                Text("\(session.name) — \(session.statusLabel)")
            }
        }

        Divider()

        Button("Preferences…") {
            openSettings()
        }
        .keyboardShortcut(",", modifiers: .command)

        Button("Quit Claudius") {
            controller.quit()
        }
        .keyboardShortcut("q", modifiers: .command)
    }
}
