import Foundation

/// Where an install of the game files comes from.
enum GameInstallSource {
    /// A direct link to an archive: downloaded, unpacked as it arrives, and never stored whole.
    case url(URL, headers: [String: String] = [:])
    /// A file a download page started in the app's browser: fetched the way Safari asked for it.
    case browser(URL, BrowserDownload)
    /// An archive in Files (other parts of a multi-part set are found by name). When the app owns
    /// it (it's in the app's folder), the part already unpacked is freed as it goes, so the
    /// archive and the game don't both need room.
    case file(URL, ownedByApp: Bool)
    /// Every part of a multi-part archive, in order.
    case parts([URL])
}

/// How Safari asked for a file: method, form data, headers and the browser's cookies.
struct BrowserDownload {
    var method: String
    var body: Data?
    var headers: [String: String] = [:]
    var cookies: [HTTPCookie]
}

/// One progress report for the screen.
struct GameInstallUpdate {
    var title: String
    var detail: String
    var fraction: Double
}

enum GameInstallResult {
    case success
    case failure(String)
    /// The link was a web page, not the file (a download portal): open it in the download browser.
    case webPage(URL)
}

/// A running install (GameInstaller does the work). Reports on the main queue.
final class GameInstallRun {
    private var task: InstallTask?
    private var scoped: [URL] = []

    /// Debug web pages whose size web downloads change slightly; the game never reads them.
    private static let tolerated: Set<String> = ["common/non_final/www/parser/psomismatch.html", "common/non_final/www/parser/schemas.html"]

    static func start(_ source: GameInstallSource, to destination: URL,
                      progress: @escaping (GameInstallUpdate) -> Void,
                      completion: @escaping (GameInstallResult) -> Void) -> GameInstallRun {
        let run = GameInstallRun()
        guard let manifest = EnginePaths.manifest else {
            DispatchQueue.main.async { completion(.failure(AppLanguage.choose("The app's game file list is missing."))) }
            return run
        }
        var options = GameInstaller.Options()
        options.log = { GameFolder.log("Install: " + $0) }
        // A locked phone or a long Wi-Fi drop: iOS pauses the app, so keep trying for half an hour (it resumes
        // at the byte it stopped) instead of 3 minutes; players lost whole downloads to a short break (GTA-93).
        options.giveUpAfter = 1800
        options.toleratedSizeMismatches = tolerated
        #if targetEnvironment(simulator)
        // Simulator tests: PG_FAKE_FREE_GB pretends there is room, so a link install starts its download.
        if let gigabytes = ProcessInfo.processInfo.environment["PG_FAKE_FREE_GB"].flatMap(Int64.init) { options.freeSpaceOverride = gigabytes << 30 }
        #endif
        let installerSource: GameInstaller.Source
        switch source {
        case let .url(url, headers):
            // A pasted link is fetched as if typed into Safari, in one request like Safari.
            options.httpHeaders = DownloadBrowserViewController.safariHeaders(for: url, page: nil, userAgent: DownloadBrowserViewController.userAgent, method: "GET", body: nil)
                .merging(headers) { $1 }
            options.singleRequest = true
            installerSource = .url(url)
        case let .browser(url, request):
            options.httpHeaders = request.headers
            options.httpMethod = request.method
            options.httpBody = request.body
            options.httpCookies = request.cookies
            options.singleRequest = true
            installerSource = .url(url)
        case let .file(url, owned):
            run.access(url)
            options.allowModifyingSource = owned
            installerSource = .file(url)
        case let .parts(urls):
            urls.forEach(run.access)
            installerSource = .files(urls)
        }
        try? FileManager.default.createDirectory(at: destination, withIntermediateDirectories: true)
        // The real folder, so the space check measures the disk the files actually go to.
        let destination = destination.resolvingSymlinksInPath()
        GameFolder.log("Install started from \(Self.describe(source)) into \(destination.lastPathComponent)")
        run.task = GameInstaller.install(from: installerSource, to: destination, manifest: URL(fileURLWithPath: manifest), options: options,
            progress: { p in progress(Self.update(p)) },
            completion: { result in
                run.scoped.forEach { $0.stopAccessingSecurityScopedResource() }
                run.scoped = []
                switch result {
                case let .success(report):
                    GameFolder.log("Install finished: \(report.filesWritten) files, \(report.format), \(Int(report.duration)) s, peak extra disk \(report.peakDiskIncrease / 1_000_000) MB")
                    completion(.success)
                case let .failure(error):
                    GameFolder.log("Install failed: \(error)")
                    if error.kind == .webPageNotFile, case let .url(url, headers) = source, headers.isEmpty {
                        completion(.webPage(url))
                        return
                    }
                    // Refused the very first request (a bot check such as Cloudflare's, or a host that only
                    // serves browsers): open the link in the browser, which passes the check like Safari and
                    // hands the file back with its cookies.
                    if error.kind == .httpError, let detail = error.detail, detail.hasSuffix("first request"),
                       ["HTTP 401", "HTTP 403", "HTTP 429", "HTTP 503"].contains(where: detail.contains),
                       case let .url(url, headers) = source, headers.isEmpty {
                        GameFolder.log("The link refused the app's request (\(detail)); opening it in the browser.")
                        completion(.webPage(url))
                        return
                    }
                    var message = error.message
                    if error.kind == .cancelled { message = AppLanguage.choose("Cancelled. What was downloaded and unpacked is kept: Continue the download picks up where it stopped.") }
                    completion(.failure(message))
                }
            })
        return run
    }

