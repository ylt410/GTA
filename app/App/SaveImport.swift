import Foundation

/// Settings › Game mode › Import a Story save: a save file from a PC copy or a friend (SGTA5…) goes into a free
/// save slot, instead of players finding the save folder five levels deep in Files (13 players asked).
enum SaveImport {
    static var folder: URL {
        URL(fileURLWithPath: EnginePaths.filesRoot().path).appendingPathComponent("userdata/Documents/Rockstar Games/GTA V/Default")
    }

    enum Outcome { case imported(title: String, slot: Int), failed(String) }

    /// The save's title ("Unknowing The Truth (100.0%) - …"): after a 4-byte version (1), UTF-16 text.
    static func title(of data: Data) -> String? {
        guard data.count > 64, data.prefix(4) == Data([0, 0, 0, 1]) else { return nil }
        var units: [UInt16] = []
        var i = 4
        while i + 1 < min(data.count, 4 + 512) {
            let unit = UInt16(data[i]) | UInt16(data[i + 1]) << 8
            if unit == 0 { break }
            units.append(unit); i += 2
        }
        let text = String(decoding: units, as: UTF16.self).trimmingCharacters(in: .whitespacesAndNewlines)
        return text.count >= 3 && !text.contains("\u{FFFD}") ? text : nil
    }

    static func importSave(from url: URL) -> Outcome {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        guard let data = try? Data(contentsOf: url), data.count >= 64 * 1024, data.count <= 8 * 1024 * 1024, let title = title(of: data) else {
            return .failed(AppLanguage.choose("That isn't a GTA V Story save. Story saves are files named like SGTA50000, about 0.5 MB."))
        }
        let fm = FileManager.default
        try? fm.createDirectory(at: folder, withIntermediateDirectories: true)
        // Manual slots are SGTA50000-SGTA50014 (SGTA50015 is the autosave): keep the file's own slot when it is free,
        // otherwise the first free one.
        let name = url.lastPathComponent
        var slots = Array(0..<15)
        if name.range(of: #"^SGTA5\d{4}$"#, options: .regularExpression) != nil, let own = Int(name.suffix(2)), own < 15 {
            slots.removeAll { $0 == own }; slots.insert(own, at: 0)
        }
        guard let slot = slots.first(where: { !fm.fileExists(atPath: folder.appendingPathComponent(String(format: "SGTA500%02d", $0)).path) }) else {
            return .failed(AppLanguage.choose("All 15 save slots are in use. Delete a save in the game (Pause › Game › Load Game) or in Files, then try again."))
        }
        do {
            try data.write(to: folder.appendingPathComponent(String(format: "SGTA500%02d", slot)), options: .atomic)
            GameFolder.log("Save imported: \"\(title)\" into slot \(slot + 1) (\(data.count) bytes)")
            return .imported(title: title, slot: slot + 1)
        } catch {
            return .failed(AppLanguage.interpolate(AppLanguage.choose("The save couldn't be copied: {arg0}"), [error.localizedDescription]))
        }
    }
}
