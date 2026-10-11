// Setup window and launcher for the native Civilization IV: Beyond the Sword build.
//
// Development: run by tools/setup.sh before the game is built and linked:
//   Civ4Setup --out FILE [--current GAMEDIR] [--docs USERDIR]
// writes the choices to FILE as JSON and exits 0 (1 if cancelled).
//
// App: the bundle's main executable. If the saved installation is still valid
// (and Option isn't held), it starts the game straight away; otherwise it shows
// the window, applies the BUG Mod choice, saves it, and starts the game.
// Either way it picks the game installation (it must be the exact build the
// recompiler targets) and whether to install BUG Mod.
import AppKit
import CryptoKit
import SwiftUI

// The one game build the recompiler supports: Aspyr's Mac port, as sold on Steam.
let supportedBundleID = "com.aspyr.civ4bts"
let supportedVersion = "3.19"
let supportedSHA256 = "fec6efc11f8e6065c3e0bd0e5b06d58335fa6c0ce842e505c3cd470bb36143ae"
let appSubpath = "Beyond the Sword/Civilization IV Beyond the Sword.app"
let exeSubpath = appSubpath + "/Contents/MacOS/Civilization IV Beyond the Sword"
// The game's user folder (where BUG Mod goes); setup.sh passes it as --docs.
var docsDir: URL {
    URL(fileURLWithPath: argument("--docs") ?? (NSHomeDirectory() + "/Documents/Civilization IV Beyond the Sword"))
}

/// Running as the app (not from tools/setup.sh).
let appMode = argument("--out") == nil
let supportDir = URL(fileURLWithPath: NSHomeDirectory() + "/Library/Application Support/Civ4 BtS Native")
let logDir = URL(fileURLWithPath: NSHomeDirectory() + "/Library/Logs/Civ4 BtS Native")
let cacheDir = URL(fileURLWithPath: NSHomeDirectory() + "/Library/Caches/Civ4 BtS Native")
let configURL = supportDir.appendingPathComponent("config.json")

struct Config: Codable { var game: String; var bug: String }

func loadConfig() -> Config? {
    guard let d = try? Data(contentsOf: configURL) else { return nil }
    return try? JSONDecoder().decode(Config.self, from: d)
}

/// Replace this process with the game (Contents/MacOS/civ4), reading game data from `game`.
func launchGame(_ game: String) -> Never {
    let fm = FileManager.default
    try? fm.createDirectory(at: logDir, withIntermediateDirectories: true)
    let exe = Bundle.main.executableURL!.deletingLastPathComponent().appendingPathComponent("civ4").path
    setenv("CIV4_GAME", game, 1)
    setenv("CIV4_LENIENT", "1", 1)            /* log unimplemented calls instead of stopping */
    setenv("PYTHONDONTWRITEBYTECODE", "1", 1) /* never write into the signed bundle */
    let log = logDir.appendingPathComponent("game.log").path
    let fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0o644)
    if fd >= 0 { dup2(fd, 1); dup2(fd, 2); close(fd) }
    let argv: [UnsafeMutablePointer<CChar>?] = [strdup(exe), nil]
    execv(exe, argv)
    fputs("cannot start \(exe): \(String(cString: strerror(errno)))\n", stderr)
    exit(1)
}

func argument(_ name: String) -> String? {
    let args = CommandLine.arguments
    guard let i = args.firstIndex(of: name), i + 1 < args.count else { return nil }
    return args[i + 1]
}

struct Check: Identifiable {
    let id = UUID()
    let title: String
    let ok: Bool
    let detail: String
}

struct Install: Identifiable, Hashable {
    let path: String
    var id: String { path }
    var name: String { (path as NSString).lastPathComponent }
}

func validate(_ root: String) -> [Check] {
    let fm = FileManager.default
    let app = (root as NSString).appendingPathComponent(appSubpath)
    let exe = (root as NSString).appendingPathComponent(exeSubpath)
    var checks: [Check] = []
    let hasApp = fm.fileExists(atPath: exe)
    checks.append(Check(title: "Beyond the Sword application",
                        ok: hasApp, detail: hasApp ? appSubpath : "not found in this folder"))
    guard hasApp else { return checks }
    let info = NSDictionary(contentsOfFile: app + "/Contents/Info.plist") ?? [:]
    let bundleID = info["CFBundleIdentifier"] as? String ?? "?"
    let version = info["CFBundleShortVersionString"] as? String ?? "?"
    checks.append(Check(title: "Aspyr Mac port", ok: bundleID == supportedBundleID, detail: bundleID))
    checks.append(Check(title: "Version \(supportedVersion)", ok: version == supportedVersion, detail: "found \(version)"))
    var hash = "unreadable"
    if let data = fm.contents(atPath: exe) {
        hash = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    }
    checks.append(Check(title: "Supported build of the game executable", ok: hash == supportedSHA256,
                        detail: "SHA-256 \(hash.prefix(16))…"))
    let assets = fm.fileExists(atPath: (root as NSString).appendingPathComponent("Assets"))
        && fm.fileExists(atPath: (root as NSString).appendingPathComponent("Beyond the Sword/Assets"))
    checks.append(Check(title: "Game data (Assets)", ok: assets, detail: assets ? "present" : "missing"))
    return checks
}

