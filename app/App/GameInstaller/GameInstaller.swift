import Foundation

/// Downloads (or reads) an archive of the game and unpacks only the game's files into
/// `<destination>/data/`, without ever needing room for both the archive and the game.
///
/// ```swift
/// let task = GameInstaller.install(
///     from: .url(URL(string: "https://example.com/GTA.zip")!),
///     to: documentsFolder,
///     manifest: Bundle.main.url(forResource: "manifest", withExtension: "json")!,
///     progress: { p in label.text = "\(p.extractedBytes) of \(p.totalBytes)" },
///     completion: { result in ... })
/// // task.cancel()
/// ```
///
/// - Formats: zip (incl. zip64), rar (v4), rar5, tar, tar.gz. Multi-part rar sets
///   (`x.part1.rar`, `x.part2.rar`... or `x.rar`, `x.r00`...) and byte-split sets
///   (`x.zip.001`, `x.zip.002`...) are read as one stream.
/// - Only files listed in the manifest are written; everything else is skipped. The folder
///   that holds the game inside the archive (e.g. `Some Folder/GTA/data/`) is found by itself.
/// - Nothing is ever written outside `<destination>/data/`.
public enum GameInstaller {

    /// Where the archive comes from.
    public enum Source {
        /// One archive over HTTP(S). Streamed through small rolling chunk files.
        case url(URL)
        /// A multi-part set over HTTP(S), in order (part 1 first).
        case urls([URL])
        /// A local archive file. Other parts of a multi-part set next to it are found by name.
        case file(URL)
        /// Local parts of a multi-part set, in order.
        case files([URL])
    }

    public struct Options {
        /// Local files only: the installer may destroy the archive as it reads it
        /// (frees the read part with F_PUNCHHOLE on APFS and deletes finished parts, and the
        /// archive itself after a successful unpack). Then the archive and the game hardly
        /// ever need disk space at the same time. If unpacking fails half way, the archive is gone.
        public var allowModifyingSource = false
        /// URL only: most bytes of downloaded-but-not-yet-unpacked data kept on disk.
        public var maxPendingBytes: Int64 = 256 << 20
        /// URL only: size of each rolling chunk file.
        public var chunkSize: Int64 = 8 << 20
        /// Space to leave free on top of what the install needs.
        public var spaceMargin: Int64 = 500_000_000
        /// URL only: give up when the network has made no progress for this long.
        public var giveUpAfter: TimeInterval = 180
        /// URL only: a connection with no data for this long is dropped and resumed.
        public var requestTimeout: TimeInterval = 30
        /// URL only: extra HTTP headers (e.g. a cookie or token for the file host).
        public var httpHeaders: [String: String] = [:]
        /// URL only: method and body of the first request (a download page's form); resumes use GET.
        public var httpMethod = "GET"
        public var httpBody: Data?
        /// URL only: cookies the session starts with (a browser's); it keeps ones set on redirects too.
        public var httpCookies: [HTTPCookie] = []
        /// URL only: fetch like a browser, one request for the whole file, and use ranges only to
        /// resume after a drop (some file hosts refuse a download asked for in pieces).
        public var singleRequest = false
        /// Queue for the progress and completion callbacks.
        public var callbackQueue: DispatchQueue = .main
        /// How often progress is reported.
        public var progressInterval: TimeInterval = 0.25
        /// Copies the manifest to `<destination>/data/manifest.json` when the install succeeds
        /// (the app's GameFolder check looks for it).
        public var writeManifest = true
        /// Log lines (plain text, any thread). Useful for the app's shareable log.
        public var log: ((String) -> Void)?
        /// Manifest paths whose size may differ from the manifest without failing verification
        /// (they must still exist). For this game's Archive.zip:
        /// `common/non_final/www/parser/psomismatch.html` and `.../schemas.html` are 2 bytes larger
        /// than the manifest says (also in the mirror's own unpacked copy). Tolerated ones are
        /// listed in `Report.toleratedSizeMismatches`.
        public var toleratedSizeMismatches: Set<String> = []
        /// Ignore (and delete) the record of an interrupted install at the destination and start from
        /// the beginning, rewriting every file. By default an interrupted install of the same archive
        /// continues where it stopped (see `GameInstaller.resumableInstall(at:)`).
        public var startFresh = false
        /// Tests only: pretend this much space is free.
        public var freeSpaceOverride: Int64?
        public init() {}
    }

    public enum Phase: String {
        case checking, downloading, unpacking, verifying, finished
    }

