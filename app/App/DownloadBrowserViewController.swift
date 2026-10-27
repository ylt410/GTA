import UIKit
import WebKit

/// A small browser for download pages ("click Download to start"): the player uses the page as
/// normal, and as soon as it starts sending a file, the app takes the download over and unpacks
/// it as it arrives (with the page's cookies, so sessions and download tokens still work).
final class DownloadBrowserViewController: UIViewController, WKNavigationDelegate, WKUIDelegate, WKDownloadDelegate {
    /// The file the page started sending, and how to ask for it exactly the way Safari did.
    var onDownload: ((URL, BrowserDownload) -> Void)?
    private let start: URL
    private var webView: WKWebView!
    private let hint = UILabel()
    private let progress = UIProgressView(progressViewStyle: .bar)
    private var observation: NSKeyValueObservation?
    private var caught = false
    /// The last request the page made in its main frame (method and form data of a "Download" button).
    private var lastRequest: URLRequest?
    /// How the last main-frame navigation started (a tap, a form, a script).
    private var lastNavigationType: WKNavigationType = .other
    /// Ad blocking and the pop-up guard; switched off when a site refuses downloads with them on.
    private var adBlocking = true
    /// The last page that finished loading (the one a refused download was started from).
    private var shownPage: URL?
    private var adBlockButton: UIBarButtonItem!
    private static func describe(_ url: URL?) -> String { "\(url?.host ?? "?")\(url?.path ?? "")" }   // no query: tokens stay out of logs
    /// Safari's own identity: WebKit builds the user agent (with its frozen OS version, and on an iPad
    /// the Mac one Safari uses there), and the app adds the part only Safari adds: "Version/<iOS> ...
    /// Safari/...". The page and the app's download then present the very same browser.
    /// Safari's user agent on this device, as WebKit builds it (filled by loadUserAgent).
    private(set) static var userAgent: String?
    /// Reads the user agent from a WebKit view set up like the browser's, once.
    static func loadUserAgent(_ done: @escaping (String?) -> Void) {
        if let userAgent { done(userAgent); return }
        let configuration = WKWebViewConfiguration()
        configuration.applicationNameForUserAgent = safariApplicationName
        configuration.defaultWebpagePreferences.preferredContentMode = iPad ? .desktop : .mobile
        let probe = WKWebView(frame: .zero, configuration: configuration)
        probe.evaluateJavaScript("navigator.userAgent") { value, _ in
            _ = probe
            userAgent = value as? String
            done(userAgent)
        }
    }
    private static var iPad: Bool { UIDevice.current.userInterfaceIdiom == .pad }
    private static var safariApplicationName: String {
        let parts = UIDevice.current.systemVersion.split(separator: ".").prefix(2)
        let version = parts.count == 2 && parts[1] != "0" ? parts.joined(separator: ".") : String(parts.first ?? "18") + ".0"
        return iPad ? "Version/\(version) Safari/605.1.15" : "Version/\(version) Mobile/15E148 Safari/604.1"
    }
    private static let archiveTypes: Set<String> = ["application/zip", "application/x-zip-compressed", "application/x-zip", "application/vnd.rar",
                                                    "application/x-rar-compressed", "application/x-rar", "application/x-7z-compressed",
                                                    "application/octet-stream", "application/x-tar", "binary/octet-stream"]

