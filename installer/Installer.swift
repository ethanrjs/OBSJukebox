import SwiftUI
import AppKit

@main struct SeparateSongInstaller: App {
    var body: some Scene { WindowGroup("Separate Song Setup") { SetupView() }.windowResizability(.contentSize) }
}
struct SetupView: View {
    @State private var gd = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/Steam/steamapps/common/Geometry Dash/Geometry Dash.app").path
    @State private var obs = "/Applications/OBS.app"
    @State private var output = ""
    @State private var busy = false
    private var root: URL { Bundle.main.bundleURL.deletingLastPathComponent() }
    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            Text("Separate Song").font(.largeTitle.bold())
            Text("One song for you. Another for your recording.").foregroundStyle(.secondary)
            pathRow("Geometry Dash", path: $gd)
            pathRow("OBS Studio", path: $obs)
            GroupBox("What this installs") {
                VStack(alignment: .leading, spacing: 8) {
                    Text("• Separate Song mod + official Jukebox 3.8.0 in GD’s Geode mods folder")
                    Text("• Official Geode 5.10.1, only if Geode is missing; keeps GD’s original audio library")
                    Text("• Native OBS plugin in your user Library, plus an optional demo song in Music")
                    Text("Audio goes only to OBS on this computer (127.0.0.1:39022). No virtual driver, administrator password, account, or startup service.").foregroundStyle(.secondary)
                    Text("Existing files are backed up. Payload hashes and all destination paths are written to an install log. Full source is included.").foregroundStyle(.secondary)
                }.frame(maxWidth: .infinity, alignment: .leading).padding(8)
            }
            HStack {
                Button("View source") { NSWorkspace.shared.open(root.appendingPathComponent("Source")) }
                Spacer()
                Button(busy ? "Installing…" : "Install") { install() }.buttonStyle(.borderedProminent).disabled(busy)
            }
            ScrollView { Text(output.isEmpty ? "Close GD and OBS, check the paths above, then click Install.\nAfterward: add GD Alternate Song in OBS once, then right-click a song in Jukebox for its purple OBS check." : output).font(.system(.body, design: .monospaced)).textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading).padding(10) }.frame(height: 165).background(.quaternary.opacity(0.3)).cornerRadius(8)
            Text("Mac test build • Windows and Linux validation is still pending").font(.caption).foregroundStyle(.secondary)
        }.padding(26).frame(width: 650)
        .onAppear {
            if !FileManager.default.fileExists(atPath: obs) {
                obs = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Applications/OBS.app").path
            }
            NSApp.setActivationPolicy(.regular)
            NSApp.activate(ignoringOtherApps: true)
        }
    }
    private func pathRow(_ title: String, path: Binding<String>) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title).font(.headline)
            HStack {
                TextField(title, text: path).textFieldStyle(.roundedBorder)
                Button("Choose…") {
                    let panel = NSOpenPanel(); panel.canChooseFiles = true; panel.canChooseDirectories = true
                    panel.allowsMultipleSelection = false; panel.treatsFilePackagesAsDirectories = false
                    panel.message = "Select \(title)’s application"; panel.prompt = "Select"
                    if panel.runModal() == .OK, let selected = panel.url { path.wrappedValue = selected.path }
                }
            }
        }
    }
    private func install() {
        busy = true; output = "Installing the files listed above…"
        let gdPath = gd, obsPath = obs, script = root.appendingPathComponent("Install.command")
        DispatchQueue.global(qos: .userInitiated).async {
            let process = Process(); process.executableURL = URL(fileURLWithPath: "/bin/zsh")
            process.arguments = [script.path, gdPath, obsPath]
            let pipe = Pipe(); process.standardOutput = pipe; process.standardError = pipe
            process.standardInput = FileHandle.nullDevice
            do {
                try process.run()
                let data = pipe.fileHandleForReading.readDataToEndOfFile(); process.waitUntilExit()
                let text = String(data: data, encoding: .utf8) ?? "Could not read the installation log."
                DispatchQueue.main.async { output = text; busy = false }
            } catch { DispatchQueue.main.async { output = error.localizedDescription; busy = false } }
        }
    }
}
