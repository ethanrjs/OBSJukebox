import SwiftUI
import AppKit

@main struct OBSJukeboxInstaller: App {
    var body: some Scene {
        WindowGroup("OBS Jukebox - macOS Setup") { SetupView() }
            .windowResizability(.contentSize)
    }
}

struct SetupView: View {
    @State private var gd = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/Steam/steamapps/common/Geometry Dash/Geometry Dash.app").path
    @State private var obs = "/Applications/OBS.app"
    @State private var output = ""
    @State private var status = "Finish any OBS recording, then click Install."
    @State private var failed = false
    @State private var busy = false
    @State private var closeApps = true
    @State private var reopen = true

    private var root: URL {
        if let resources = Bundle.main.resourceURL,
           FileManager.default.fileExists(atPath: resources.appendingPathComponent("Install.command").path) { return resources }
        return Bundle.main.bundleURL.deletingLastPathComponent()
    }
    private var stateRoot: URL {
        FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/OBS Jukebox")
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            HStack(spacing: 12) {
                if let logo = NSImage(contentsOf: root.appendingPathComponent("logo.png")) {
                    Image(nsImage: logo).resizable().scaledToFit().frame(width: 42, height: 42)
                }
                Text("OBS Jukebox").font(.system(size: 30, weight: .bold))
            }
            Text("Separate GD/Song output for OBS")
            pathRow("Geometry Dash application", path: $gd)
            pathRow("OBS Studio application", path: $obs)
            Text("Jukebox 3.8.0 is included and will be installed.")
                .padding(.vertical, 6)
            VStack(alignment: .leading, spacing: 8) {
                Toggle("Close GD and OBS for installation", isOn: $closeApps)
                Toggle("Reopen GD and OBS afterward", isOn: $reopen)
            }.toggleStyle(.checkbox).disabled(busy)
            Text(status)
                .font(.system(size: 15, weight: .bold))
                .foregroundStyle(failed ? Color.red : Color.primary)
                .fixedSize(horizontal: false, vertical: true)
                .padding(.top, 12)
            if !output.isEmpty {
                ScrollView {
                    Text(output).font(.system(size: 11, design: .monospaced))
                        .textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading)
                }.frame(maxHeight: 135)
            }
            Spacer(minLength: 0)
            Button(busy ? "Installing…" : "Install") { Task { await install() } }
                .disabled(busy)
            Button("Open backups and setup logs") { openLogs() }
                .buttonStyle(.link)
        }.padding(22).frame(width: 730, height: 650, alignment: .topLeading)
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
            Text(title)
            HStack(spacing: 8) {
                TextField(title, text: path).textFieldStyle(.roundedBorder)
                Button("Browse...") {
                    let panel = NSOpenPanel()
                    panel.canChooseFiles = true
                    panel.canChooseDirectories = false
                    panel.allowsMultipleSelection = false
                    panel.treatsFilePackagesAsDirectories = false
                    panel.message = "Choose \(title)"
                    panel.prompt = "Select"
                    if panel.runModal() == .OK, let selected = panel.url { path.wrappedValue = selected.path }
                }
            }.disabled(busy)
        }
    }

    private func openLogs() {
        do {
            try FileManager.default.createDirectory(at: stateRoot, withIntermediateDirectories: true)
            NSWorkspace.shared.open(stateRoot)
        } catch {
            status = "Could not open setup logs: \(error.localizedDescription)"
            failed = true
        }
    }

    @MainActor private func quitApplications(_ paths: [String]) async throws {
        let selected = Set(paths.map { URL(fileURLWithPath: $0).resolvingSymlinksInPath().standardizedFileURL })
        let running = NSWorkspace.shared.runningApplications.filter {
            guard let url = $0.bundleURL else { return false }
            return selected.contains(url.resolvingSymlinksInPath().standardizedFileURL)
        }
        if running.isEmpty { return }
        status = "Close any OBS confirmation to continue installation."
        for app in running { _ = app.terminate() }
        let deadline = Date().addingTimeInterval(30)
        while running.contains(where: { !$0.isTerminated }) && Date() < deadline {
            try await Task.sleep(nanoseconds: 200_000_000)
        }
        if running.contains(where: { !$0.isTerminated }) {
            throw NSError(domain: "OBSJukeboxSetup", code: 1, userInfo: [NSLocalizedDescriptionKey:
                "GD or OBS is still running. Finish any recording, close the application, then click Install again."])
        }
    }

    @MainActor private func install() async {
        busy = true
        failed = false
        output = ""
        status = "Installing OBS Jukebox…"
        let gdPath = gd, obsPath = obs, shouldReopen = reopen
        let script = root.appendingPathComponent("Install.command")
        do {
            guard FileManager.default.fileExists(atPath: gdPath + "/Contents/MacOS/Geometry Dash"),
                  Bundle(path: obsPath)?.executableURL != nil else {
                throw NSError(domain: "OBSJukeboxSetup", code: 2, userInfo: [NSLocalizedDescriptionKey:
                    "Choose the Geometry Dash and OBS Studio applications before installing."])
            }
            if closeApps { try await quitApplications([gdPath, obsPath]) }
            let result: (String, Bool) = await withCheckedContinuation { continuation in
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
                        continuation.resume(returning: (String(data: data, encoding: .utf8) ?? "Could not read the installation log.", process.terminationStatus == 0))
                    } catch {
                        continuation.resume(returning: (error.localizedDescription, false))
                    }
                }
            }
            output = result.0
            failed = !result.1
            status = result.1 ? "Installed. In OBS, add Sources > GD Sounds once." : "Installation did not finish. See the details below and setup logs."
            if result.1 && shouldReopen {
                NSWorkspace.shared.open(URL(fileURLWithPath: gdPath))
                NSWorkspace.shared.open(URL(fileURLWithPath: obsPath))
            }
        } catch {
            status = error.localizedDescription
            output = error.localizedDescription
            failed = true
        }
        do {
            let logs = stateRoot.appendingPathComponent("SetupLogs")
            try FileManager.default.createDirectory(at: logs, withIntermediateDirectories: true)
            try (status + "\n\n" + output).write(to: logs.appendingPathComponent(UUID().uuidString + ".log"), atomically: true, encoding: .utf8)
        } catch {
            output += "\nCould not save setup log: \(error.localizedDescription)"
        }
        busy = false
    }
}