    init(url: URL) {
        start = url
        super.init(nibName: nil, bundle: nil)
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    override func viewDidLoad() {
        super.viewDidLoad()
        title = start.host
        view.backgroundColor = Theme.night
        let configuration = WKWebViewConfiguration()
        configuration.websiteDataStore = .default()
        configuration.applicationNameForUserAgent = Self.safariApplicationName
        configuration.defaultWebpagePreferences.preferredContentMode = Self.iPad ? .desktop : .mobile
        configuration.preferences.javaScriptCanOpenWindowsAutomatically = false
        // Pages may not open new windows by script: ad pop-ups go nowhere, file links still load here.
        configuration.userContentController.addUserScript(WKUserScript(source: Self.popupGuard, injectionTime: .atDocumentStart, forMainFrameOnly: false))
        webView = WKWebView(frame: .zero, configuration: configuration)
        // The page loads once the ad blocking is in place.
        Self.adRules { [weak self] rules in
            guard let self else { return }
            if let rules { self.webView.configuration.userContentController.add(rules) }
            self.webView.load(URLRequest(url: self.start))
        }
        webView.navigationDelegate = self
        webView.uiDelegate = self
        webView.allowsBackForwardNavigationGestures = true
        hint.text = AppLanguage.choose("Tap the page's download button. The app takes the download over by itself and unpacks it.")
        hint.font = Theme.body(13, .medium)
        hint.textColor = Theme.text
        hint.numberOfLines = 2
        hint.textAlignment = .center
        hint.backgroundColor = Theme.accentDeep.withAlphaComponent(0.9)
        progress.progressTintColor = Theme.accent
        for item in [webView!, hint, progress] as [UIView] { item.translatesAutoresizingMaskIntoConstraints = false; view.addSubview(item) }
        let guide = view.safeAreaLayoutGuide
        NSLayoutConstraint.activate([
            hint.topAnchor.constraint(equalTo: guide.topAnchor),
            hint.leadingAnchor.constraint(equalTo: view.leadingAnchor), hint.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            hint.heightAnchor.constraint(greaterThanOrEqualToConstant: 34),
            progress.topAnchor.constraint(equalTo: hint.bottomAnchor),
            progress.leadingAnchor.constraint(equalTo: view.leadingAnchor), progress.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            webView.topAnchor.constraint(equalTo: progress.bottomAnchor),
            webView.leadingAnchor.constraint(equalTo: view.leadingAnchor), webView.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            webView.bottomAnchor.constraint(equalTo: view.bottomAnchor)
        ])
        navigationItem.leftBarButtonItem = UIBarButtonItem(systemItem: .close, primaryAction: UIAction { [weak self] _ in self?.dismiss(animated: true) })
        adBlockButton = UIBarButtonItem(image: UIImage(systemName: "shield.lefthalf.filled"), primaryAction: UIAction { [weak self] _ in
            guard let self else { return }
            self.setAdBlocking(!self.adBlocking, reason: "turned off by the player")
        })
        adBlockButton.accessibilityLabel = AppLanguage.choose("Ad blocking")
        navigationItem.rightBarButtonItems = [
            adBlockButton,
            UIBarButtonItem(image: UIImage(systemName: "arrow.clockwise"), primaryAction: UIAction { [weak self] _ in self?.webView.reload() }),
            UIBarButtonItem(image: UIImage(systemName: "chevron.backward"), primaryAction: UIAction { [weak self] _ in self?.webView.goBack() })
        ]
        observation = webView.observe(\.estimatedProgress) { [weak self] web, _ in
            self?.progress.setProgress(Float(web.estimatedProgress), animated: true)
            self?.progress.isHidden = web.estimatedProgress >= 1
        }
    }

    // MARK: Catching the download

    func webView(_ webView: WKWebView, decidePolicyFor navigationAction: WKNavigationAction, decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
        // Clicks hijacked to an ad network go nowhere.
        if adBlocking, Self.isAd(navigationAction.request.url) {
            GameFolder.log("Browser blocked a redirect to \(navigationAction.request.url?.host ?? "?")")
            decisionHandler(.cancel); return
        }
        if navigationAction.targetFrame?.isMainFrame != false {
            lastRequest = navigationAction.request
            lastNavigationType = navigationAction.navigationType
            GameFolder.log("Browser: \(navigationAction.request.httpMethod ?? "GET") \(Self.describe(navigationAction.request.url)) (\(Self.kind(navigationAction.navigationType)))")
        }
        // Links with a download attribute go straight to a download.
        decisionHandler(navigationAction.shouldPerformDownload ? .download : .allow)
    }

    func webView(_ webView: WKWebView, decidePolicyFor navigationResponse: WKNavigationResponse, decisionHandler: @escaping (WKNavigationResponsePolicy) -> Void) {
        let response = navigationResponse.response
        let disposition = (response as? HTTPURLResponse)?.value(forHTTPHeaderField: "Content-Disposition")?.lowercased() ?? ""
        let type = response.mimeType?.lowercased() ?? ""
        let big = response.expectedContentLength > 50_000_000
        let http = response as? HTTPURLResponse
        if navigationResponse.isForMainFrame {
            let mitigated = http?.value(forHTTPHeaderField: "CF-Mitigated")
            GameFolder.log("Browser: HTTP \(http?.statusCode ?? 0) \(type.isEmpty ? "no type" : type) for \(Self.describe(response.url))\(mitigated.map { " (Cloudflare \($0))" } ?? "")\(disposition.isEmpty ? "" : ", " + disposition)")
            // A tap that ends in a refusal (not a bot check, which solves itself): download sites often refuse
            // when their ad scripts were blocked. Turn ad blocking off and go back to the page once.
            if let status = http?.statusCode, [401, 403].contains(status), mitigated == nil, adBlocking,
               [.linkActivated, .formSubmitted, .other].contains(lastNavigationType), let page = shownPage, page != response.url {
                decisionHandler(.cancel)
                setAdBlocking(false, reason: "the site answered HTTP \(status) to the download", reloading: page)
                return
            }
        }
        if disposition.contains("attachment") || Self.archiveTypes.contains(type) || big || !navigationResponse.canShowMIMEType,
           let url = response.url {
            decisionHandler(.cancel)
            take(url)
        } else {
            decisionHandler(.allow)
        }
    }

    private static func kind(_ type: WKNavigationType) -> String {
        switch type {
        case .linkActivated: return "tap on a link"
        case .formSubmitted: return "form"
        case .backForward: return "back/forward"
        case .reload: return "reload"
        case .formResubmitted: return "form again"
        default: return "page or script"
        }
    }

    /// Ad blocking off: no content rules, no pop-up guard, and the page loads again without them.
    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) { shownPage = webView.url }

