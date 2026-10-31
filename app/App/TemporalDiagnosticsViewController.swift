import UIKit

/// A user-started, bounded renderer metadata capture. This page does not enable temporal upscaling.
final class TemporalDiagnosticsViewController: UIViewController {
    private enum CaptureStatus: Int {
        case idle = 0, armed, recording, saving, ready, failed
    }

    private static var sharedReportSignature: String?

    private let statusTitle = UILabel()
    private let statusDetail = UILabel()
    private let startButton = UIButton(type: .system)
    private let stopButton = UIButton(type: .system)
    private let shareButton = UIButton(type: .system)
    private var pollTimer: Timer?
    private var status: CaptureStatus = .idle
    private var reportURL: URL? {
        guard let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first,
              let pathPointer = PGTemporalDiagnosticsReportPath(),
              let path = String(validatingUTF8: pathPointer) else { return nil }
        let expected = documents.appendingPathComponent("temporal-diagnostics.json").standardizedFileURL
        let reported = URL(fileURLWithPath: path).standardizedFileURL
        return reported == expected ? expected : nil
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        title = text("Temporal diagnostics", simplified: "时域诊断", traditional: "時域診斷")
        overrideUserInterfaceStyle = .dark
        view.backgroundColor = Theme.night
        view.addSubview(Theme.dimmedBackdrop(image: "SettingsBackground"))
        if let background = view.subviews.first {
            background.translatesAutoresizingMaskIntoConstraints = false
            NSLayoutConstraint.activate([
                background.leadingAnchor.constraint(equalTo: view.leadingAnchor),
                background.trailingAnchor.constraint(equalTo: view.trailingAnchor),
                background.topAnchor.constraint(equalTo: view.topAnchor),
                background.bottomAnchor.constraint(equalTo: view.bottomAnchor)
            ])
        }

        let scroll = UIScrollView()
        scroll.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(scroll)
        NSLayoutConstraint.activate([
            scroll.leadingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.leadingAnchor),
            scroll.trailingAnchor.constraint(equalTo: view.safeAreaLayoutGuide.trailingAnchor),
            scroll.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor),
            scroll.bottomAnchor.constraint(equalTo: view.safeAreaLayoutGuide.bottomAnchor)
        ])

        let stack = UIStackView()
        stack.axis = .vertical
        stack.spacing = 16
        stack.translatesAutoresizingMaskIntoConstraints = false
        scroll.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: scroll.contentLayoutGuide.leadingAnchor, constant: 20),
            stack.trailingAnchor.constraint(equalTo: scroll.contentLayoutGuide.trailingAnchor, constant: -20),
            stack.topAnchor.constraint(equalTo: scroll.contentLayoutGuide.topAnchor, constant: 20),
            stack.bottomAnchor.constraint(equalTo: scroll.contentLayoutGuide.bottomAnchor, constant: -24),
            stack.widthAnchor.constraint(equalTo: scroll.frameLayoutGuide.widthAnchor, constant: -40)
        ])

        let statusCard = Theme.panel(cornerRadius: 16)
        let statusContent = UIStackView()
        statusContent.axis = .vertical
        statusContent.spacing = 6
        statusContent.translatesAutoresizingMaskIntoConstraints = false
        statusTitle.font = Theme.display(18, .bold)
        statusTitle.textColor = Theme.text
        statusTitle.numberOfLines = 0
        statusDetail.font = Theme.body(14)
        statusDetail.textColor = Theme.secondaryText
        statusDetail.numberOfLines = 0
        statusContent.addArrangedSubview(statusTitle)
        statusContent.addArrangedSubview(statusDetail)
        statusCard.contentView.addSubview(statusContent)
        NSLayoutConstraint.activate([
            statusContent.leadingAnchor.constraint(equalTo: statusCard.contentView.leadingAnchor, constant: 16),
            statusContent.trailingAnchor.constraint(equalTo: statusCard.contentView.trailingAnchor, constant: -16),
            statusContent.topAnchor.constraint(equalTo: statusCard.contentView.topAnchor, constant: 16),
            statusContent.bottomAnchor.constraint(equalTo: statusCard.contentView.bottomAnchor, constant: -16)
        ])
        stack.addArrangedSubview(statusCard)

        configureButton(startButton, title: text("Start capture", simplified: "开始采集", traditional: "開始擷取"), symbol: "record.circle")
        startButton.configuration = .filled()
        startButton.configuration?.title = text("Start capture", simplified: "开始采集", traditional: "開始擷取")
        startButton.configuration?.image = UIImage(systemName: "record.circle")
        startButton.configuration?.imagePadding = 8
        startButton.configuration?.cornerStyle = .capsule
        startButton.configuration?.baseBackgroundColor = Theme.accent
        startButton.configuration?.baseForegroundColor = UIColor(white: 0.12, alpha: 1)
        startButton.addAction(UIAction { [weak self] _ in self?.startCapture() }, for: .touchUpInside)

        configureButton(stopButton, title: text("Stop", simplified: "停止", traditional: "停止"), symbol: "stop.circle")
        stopButton.addAction(UIAction { [weak self] _ in
            PGTemporalDiagnosticsStop()
            self?.refreshStatus()
        }, for: .touchUpInside)

        configureButton(shareButton, title: text("Share report", simplified: "分享报告", traditional: "分享報告"), symbol: "square.and.arrow.up")
        shareButton.addAction(UIAction { [weak self] _ in self?.shareReport() }, for: .touchUpInside)

        stack.addArrangedSubview(startButton)
        stack.addArrangedSubview(stopButton)
        stack.addArrangedSubview(shareButton)

        let privacy = UILabel()
        privacy.font = Theme.body(14)
        privacy.textColor = Theme.secondaryText
        privacy.numberOfLines = 0
        privacy.text = text(
            "This does not turn on Temporal MetalFX and does not improve frame rate or image quality. The report includes bounded renderer metadata, including in-game camera/view and transform values; it does not include account information, screenshots, or audio. Nothing uploads automatically; sharing opens the iOS share sheet for you to choose.",
            simplified: "这不会启用 MetalFX 时域超分，也不会提升帧率或画质。报告会记录有限的渲染器元数据，包括游戏内相机/视图和变换数值；不含账号信息、截图或音频。不会自动上传；点击分享后由你在 iOS 分享菜单中选择。",
            traditional: "這不會啟用 MetalFX 時域超解析，也不會提升影格率或畫質。報告會記錄有限的渲染器中繼資料，包括遊戲內相機／視圖與變換數值；不含帳號資訊、螢幕截圖或音訊。不會自動上傳；點擊分享後由你在 iOS 分享選單中選擇。"
        )
        stack.addArrangedSubview(privacy)

        let instructions = UILabel()
        instructions.font = Theme.body(14)
        instructions.textColor = Theme.warning
        instructions.numberOfLines = 0
        instructions.text = text(
            "Set SR to Off in Graphics settings before starting. Capture begins 3 seconds after you tap Start, so return to gameplay before then; leaving this diagnostics/settings page open could sample the settings screen instead. Nothing dismisses the page or interrupts gameplay automatically. It stops after at most 60 game source-rendered frames (not display refreshes) or 15 seconds. The file is Documents/temporal-diagnostics.json; another capture replaces it, so share the current report first.",
            simplified: "开始前请在图像设置中将 SR 设为 Off。点击“开始”3 秒后开始采集，请在此之前返回游戏；若诊断/设置页面一直停留在前台，采样到的可能是设置画面。页面不会自动关闭，也不会自动打断游戏操作。最多记录 60 个游戏源渲染帧（不是屏幕刷新帧）或 15 秒后停止。文件保存在 Documents/temporal-diagnostics.json；再次采集会替换它，请先分享当前报告。",
            traditional: "開始前請在圖像設定中將 SR 設為 Off。點擊「開始」3 秒後開始擷取，請在此之前返回遊戲；若診斷／設定頁面一直停留在前景，取樣到的可能是設定畫面。頁面不會自動關閉，也不會自動打斷遊戲操作。最多記錄 60 個遊戲源渲染影格（不是螢幕更新影格）或 15 秒後停止。檔案保存在 Documents/temporal-diagnostics.json；再次擷取會取代它，請先分享目前報告。"
        )
        stack.addArrangedSubview(instructions)
        refreshStatus()
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        refreshStatus()
        pollTimer?.invalidate()
        pollTimer = Timer.scheduledTimer(timeInterval: 0.5, target: self, selector: #selector(pollStatus), userInfo: nil, repeats: true)
    }

    override func viewDidDisappear(_ animated: Bool) {
        super.viewDidDisappear(animated)
        pollTimer?.invalidate()
        pollTimer = nil
    }

    @objc private func pollStatus() { refreshStatus() }

    private func configureButton(_ button: UIButton, title: String, symbol: String) {
        var configuration = UIButton.Configuration.gray()
        configuration.title = title
        configuration.image = UIImage(systemName: symbol)
        configuration.imagePadding = 8
        configuration.cornerStyle = .capsule
        button.configuration = configuration
        button.heightAnchor.constraint(greaterThanOrEqualToConstant: 48).isActive = true
        button.contentHorizontalAlignment = .center
    }

    private func startCapture() {
        guard status == .idle || status == .ready || status == .failed else { return }
        if let reportURL, FileManager.default.fileExists(atPath: reportURL.path) {
            guard let signature = reportSignature(at: reportURL), Self.sharedReportSignature == signature else {
                let alert = UIAlertController(
                    title: text("Share the current report first", simplified: "请先分享当前报告", traditional: "請先分享目前報告"),
                    message: text("A new capture replaces Documents/temporal-diagnostics.json. Share the saved file first, then start the next capture.", simplified: "新的采集会替换 Documents/temporal-diagnostics.json。请先分享已保存的文件，再开始下一次采集。", traditional: "新的擷取會取代 Documents/temporal-diagnostics.json。請先分享已儲存的檔案，再開始下一次擷取。"),
                    preferredStyle: .alert
                )
                alert.addAction(UIAlertAction(title: text("Share report", simplified: "分享报告", traditional: "分享報告"), style: .default) { [weak self] _ in self?.shareReport() })
                alert.addAction(UIAlertAction(title: AppLanguage.choose("Cancel"), style: .cancel))
                present(alert, animated: true)
                return
            }
        }
        let alert = UIAlertController(
            title: text("Start a short diagnostics capture?", simplified: "开始短时诊断采集？", traditional: "開始短時間診斷擷取？"),
            message: text("Set SR to Off in Graphics settings before starting. Capture begins after a 3-second delay, so return to gameplay before then. This only records bounded metadata; it does not enable Temporal MetalFX or change spatial upscaling.", simplified: "开始前请在图像设置中将 SR 设为 Off。采集会在 3 秒后开始，请在此之前返回游戏。本操作仅记录有限元数据，不会启用 MetalFX 时域超分，也不会更改空间升频设置。", traditional: "開始前請在圖像設定中將 SR 設為 Off。擷取會在 3 秒後開始，請在此之前返回遊戲。本操作僅記錄有限中繼資料，不會啟用 MetalFX 時域超解析，也不會變更空間升頻設定。"),
            preferredStyle: .alert
        )
        alert.addAction(UIAlertAction(title: AppLanguage.choose("Cancel"), style: .cancel))
        alert.addAction(UIAlertAction(title: text("Start", simplified: "开始", traditional: "開始"), style: .default) { [weak self] _ in
            PGTemporalDiagnosticsStart()
            self?.refreshStatus()
        })
        present(alert, animated: true)
    }

    private func refreshStatus() {
        status = CaptureStatus(rawValue: Int(PGTemporalDiagnosticsStatus())) ?? .failed
        let title: String
        let detail: String
        switch status {
        case .idle:
            title = text("Ready when you are", simplified: "等待开始", traditional: "等待開始")
            detail = text("Nothing is being recorded. Tap Start while gameplay is rendering.", simplified: "当前没有采集。请在游戏画面正在渲染时点击“开始采集”。", traditional: "目前沒有擷取。請在遊戲畫面正在呈現時點擊「開始擷取」。")
        case .armed:
            title = text("Starting in 3 seconds…", simplified: "3 秒后开始…", traditional: "3 秒後開始…")
            detail = text("You can stop before capture begins.", simplified: "采集开始前仍可停止。", traditional: "擷取開始前仍可停止。")
        case .recording:
            title = text("Recording bounded metadata…", simplified: "正在采集有限元数据…", traditional: "正在擷取有限中繼資料…")
            detail = text("It stops after at most 60 game source-rendered frames (not display refreshes) or 15 seconds.", simplified: "最多记录 60 个游戏源渲染帧（不是屏幕刷新帧）或 15 秒后自动停止。", traditional: "最多記錄 60 個遊戲源渲染影格（不是螢幕更新影格）或 15 秒後自動停止。")
        case .saving:
            title = text("Saving report…", simplified: "正在保存报告…", traditional: "正在儲存報告…")
            detail = text("Keep the app open until saving finishes.", simplified: "请保持应用打开，直到保存完成。", traditional: "請保持應用程式開啟，直到儲存完成。")
        case .ready:
            title = text("Report ready", simplified: "报告已就绪", traditional: "報告已就緒")
            detail = text("Saved locally to Documents/temporal-diagnostics.json. Share it before recording again.", simplified: "已保存在本机 Documents/temporal-diagnostics.json。再次采集前请先分享。", traditional: "已保存在本機 Documents/temporal-diagnostics.json。再次擷取前請先分享。")
        case .failed:
            title = text("Capture failed", simplified: "采集失败", traditional: "擷取失敗")
            if let url = reportURL, FileManager.default.fileExists(atPath: url.path) {
                detail = text(
                    "The new report could not be saved; the previous report was preserved. Share it before starting another capture.",
                    simplified: "新报告保存失败；之前的报告已保留。开始下一次采集前，请先分享这份报告。",
                    traditional: "新報告儲存失敗；先前的報告已保留。開始下一次擷取前，請先分享這份報告。"
                )
            } else {
                detail = text("No report is ready to share. You can try again while the game is rendering.", simplified: "没有可分享的报告。请在游戏画面正在渲染时重试。", traditional: "沒有可分享的報告。請在遊戲畫面正在呈現時重試。")
            }
        }
        statusTitle.text = title
        statusDetail.text = detail
        let active = status == .armed || status == .recording
        stopButton.isEnabled = active
        stopButton.alpha = active ? 1 : 0.45
        let canShareSavedReport = status == .ready || status == .failed
        let exists = canShareSavedReport && reportURL.map { FileManager.default.fileExists(atPath: $0.path) } == true
        shareButton.isEnabled = exists
        shareButton.alpha = exists ? 1 : 0.45
        startButton.isEnabled = !active && status != .saving
        startButton.alpha = startButton.isEnabled ? 1 : 0.45
        startButton.configuration?.title = active
            ? text("Capture in progress", simplified: "采集中", traditional: "擷取中")
            : text("Start capture", simplified: "开始采集", traditional: "開始擷取")
    }

    private func reportSignature(at url: URL) -> String? {
        guard let values = try? url.resourceValues(forKeys: [.fileSizeKey, .contentModificationDateKey]),
              let size = values.fileSize,
              let date = values.contentModificationDate else { return nil }
        return "\(size):\(date.timeIntervalSince1970)"
    }

    private func shareReport() {
        guard (status == .ready || status == .failed),
              let url = reportURL,
              FileManager.default.fileExists(atPath: url.path) else { return }
        let signature = reportSignature(at: url)
        let activity = UIActivityViewController(activityItems: [url], applicationActivities: nil)
        activity.completionWithItemsHandler = { [weak self] _, completed, _, _ in
            guard completed, let signature else { return }
            DispatchQueue.main.async {
                Self.sharedReportSignature = signature
                self?.refreshStatus()
            }
        }
        if let popover = activity.popoverPresentationController {
            popover.sourceView = shareButton
            popover.sourceRect = shareButton.bounds
        }
        present(activity, animated: true)
    }

    static func text(_ english: String, simplified: String, traditional: String) -> String {
        switch AppLanguage.current.rawValue {
        case "zh-CN": return simplified
        case "zh-TW": return traditional
        default: return AppLanguage.choose(english)
        }
    }
}
