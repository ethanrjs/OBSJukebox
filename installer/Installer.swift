import SwiftUI
import AppKit

@main struct OBSJukeboxInstaller: App {
    var body: some Scene {
        WindowGroup("OBS Jukebox Setup") { SetupView() }
            .windowResizability(.contentSize)
    }
}
struct SetupView: View {
    @State private var gd = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/Steam/steamapps/common/Geometry Dash/Geometry Dash.app").path
    @State private var obs = "/Applications/OBS.app"
    @State private var output = ""
    @State private var busy = false
    @State private var reopen = true
    private var root: URL {
        if let resources = Bundle.main.resourceURL,
           FileManager.default.fileExists(atPath: resources.appendingPathComponent("Install.command").path) { return resources }
        return Bundle.main.bundleURL.deletingLastPathComponent()
    }
    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            Text("OBS Jukebox").font(.largeTitle.bold())
            Text("Separate music for your game and OBS, with game sound effects.").foregroundStyle(.secondary)
            pathRow("Geometry Dash", path: $gd)
            pathRow("OBS Studio", path: $obs)
            GroupBox("What this installs") {
                VStack(alignment: .leading, spacing: 8) {
                    Text("• OBS Jukebox 1.0.0 and Jukebox 3.8.0 in Geode’s mods folder")
                    Text("• Geode 5.10.1 if Geode is not already installed")
                    Text("• The GD Sounds source for OBS Studio")
                    Text("No audio driver or virtual cable is needed. Existing files are backed up in your Library/Application Support/OBS Jukebox folder.").foregroundStyle(.secondary)
                }.frame(maxWidth: .infinity, alignment: .leading).padding(8)
            }
            Toggle("Reopen Geometry Dash and OBS after installation", isOn: $reopen).disabled(busy)
            Text("Finish any OBS recording and close Geometry Dash and OBS before installing.").font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            HStack {
                if FileManager.default.fileExists(atPath: root.appendingPathComponent("Source").path) {
                    Button("View source") { NSWorkspace.shared.open(root.appendingPathComponent("Source")) }
                }
                Spacer()
                Button(busy ? "Installing…" : "Install") { install() }
                    .buttonStyle(.borderedProminent).disabled(busy)
            }
            ScrollView {
                Text(output.isEmpty ? "After installation, add Sources > GD Sounds in OBS once.\nUse the Game and OBS checkboxes in Jukebox to choose each song." : output)
                    .font(.system(.body, design: .monospaced)).textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading).padding(10)
            }.frame(height: 180).background(.quaternary.opacity(0.3)).cornerRadius(8)
            Text("OBS Jukebox 1.0.0 • macOS").font(.caption).foregroundStyle(.secondary)
        }.padding(26).frame(width: 650)
        .onAppear {
            if !FileManager.default.fileExists(atPath: obs) {
                let userOBS = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Applications/OBS.app").path
                if FileManager.default.fileExists(atPath: userOBS) { obs = userOBS }
            }
            NSApp.setActivationPolicy(.regular)
            NSApp.activate(ignoringOtherApps: true)
        }
    }
    private func pathRow(_ title: String, path: Binding<String>) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title).font(.headline)
            HStack {
                TextField(title, text: path).textFieldStyle(.roundedBorder).disabled(busy)
                Button("Choose…") {
                    let panel = NSOpenPanel()
                    panel.canChooseFiles = true
                    panel.canChooseDirectories = false
                    panel.allowsMultipleSelection = false
                    panel.treatsFilePackagesAsDirectories = false
                    panel.message = "Select \(title)’s application"
                    panel.prompt = "Select"
                    if panel.runModal() == .OK, let selected = panel.url { path.wrappedValue = selected.path }
                }.disabled(busy)
            }
        }
    }
    private func install() {
        busy = true
        output = "Installing…"
        let gdPath = gd, obsPath = obs, shouldReopen = reopen
        let script = root.appendingPathComponent("Install.command")
        DispatchQueue.global(qos: .userInitiated).async {
            let process = Process()
            process.executableURL = URL(fileURLWithPath: "/bin/zsh")
            process.arguments = [script.path, gdPath, obsPath]
            let pipe = Pipe()
            process.standardOutput = pipe
            process.standardError = pipe
            process.standardInput = FileHandle.nullDevice
            do {
                try process.run()
                let data = pipe.fileHandleForReading.readDataToEndOfFile()
                process.waitUntilExit()
                let text = String(data: data, encoding: .utf8) ?? "Could not read the installation log."
                let succeeded = process.terminationStatus == 0
                DispatchQueue.main.async {
                    output = text
                    busy = false
                    if succeeded && shouldReopen {
                        NSWorkspace.shared.open(URL(fileURLWithPath: gdPath))
                        NSWorkspace.shared.open(URL(fileURLWithPath: obsPath))
                    }
                }
            } catch {
                DispatchQueue.main.async { output = error.localizedDescription; busy = false }
            }
        }
    }
}