    private func setAdBlocking(_ on: Bool, reason: String, reloading page: URL? = nil) {
        adBlocking = on
        adBlockButton.image = UIImage(systemName: on ? "shield.lefthalf.filled" : "shield.slash")
        GameFolder.log("Browser: ad blocking \(on ? "on" : "off") (\(reason)).")
        let controller = webView.configuration.userContentController
        controller.removeAllContentRuleLists()
        controller.removeAllUserScripts()
        let reload = { [weak self] in
            guard let self else { return }
            self.hint.text = on ? AppLanguage.choose("Tap the page's download button. The app takes the download over by itself and unpacks it.")
                                : AppLanguage.choose("Ad blocking is off for this site. Tap the page's download button again.")
            if let page { self.webView.load(URLRequest(url: page)) } else { self.webView.reload() }
        }
        guard on else { reload(); return }
        controller.addUserScript(WKUserScript(source: Self.popupGuard, injectionTime: .atDocumentStart, forMainFrameOnly: false))
        Self.adRules { rules in
            if let rules { controller.add(rules) }
            reload()
        }
    }

    func webView(_ webView: WKWebView, navigationAction: WKNavigationAction, didBecome download: WKDownload) { catchDownload(download) }
    func webView(_ webView: WKWebView, navigationResponse: WKNavigationResponse, didBecome download: WKDownload) { catchDownload(download) }
    private func catchDownload(_ download: WKDownload) {
        download.delegate = self
        let request = download.originalRequest
        download.cancel { _ in }
        if let url = request?.url { take(url, request: request) }
    }
    func download(_ download: WKDownload, decideDestinationUsing response: URLResponse, suggestedFilename: String, completionHandler: @escaping (URL?) -> Void) {
        completionHandler(nil) // cancelled: the app downloads it itself
    }

    /// New windows: only a link that looks like the file itself is followed (here); everything else,
    /// which on download portals is nearly always an ad, is dropped.
    func webView(_ webView: WKWebView, createWebViewWith configuration: WKWebViewConfiguration, for navigationAction: WKNavigationAction, windowFeatures: WKWindowFeatures) -> WKWebView? {
        if let url = navigationAction.request.url, Self.looksLikeFile(url) { webView.load(navigationAction.request) }
        else { GameFolder.log("Browser blocked a pop-up: \(navigationAction.request.url?.host ?? "?")") }
        return nil
    }