/// Steam libraries (libraryfolders.vdf) and /Applications, for folders that look like Civ IV.
func detectInstalls(current: String?) -> [Install] {
    let fm = FileManager.default
    let home = fm.homeDirectoryForCurrentUser.path
    var libraries = [home + "/Library/Application Support/Steam"]
    if let vdf = try? String(contentsOfFile: home + "/Library/Application Support/Steam/steamapps/libraryfolders.vdf",
                             encoding: .utf8) {
        let re = try! NSRegularExpression(pattern: "\"path\"\\s+\"([^\"]+)\"")
        for m in re.matches(in: vdf, range: NSRange(vdf.startIndex..., in: vdf)) {
            if let r = Range(m.range(at: 1), in: vdf) { libraries.append(String(vdf[r])) }
        }
    }
    var found: [String] = []
    if let current, !current.isEmpty { found.append(current) }
    for lib in Set(libraries) {
        let common = lib + "/steamapps/common"
        for name in (try? fm.contentsOfDirectory(atPath: common)) ?? [] where name.contains("Civilization IV") {
            found.append(common + "/" + name)
        }
    }
    for name in (try? fm.contentsOfDirectory(atPath: "/Applications")) ?? [] where name.contains("Civilization IV") {
        let p = "/Applications/" + name
        if fm.fileExists(atPath: p + "/Beyond the Sword") { found.append(p) }
    }
    var seen = Set<String>()
    return found.map { ($0 as NSString).resolvingSymlinksInPath }.filter { seen.insert($0).inserted }.map { Install(path: $0) }
}

/// BUG Mod state from the manifest (tools/bugmod.py and BugMod.swift write the same one).
func bugInstalledMode() -> String? { BugMod.installedMode(docs: docsDir) }

final class Model: ObservableObject {
    @Published var installs: [Install]
    @Published var selected: Install?
    @Published var checks: [Check] = []
    @Published var installBUG: Bool
    @Published var bugMode: String
    @Published var working: String?   // progress while applying the choice
    @Published var error: String?
    let bugWasInstalled: String?

    init() {
        let current = argument("--current") ?? loadConfig()?.game
        installs = detectInstalls(current: current)
        bugWasInstalled = bugInstalledMode()
        installBUG = bugWasInstalled != nil
        bugMode = bugWasInstalled ?? "customassets"
        let firstValid = installs.first { validate($0.path).allSatisfy(\.ok) }
        select(firstValid ?? installs.first)
    }

    func select(_ i: Install?) {
        selected = i
        checks = i.map { validate($0.path) } ?? []
    }

    var valid: Bool { !checks.isEmpty && checks.allSatisfy(\.ok) }

    func choose() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.message = "Choose the folder that contains “Beyond the Sword” (for Steam: …/steamapps/common/Sid Meier's Civilization IV Beyond the Sword)"
        if panel.runModal() == .OK, let url = panel.url {
            let i = Install(path: url.path)
            if !installs.contains(i) { installs.append(i) }
            select(i)
        }
    }

    func finish() {
        guard let sel = selected else { return }
        if appMode {
            let bug = installBUG ? bugMode : "none"
            working = "Starting…"
            error = nil
            DispatchQueue.global().async {
                do {
                    try BugMod.apply(mode: bug, docs: docsDir, cache: cacheDir) { msg in
                        DispatchQueue.main.async { self.working = msg }
                    }
                    try FileManager.default.createDirectory(at: supportDir, withIntermediateDirectories: true)
                    try JSONEncoder().encode(Config(game: sel.path, bug: bug)).write(to: configURL)
                    DispatchQueue.main.async { launchGame(sel.path) }
                } catch {
                    DispatchQueue.main.async { self.working = nil; self.error = "\(error)" }
                }
            }
            return
        }
        guard let out = argument("--out") else { NSApp.terminate(nil); return }
        let result: [String: Any] = ["game": sel.path, "bug": installBUG ? bugMode : "none"]
        let data = try! JSONSerialization.data(withJSONObject: result, options: [.prettyPrinted, .sortedKeys])
        FileManager.default.createFile(atPath: out, contents: data)
        exit(0)
    }
}

