import UIKit

/// On launch, asks GitHub for the newest release, and when it is newer than this app, a popup points to
/// the Telegram group's Releases topic, where every new IPA is posted. Each new version is shown once.
/// It reads the release page's redirect to its tag: GitHub's API allows 60 requests an hour per
/// address, which players sharing a mobile carrier's address would run out of.
enum UpdateCheck {
    private static let latestRelease = URL(string: "https://github.com/kevinerikjs/gta5-ios-port/releases/latest")!
    /// The Releases topic of the Telegram group.
    private static let releasesTopic = URL(string: "https://t.me/fivestarport/786")!
    private static let shownKey = "updatePromptShownFor"

    static func run(from presenter: UIViewController, log: @escaping (String) -> Void) {
        let current = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "0"
        var request = URLRequest(url: latestRelease, timeoutInterval: 10)
        request.httpMethod = "HEAD"
        URLSession.shared.dataTask(with: request) { _, response, _ in
            // Redirected to .../releases/tag/v1.3.3
            let status = (response as? HTTPURLResponse)?.statusCode ?? 0
            let path = response?.url?.pathComponents ?? []
            guard status == 200, path.count >= 2, path[path.count - 2] == "tag" else {
                log("Update check: no answer from GitHub (HTTP \(status)).")
                return
            }
            let tag = path[path.count - 1]
            let latest = tag.trimmingCharacters(in: CharacterSet(charactersIn: "vV "))
            guard isNewer(latest, than: current) else { log("Update check: v\(current) is the newest (GitHub has v\(latest))."); return }
            DispatchQueue.main.async {
                guard UserDefaults.standard.string(forKey: shownKey) != latest else {
                    log("Update check: v\(latest) is out (this is v\(current)); already told the player.")
                    return
                }
                log("Update check: v\(latest) is out (this is v\(current)); showing the update popup.")
                show(latest: latest, current: current, from: presenter)
            }
        }.resume()
    }

    /// "1.3.10" > "1.3.9": compared number by number.
    static func isNewer(_ a: String, than b: String) -> Bool {
        let x = a.split(separator: ".").map { Int($0.prefix { $0.isNumber }) ?? 0 }
        let y = b.split(separator: ".").map { Int($0.prefix { $0.isNumber }) ?? 0 }
        for i in 0..<max(x.count, y.count) {
            let p = i < x.count ? x[i] : 0, q = i < y.count ? y[i] : 0
            if p != q { return p > q }
        }
        return false
    }

    private static func show(latest: String, current: String, from presenter: UIViewController) {
        var top = presenter
        while let next = top.presentedViewController, !next.isBeingDismissed { top = next }
        // Another popup is up (a startup notice): try again once it is gone.
        if top is UIAlertController {
            DispatchQueue.main.asyncAfter(deadline: .now() + 2) { show(latest: latest, current: current, from: presenter) }
            return
        }
        UserDefaults.standard.set(latest, forKey: shownKey)
        let alert = UIAlertController(
            title: AppLanguage.choose("Update available"),
            message: AppLanguage.interpolate(AppLanguage.choose("Version {arg0} is out. You have {arg1}.\n\nThe new IPA is posted in the Releases topic of our Telegram group. Install it the same way you installed this one."), ["v" + latest, "v" + current]),
            preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: AppLanguage.choose("Later"), style: .cancel))
        let open = UIAlertAction(title: AppLanguage.choose("Open Telegram"), style: .default) { _ in UIApplication.shared.open(releasesTopic) }
        alert.addAction(open)
        alert.preferredAction = open
        top.present(alert, animated: true)
    }
}