    func cancel() { task?.cancel() }

    private func access(_ url: URL) {
        if url.startAccessingSecurityScopedResource() { scoped.append(url) }
    }

    private static func describe(_ source: GameInstallSource) -> String {
        switch source {
        case let .url(url, _): return "a link (\(url.host ?? "?"))"
        case let .browser(url, _): return "a download page's file (\(url.host ?? "?"))"
        case let .file(url, owned): return "\(url.lastPathComponent)\(owned ? " (freed as it unpacks)" : "")"
        case let .parts(urls): return "\(urls.count) parts"
        }
    }

    private static func update(_ p: GameInstaller.Progress) -> GameInstallUpdate {
        let size = { (bytes: Int64) in ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file) }
        var detail: [String] = []
        switch p.phase {
        case .checking:
            return GameInstallUpdate(title: AppLanguage.choose("Checking space and the archive…"), detail: "", fraction: 0)
        case .verifying:
            return GameInstallUpdate(title: AppLanguage.choose("Checking every file…"), detail: "", fraction: 1)
        case .finished:
            return GameInstallUpdate(title: AppLanguage.choose("Done"), detail: "", fraction: 1)
        case .downloading, .unpacking:
            detail.append(AppLanguage.interpolate(AppLanguage.choose("{arg0} of {arg1} unpacked · {arg2} of {arg3} files"), [size(p.extractedBytes), size(p.totalBytes), String(p.filesDone), String(p.filesTotal)]))
            if p.downloadedBytes > 0 {
                let speed = p.bytesPerSecond.map { " · " + size(Int64($0)) + "/s" } ?? ""
                let downloaded = p.downloadTotalBytes.map { AppLanguage.interpolate(AppLanguage.choose("Downloaded {arg0} of {arg1}"), [size(p.downloadedBytes), size($0)]) }
                    ?? AppLanguage.interpolate(AppLanguage.choose("Downloaded {arg0}"), [size(p.downloadedBytes)])
                detail.append(downloaded + speed)
            }
            if let seconds = p.secondsRemaining, seconds.isFinite {
                detail.append(seconds < 90 ? AppLanguage.choose("About a minute left") : AppLanguage.interpolate(AppLanguage.choose("About {arg0} minutes left"), [String(Int(seconds / 60))]))
            }
            if p.retries > 0 { detail.append(AppLanguage.interpolate(AppLanguage.choose("Connection dropped {arg0}× and resumed"), [String(p.retries)])) }
            if let file = p.currentFile { detail.append(file) }
            let format = p.phase == .downloading ? AppLanguage.choose("Downloading and unpacking… {arg0}%") : AppLanguage.choose("Unpacking… {arg0}%")
            let title = AppLanguage.interpolate(format, [String(Int(p.fraction * 100))])
            return GameInstallUpdate(title: title, detail: detail.joined(separator: "\n"), fraction: p.fraction)
        }
    }
}

extension Notification.Name {
    /// The shared install (GameInstallManager) started, made progress or finished.
    static let gameInstallChanged = Notification.Name("GameInstallChanged")
}

/// The one install that may run at a time. It belongs to the app, not to a screen, so the player
/// can leave the game files screen (start screen, Settings, controls layout) while it runs. Every
/// change is posted as `.gameInstallChanged`; only starting the game waits for it.
final class GameInstallManager {
    static let shared = GameInstallManager()
    enum State {
        case idle
        case running(GameInstallUpdate)
        case finished(GameInstallResult)
    }
    private(set) var state = State.idle
    private var run: GameInstallRun?
    var isRunning: Bool { if case .running = state { return true }; return false }

    func start(_ source: GameInstallSource, to destination: URL) {
        guard !isRunning else { return }
        state = .running(GameInstallUpdate(title: AppLanguage.choose("Starting…"), detail: "", fraction: 0))
        post()
        run = GameInstallRun.start(source, to: destination, progress: { [weak self] update in
            self?.state = .running(update)
            self?.post()
        }, completion: { [weak self] result in
            guard let self else { return }
            self.run = nil
            if case .success = result {
                // The new game is picked up from the app's folder (and checked) before Play unlocks.
                GameFolder.clear()
                DispatchQueue.global(qos: .userInitiated).async {
                    GameFolder.findInAppFolder()
                    DispatchQueue.main.async { self.state = .finished(result); self.post() }
                }
            } else {
                self.state = .finished(result)
                self.post()
            }
        })
    }

    func cancel() { run?.cancel() }

    /// Forgets a finished result once a screen has shown it.
    func acknowledge() { if case .finished = state { state = .idle } }

    private func post() { NotificationCenter.default.post(name: .gameInstallChanged, object: self) }
}