    public struct Progress {
        public var phase: Phase
        /// How far into the archive the download (URL) or reading (local) is, in bytes. When an
        /// interrupted install continues, this starts at `resumedFromByte`, not at 0.
        public var downloadedBytes: Int64
        /// Size of the whole archive when known (Content-Length / file sizes).
        public var downloadTotalBytes: Int64?
        /// Game bytes written so far, and the game's total size (from the manifest).
        public var extractedBytes: Int64
        public var totalBytes: Int64
        public var filesDone: Int
        public var filesTotal: Int
        /// The file being written right now (manifest path), if any.
        public var currentFile: String?
        /// Unpack speed (game bytes per second) over the last few seconds.
        public var bytesPerSecond: Double?
        /// Estimated seconds left, when there is enough data to guess.
        public var secondsRemaining: Double?
        /// URL mode: downloaded bytes waiting on disk to be unpacked.
        public var pendingBytes: Int64
        /// Times a dropped connection was resumed.
        public var retries: Int
        /// When an interrupted install continues: the archive offset it continued from (0 otherwise).
        public var resumedFromByte: Int64 = 0
        public var fraction: Double { totalBytes > 0 ? min(1, Double(extractedBytes) / Double(totalBytes)) : 0 }
    }

    public struct Report {
        /// The game's data folder: `<destination>/data`.
        public var dataFolder: URL
        public var filesWritten: Int
        public var bytesWritten: Int64
        public var duration: TimeInterval
        /// Folder inside the archive that held the game (e.g. "Some Folder/GTA/data/"), "" for the root.
        public var archivePrefix: String
        /// Archive entries that were not game files (skipped).
        public var skippedEntries: Int
        /// Entries refused for safety (absolute paths, "..", links).
        public var unsafeEntries: [String]
        /// URL mode: largest amount of downloaded data waiting on disk at once.
        public var peakPendingBytes: Int64
        /// Largest growth of disk use during the install, compared with the start
        /// (archive bytes still allocated + game bytes written + pending chunks).
        public var peakDiskIncrease: Int64
        /// Archive bytes downloaded (URL) or read (local) in THIS run.
        public var downloadedBytes: Int64
        public var retries: Int
        public var format: String
        /// The archive offset this run continued from (0 = it started at the beginning).
        public var resumedFromByte: Int64 = 0
        /// Game files unpacked by an earlier, interrupted run and kept (checked by size, not written again).
        public var filesKeptFromEarlier: Int = 0
        /// How an earlier interrupted install was continued, in plain words (nil when there was none).
        public var resumeNote: String?
        /// Files whose size differs from the manifest but were allowed by `toleratedSizeMismatches`.
        public var toleratedSizeMismatches: [String] = []
    }

    /// Starts an install. Returns at once; work happens on background threads.
    /// `completion` is called exactly once (also after `cancel()`).
    @discardableResult
    public static func install(from source: Source,
                               to destination: URL,
                               manifest: URL,
                               options: Options = Options(),
                               progress: @escaping (Progress) -> Void,
                               completion: @escaping (Result<Report, InstallError>) -> Void) -> InstallTask {
        let task = InstallTask()
        let job = InstallJob(source: source, destination: destination, manifestURL: manifest,
                             options: options, task: task, progress: progress, completion: completion)
        task.job = job
        job.start()
        return task
    }
}

/// Handle for a running install.
public final class InstallTask {
    var job: InstallJob?
    /// Stops the install as soon as possible. Completion then reports `.cancelled`.
    /// Files already unpacked are kept; the half-written file and downloaded chunks are removed.
    public func cancel() { job?.cancel() }
}

/// Plain-English install failures.
public struct InstallError: Error, LocalizedError, CustomStringConvertible {
    public enum Kind: String {
        case notEnoughSpace, outOfSpace, webPageNotFile, httpError, networkLost, fileChangedOnServer
        case notAnArchive, unsupported, passwordProtected, corrupted, truncated, missingPart
        case noGameFiles, verificationFailed, sourceUnreadable, manifestUnreadable, cancelled, writeFailed
        /// The download link stopped working part way (HTTP 401/403/404/410, or a web page instead of
        /// the file): typical for download-portal links that expire. Get a fresh link (reopen the
        /// download page) and call `install` again with the same destination: it continues from
        /// where it stopped.
        case linkExpired
    }
    public var kind: Kind
    /// A sentence or two a player can act on.
    public var message: String
    /// Files involved (corrupted file, missing/wrong-size files after verification...).
    public var files: [String] = []
    /// Technical detail for logs (libarchive message, HTTP status...).
    public var detail: String?
    public var errorDescription: String? { message }
    public var description: String { "[\(kind.rawValue)] \(message)" + (detail.map { " (\($0))" } ?? "") }

    init(_ kind: Kind, _ message: String, files: [String] = [], detail: String? = nil) {
        self.kind = kind; self.message = message; self.files = files; self.detail = detail
    }
}

func formatBytes(_ bytes: Int64) -> String {
    let b = Double(bytes)
    if abs(b) >= 1e9 { return String(format: "%.2f GB", b / 1e9) }
    if abs(b) >= 1e6 { return String(format: "%.0f MB", b / 1e6) }
    if abs(b) >= 1e3 { return String(format: "%.0f KB", b / 1e3) }
    return "\(bytes) bytes"
}
