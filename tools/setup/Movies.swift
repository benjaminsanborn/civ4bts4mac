// The game's own movies (the original game's wonder, era and victory movies
// and some intros) are Sorenson Video 3, which modern macOS can't decode; the
// rest are H.264 or MPEG-4 and play as they are. The app converts those to
// H.264 MP4 with the ffmpeg it carries (tools/build_ffmpeg.sh), once, in the
// background while the game runs: the game plays a converted movie from
// movies/ (mirroring its path in the game folder) when there is one, and
// skips it until then.
import AVFoundation
import Foundation

enum Movies {
    static let outDir = supportDir.appendingPathComponent("movies")
    static let doneMarker = outDir.appendingPathComponent(".done")
    static var ffmpeg: String { Bundle.main.executableURL!.deletingLastPathComponent().appendingPathComponent("ffmpeg").path }

    /// Whether everything that needs converting has been (for this installation).
    static func done(game: String) -> Bool {
        (try? String(contentsOf: doneMarker, encoding: .utf8)) == game
    }

    /// Whether AVFoundation can decode a movie's video (as the game runtime checks).
    static func playable(_ url: URL) -> Bool {
        let asset = AVURLAsset(url: url)
        guard let track = asset.tracks(withMediaType: .video).first, track.isPlayable else { return false }
        for d in track.formatDescriptions {
            let codec = CMFormatDescriptionGetMediaSubType(d as! CMFormatDescription)
            if codec == 0x5356_5133 /* SVQ3 */ || codec == 0x5356_5131 /* SVQ1 */ { return false }
        }
        return true
    }

    /// Starts the converter (this program with --convert-movies) in the
    /// background if there is anything left to do; it outlives the launcher,
    /// which turns into the game.
    static func convertInBackground(game: String) {
        guard !done(game: game), FileManager.default.isExecutableFile(atPath: ffmpeg) else { return }
        let p = Process()
        p.executableURL = Bundle.main.executableURL
        p.arguments = ["--convert-movies", game]
        try? FileManager.default.createDirectory(at: logDir, withIntermediateDirectories: true)
        let log = logDir.appendingPathComponent("movies.log")
        FileManager.default.createFile(atPath: log.path, contents: nil)
        p.standardOutput = try? FileHandle(forWritingTo: log)
        p.standardError = p.standardOutput
        p.standardInput = FileHandle.nullDevice
        try? p.run()
    }

    /// --convert-movies GAME: convert what needs it, one movie at a time, at low priority.
    static func convertMain(game: String) -> Never {
        let fm = FileManager.default
        try? fm.createDirectory(at: outDir, withIntermediateDirectories: true)
        // One converter at a time.
        let lock = open(outDir.appendingPathComponent(".lock").path, O_CREAT | O_RDWR, 0o644)
        guard lock >= 0, flock(lock, LOCK_EX | LOCK_NB) == 0 else { exit(0) }
        setpriority(PRIO_PROCESS, 0, 10)
        let root = URL(fileURLWithPath: game).resolvingSymlinksInPath()
        let movies = (fm.enumerator(at: root, includingPropertiesForKeys: nil)?.allObjects as? [URL] ?? [])
            .filter { $0.pathExtension.lowercased() == "mov" }
        var failed = 0
        for src in movies {
            let rel = String(src.resolvingSymlinksInPath().path.dropFirst(root.path.count + 1))
            let dst = outDir.appendingPathComponent(rel + ".mp4")
            if fm.fileExists(atPath: dst.path) || playable(src) { continue }
            try? fm.createDirectory(at: dst.deletingLastPathComponent(), withIntermediateDirectories: true)
            let tmp = dst.deletingLastPathComponent().appendingPathComponent(".converting.mp4")
            let (status, out) = run(ffmpeg, ["-nostdin", "-loglevel", "error", "-y", "-i", src.path,
                                             "-c:v", "h264_videotoolbox", "-b:v", "5M", "-pix_fmt", "yuv420p",
                                             "-c:a", "aac", "-b:a", "192k", "-movflags", "+faststart", tmp.path])
            if status == 0, (try? fm.moveItem(at: tmp, to: dst)) != nil {
                print("converted \(rel)")
            } else {
                try? fm.removeItem(at: tmp)
                print("could not convert \(rel): \(out)")
                failed += 1
            }
        }
        if failed == 0 { try? game.write(to: doneMarker, atomically: true, encoding: .utf8) }
        exit(failed == 0 ? 0 : 1)
    }
}