struct CheckRow: View {
    let check: Check
    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Image(systemName: check.ok ? "checkmark.circle.fill" : "xmark.octagon.fill")
                .foregroundStyle(check.ok ? .green : .red)
            Text(check.title)
            Spacer()
            Text(check.detail).foregroundStyle(.secondary).font(.callout).lineLimit(1).truncationMode(.middle)
        }
    }
}

struct SetupView: View {
    @ObservedObject var model: Model

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            VStack(alignment: .leading, spacing: 6) {
                Text("Civilization IV: Beyond the Sword").font(.title.bold())
                Text("Native Apple Silicon build — setup").font(.title3).foregroundStyle(.secondary)
            }
            Text("""
                This build recompiles the game from your own copy, so it needs that copy installed: \
                **Aspyr’s Mac port of Beyond the Sword, version \(supportedVersion)** (the version sold on Steam). \
                It must be exactly this build of the game; other versions and the Windows release won’t work. \
                The game files are only read, never changed.
                """)
                .fixedSize(horizontal: false, vertical: true)
            if appMode {
                Text("This window appears on first launch; hold ⌥ Option while opening the app to come back to it. Movies (the intro, wonder and era movies) don’t play in this test version.")
                    .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }

            GroupBox("Game installation") {
                VStack(alignment: .leading, spacing: 10) {
                    HStack {
                        Picker("Installation", selection: Binding(get: { model.selected }, set: { model.select($0) })) {
                            if model.installs.isEmpty { Text("None found").tag(Install?.none) }
                            ForEach(model.installs) { Text($0.name).tag(Optional($0)) }
                        }
                        .labelsHidden()
                        Button("Choose Folder…") { model.choose() }
                    }
                    if let sel = model.selected {
                        Text(sel.path).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
                    }
                    ForEach(model.checks) { CheckRow(check: $0) }
                    if model.selected != nil && !model.valid {
                        Text("This folder can’t be used. Choose the folder of the Steam (Aspyr) Mac version, version \(supportedVersion).")
                            .font(.callout).foregroundStyle(.red)
                    }
                }
                .padding(6)
                .frame(maxWidth: .infinity, alignment: .leading)
            }

            GroupBox("BUG Mod") {
                VStack(alignment: .leading, spacing: 8) {
                    Toggle("Install BUG Mod 4.4", isOn: $model.installBUG)
                    Text("""
                        The BTS Unaltered Gameplay mod: a much richer interface (scoreboard, city and unit details, \
                        advisors, a clock, reminders) with the rules left exactly as they are. Downloaded from \
                        SourceForge (2.8 MB) and installed in your Documents folder, not in the game; \
                        turning this off later removes it and restores anything it replaced.
                        """)
                        .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                    Picker("Load it", selection: $model.bugMode) {
                        Text("Always on (CustomAssets)").tag("customassets")
                        Text("As a mod (Advanced ▸ Load a Mod)").tag("mod")
                    }
                    .pickerStyle(.radioGroup)
                    .disabled(!model.installBUG)
                    if let was = model.bugWasInstalled {
                        Text("Currently installed (\(was == "mod" ? "as a mod" : "always on")).")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                }
                .padding(6)
                .frame(maxWidth: .infinity, alignment: .leading)
            }

            HStack {
                if let w = model.working {
                    ProgressView().controlSize(.small)
                    Text(w).foregroundStyle(.secondary)
                } else if let e = model.error {
                    Text(e).foregroundStyle(.red).font(.callout)
                }
                Spacer()
                Button(appMode ? "Quit" : "Cancel") { exit(1) }.keyboardShortcut(.cancelAction)
                Button(appMode ? "Play" : "Continue") { model.finish() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(!model.valid || model.working != nil)
            }
        }
        .padding(24)
        .frame(width: 620)
    }
}

@main
enum Main {
    static func main() {
        // App: go straight to the game when the saved installation still checks
        // out, unless Option is held to change the setup.
        if appMode, let c = loadConfig(), !NSEvent.modifierFlags.contains(.option),
           validate(c.game).allSatisfy(\.ok) {
            launchGame(c.game)
        }
        SetupApp.main()
    }
}

struct SetupApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate
    @StateObject var model = Model()
    var body: some Scene {
        Window("Civ IV BtS Setup", id: "setup") {
            SetupView(model: model)
        }
        .windowResizability(.contentSize)
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ n: Notification) {
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ app: NSApplication) -> Bool { true }
}
