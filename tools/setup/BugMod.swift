// BUG Mod 4.4 install/uninstall for the app (testers may not have Python).
// Same behaviour and manifest format as tools/bugmod.py: everything goes into
// the game's user folder (~/Documents/Civilization IV Beyond the Sword), files
// that get overwritten are backed up, and uninstall restores the folder.
import CryptoKit
import Foundation

enum BugMod {
    static let url = URL(string: "https://sourceforge.net/projects/civ4bug/files/The%20BUG%20Mod%20%28BTS%20v3.13%2B%29/The%20BUG%20Mod%204.4/BUGMod_4.4.zip/download")!
    static let sha256 = "c1e115bb1e957872b8b8a7d0812823435ec1f030cea02bf485281118fa3692a2"
    static let manifestName = "civ4bts4mac-bugmod.json"
    static let backupName = "civ4bts4mac-bugmod-backup"

    struct Failure: Error, CustomStringConvertible { let description: String }

    static func installedMode(docs: URL) -> String? {
        guard let data = try? Data(contentsOf: docs.appendingPathComponent(manifestName)),
              let m = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
        return m["mode"] as? String
    }

    /// Bring the folder to `mode` ("none", "customassets" or "mod").
    static func apply(mode: String, docs: URL, cache: URL, progress: (String) -> Void) throws {
        let current = installedMode(docs: docs) ?? "none"
        if current == mode { return }
        if current != "none" {
            progress("Removing BUG Mod…")
            try uninstall(docs: docs)
        }
        if mode != "none" {
            try install(mode: mode, docs: docs, cache: cache, progress: progress)
        }
    }

    static func fileSHA256(_ url: URL) -> String? {
        guard let data = try? Data(contentsOf: url) else { return nil }
        return SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
    }

    static func fetch(cache: URL, progress: (String) -> Void) throws -> URL {
        let fm = FileManager.default
        try fm.createDirectory(at: cache, withIntermediateDirectories: true)
        let zip = cache.appendingPathComponent("BUGMod_4.4.zip")
        if fileSHA256(zip) == sha256 { return zip }
        progress("Downloading BUG Mod 4.4 from SourceForge…")
        let done = DispatchSemaphore(value: 0)
        var result: Result<URL, Error> = .failure(Failure(description: "download did not finish"))
        URLSession.shared.downloadTask(with: url) { tmp, _, error in
            if let tmp {
                let part = zip.appendingPathExtension("part")
                try? fm.removeItem(at: part)
                do { try fm.moveItem(at: tmp, to: part); result = .success(part) } catch { result = .failure(error) }
            } else {
                result = .failure(error ?? Failure(description: "download failed"))
            }
            done.signal()
        }.resume()
        done.wait()
        let part = try result.get()
        guard fileSHA256(part) == sha256 else {
            try? fm.removeItem(at: part)
            throw Failure(description: "The BUG Mod download didn't match the expected checksum.")
        }
        try? fm.removeItem(at: zip)
        try fm.moveItem(at: part, to: zip)
        return zip
    }