    private static func looksLikeFile(_ url: URL) -> Bool {
        let name = url.lastPathComponent.lowercased()
        return [".zip", ".rar", ".7z", ".001"].contains(where: name.hasSuffix) || name.range(of: #"\.(z|r)\d\d$"#, options: .regularExpression) != nil
            || url.path.lowercased().contains("download")
    }

    /// Replaces window.open: file links open in this page, anything else is ignored.
    private static let popupGuard = """
    (function(){
      var looksLikeFile=function(u){try{var p=new URL(u,location.href).pathname.toLowerCase();return /\\.(zip|rar|7z|001)$|\\.(z|r)\\d\\d$|download/.test(p);}catch(e){return false;}};
      window.open=function(u){ if(u&&looksLikeFile(u)){ location.href=u; } return null; };
    })();
    """

    /// Content blocking for the ad and pop-under networks download portals use (compiled once).
    private static var compiledRules: WKContentRuleList?
    private static func adRules(_ done: @escaping (WKContentRuleList?) -> Void) {
        if let compiledRules { done(compiledRules); return }
        let rules = adNetworks.map { domain in
            ["trigger": ["url-filter": NSRegularExpression.escapedPattern(for: domain), "url-filter-is-case-sensitive": false], "action": ["type": "block"]] as [String: Any]
        }
        guard let data = try? JSONSerialization.data(withJSONObject: rules), let json = String(data: data, encoding: .utf8) else { done(nil); return }
        WKContentRuleListStore.default().compileContentRuleList(forIdentifier: "download-portal-ads-v2", encodedContentRuleList: json) { list, error in
            if let error { GameFolder.log("Browser ad rules didn't compile: \(error.localizedDescription)") }
            compiledRules = list
            done(list)
        }
    }

    /// The ad and pop-under networks download portals use.
    private static let adNetworks = ["propellerads.com", "propellerclick.com", "popads.net", "popcash.net", "adsterra.com", "adsterratools.com",
                        "exoclick.com", "exosrv.com", "juicyads.com", "a-ads.com", "adcash.com", "hilltopads.net", "hilltopads.com",
                        "clickadu.com", "monetag.com", "onclickads.net", "onclickalgo.com", "onclckmn.com", "popmyads.com", "trafficjunky.net",
                        "ad-maven.com", "admaven.com", "adspyglass.com", "pushame.com", "richpush.co", "evadav.com", "galaksion.com",
                        "realsrv.com", "highperformanceformat.com", "profitablecpmrate.com", "effectiveratecpm.com", "displayvertising.com",
                        "doubleclick.net", "googlesyndication.com", "adservice.google.com", "taboola.com", "outbrain.com", "mgid.com",
                        "revcontent.com", "zeropark.com", "trafficstars.com", "tsyndicate.com", "adnium.com", "clickaine.com", "pemsrv.com",
                        "bidgear.com", "yllix.com", "ylx-1.com", "ylx-2.com", "ad.plus", "adskeeper.com", "adskeeper.co.uk", "acint.net",
                        "s.pemsrv.com", "betterads.co", "notifpush.com", "pushprofit.net", "go.oclasrv.com", "oclasrv.com", "onclasrv.com"]
    private static func isAd(_ url: URL?) -> Bool {
        guard let host = url?.host?.lowercased() else { return false }
        return adNetworks.contains { host == $0 || host.hasSuffix("." + $0) }
    }

    /// Hands the file to the app as Safari asked for it: the same user agent, headers, cookies, and
    /// (when the file came straight from a form) the same method and form data. The app then fetches
    /// it with one plain request, the way Safari does, and only uses ranges to resume after a drop.
    private func take(_ url: URL, request: URLRequest? = nil) {
        guard !caught else { return }
        caught = true
        hint.text = AppLanguage.choose("Download caught. Starting…")
        let page = webView.url
        // The request that produced the file: replayed as is when it asked for this URL directly; after
        // a redirect the file's own URL is fetched with GET, as Safari's redirect did.
        let original = request ?? lastRequest
        let direct = original.flatMap { $0.url == url ? $0 : nil }
        webView.evaluateJavaScript("navigator.userAgent") { [weak self] value, _ in
            guard let self else { return }
            self.webView.configuration.websiteDataStore.httpCookieStore.getAllCookies { cookies in
                if let value = value as? String { Self.userAgent = value }
                var download = BrowserDownload(method: direct?.httpMethod ?? "GET", body: direct?.httpBody, cookies: cookies)
                download.headers = Self.safariHeaders(for: url, page: page, userAgent: value as? String, method: download.method, body: download.body)
                for (name, value) in direct?.allHTTPHeaderFields ?? [:] where !["cookie", "range"].contains(name.lowercased()) {
                    download.headers[name] = value   // what WebKit itself put on the request wins
                }
                let host = url.host ?? ""
                let sent = cookies.filter { cookie in
                    let domain = cookie.domain.hasPrefix(".") ? String(cookie.domain.dropFirst()) : cookie.domain
                    return host == domain || host.hasSuffix("." + domain)
                }.count
                GameFolder.log("Browser caught a download from \(host): \(download.method)\(download.body.map { " with \($0.count) bytes of form data" } ?? ""), \(sent) cookies for that site, as \(download.headers["User-Agent"] ?? "?")")
                self.dismiss(animated: true) { self.onDownload?(url, download) }
            }
        }
    }

    /// The headers Safari sends when a tap on a page starts a download.
    static func safariHeaders(for url: URL, page: URL?, userAgent: String?, method: String, body: Data?) -> [String: String] {
        var headers = [
            "User-Agent": userAgent ?? "Mozilla/5.0 (iPhone; CPU iPhone OS 18_6 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) " + safariApplicationName,
            "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
            "Accept-Language": acceptLanguage,
            "Sec-Fetch-Dest": "document",
            "Sec-Fetch-Mode": "navigate",
            "Upgrade-Insecure-Requests": "1",
            // WebKit asks for Brotli only over HTTPS.
            "Accept-Encoding": url.scheme == "https" ? "gzip, deflate, br" : "gzip, deflate",
        ]
        var site = "none"
        if let page, let pageHost = page.host?.lowercased(), let host = url.host?.lowercased() {
            headers["Referer"] = page.absoluteString
            if page.scheme == url.scheme && pageHost == host && page.port == url.port { site = "same-origin" }
            else if registrable(pageHost) == registrable(host) { site = "same-site" }
            else { site = "cross-site" }
            if method == "POST", let scheme = page.scheme {
                headers["Origin"] = "\(scheme)://\(pageHost)" + (page.port.map { ":\($0)" } ?? "")
            }
        }
        headers["Sec-Fetch-Site"] = site
        if page != nil { headers["Sec-Fetch-User"] = "?1" }   // a tap on the page
        if method == "POST", body != nil { headers["Content-Type"] = "application/x-www-form-urlencoded" }
        return headers
    }

    /// Accept-Language as WebKit sends it: the first preferred language only, "en-GB,en;q=0.9".
    private static var acceptLanguage: String {
        let tag = (Locale.preferredLanguages.first ?? "en-US").replacingOccurrences(of: "_", with: "-")
        let base = tag.split(separator: "-").first.map(String.init) ?? tag
        return base == tag ? tag : "\(tag),\(base);q=0.9"
    }

    /// The site a host belongs to, close enough for Sec-Fetch-Site ("cdn.example.co.uk" -> "example.co.uk").
    private static func registrable(_ host: String) -> String {
        let labels = host.split(separator: ".")
        guard labels.count > 2 else { return host }
        let twoPart = ["co", "com", "net", "org", "ac", "gov", "edu"].contains(String(labels[labels.count - 2])) && labels.last!.count == 2
        return labels.suffix(twoPart ? 3 : 2).joined(separator: ".")
    }
}