    /// (source file, destination relative to docs) for every file to install.
    static func plan(src: URL, mode: String) -> [(URL, String)] {
        var moves: [(URL, String)] = []
        func addTree(_ sub: String, _ dest: String) {
            let base = src.appendingPathComponent(sub)
            guard let e = FileManager.default.enumerator(at: base, includingPropertiesForKeys: [.isRegularFileKey]) else { return }
            for case let f as URL in e where (try? f.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true {
                let rel = String(f.standardizedFileURL.path.dropFirst(base.standardizedFileURL.path.count + 1))
                moves.append((f, dest + "/" + rel))
            }
        }
        if mode == "customassets" {
            addTree("Assets", "CustomAssets")
            for sub in ["Info", "UserSettings", "GameSetUpCheck"] { addTree(sub, "BUG Mod/" + sub) }
        } else {
            for sub in ["Assets", "Info", "UserSettings", "GameSetUpCheck"] { addTree(sub, "MODS/BUG Mod 4.4/" + sub) }
            moves.append((src.appendingPathComponent("Readme.txt"), "MODS/BUG Mod 4.4/Readme.txt"))
        }
        addTree("PrivateMaps", "PublicMaps")
        return moves
    }

    /// The existing path for `rel` under docs, matched case-insensitively
    /// (the game creates lowercase folders such as CustomAssets/python).
    static func findExisting(docs: URL, rel: String) -> URL? {
        var cur = docs
        for part in rel.split(separator: "/") {
            guard let names = try? FileManager.default.contentsOfDirectory(atPath: cur.path),
                  let hit = names.first(where: { $0.lowercased() == part.lowercased() }) else { return nil }
            cur = cur.appendingPathComponent(hit)
        }
        return cur
    }

    static func install(mode: String, docs: URL, cache: URL, progress: (String) -> Void) throws {
        let fm = FileManager.default
        let zip = try fetch(cache: cache, progress: progress)
        progress("Installing BUG Mod…")
        let tmp = fm.temporaryDirectory.appendingPathComponent("bugmod-\(UUID().uuidString)")
        defer { try? fm.removeItem(at: tmp) }
        let ditto = Process()
        ditto.executableURL = URL(fileURLWithPath: "/usr/bin/ditto")
        ditto.arguments = ["-x", "-k", zip.path, tmp.path]
        try ditto.run()
        ditto.waitUntilExit()
        guard ditto.terminationStatus == 0 else { throw Failure(description: "Couldn't unpack BUG Mod.") }
        try fm.createDirectory(at: docs, withIntermediateDirectories: true)
        var written: [String] = [], backedUp: [String] = []
        let backupRoot = docs.appendingPathComponent(backupName)
        for (src, rel) in plan(src: tmp, mode: mode) {
            let existing = findExisting(docs: docs, rel: rel)
            let dest = existing ?? docs.appendingPathComponent(rel)
            let destRel = String(dest.path.dropFirst(docs.path.count + 1))
            if let existing {
                let b = backupRoot.appendingPathComponent(destRel)
                try fm.createDirectory(at: b.deletingLastPathComponent(), withIntermediateDirectories: true)
                try? fm.removeItem(at: b)
                try fm.copyItem(at: existing, to: b)
                backedUp.append(destRel)
                try fm.removeItem(at: existing)
            }
            try fm.createDirectory(at: dest.deletingLastPathComponent(), withIntermediateDirectories: true)
            try fm.copyItem(at: src, to: dest)
            written.append(destRel)
        }
        let manifest: [String: Any] = ["version": "4.4", "mode": mode, "files": written, "backed_up": backedUp]
        try JSONSerialization.data(withJSONObject: manifest, options: [.prettyPrinted])
            .write(to: docs.appendingPathComponent(manifestName))
    }

    static func uninstall(docs: URL) throws {
        let fm = FileManager.default
        let mpath = docs.appendingPathComponent(manifestName)
        guard let data = try? Data(contentsOf: mpath),
              let m = try JSONSerialization.jsonObject(with: data) as? [String: Any] else { return }
        let files = m["files"] as? [String] ?? [], backedUp = m["backed_up"] as? [String] ?? []
        for rel in files { try? fm.removeItem(at: docs.appendingPathComponent(rel)) }
        let backupRoot = docs.appendingPathComponent(backupName)
        for rel in backedUp {
            let b = backupRoot.appendingPathComponent(rel), d = docs.appendingPathComponent(rel)
            try? fm.createDirectory(at: d.deletingLastPathComponent(), withIntermediateDirectories: true)
            try? fm.moveItem(at: b, to: d)
        }
        // Remove folders the install left empty.
        for dir in Set(files.map { ($0 as NSString).deletingLastPathComponent }).sorted(by: { $0.count > $1.count }) {
            var d = docs.appendingPathComponent(dir)
            while d.path.hasPrefix(docs.path + "/"), (try? fm.contentsOfDirectory(atPath: d.path))?.isEmpty == true {
                try? fm.removeItem(at: d)
                d = d.deletingLastPathComponent()
            }
        }
        try? fm.removeItem(at: backupRoot)
        try fm.removeItem(at: mpath)
    }
}
