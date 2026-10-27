import Foundation

/// One thing a Cheats row does in the game. PGQueueCheat runs it on the game thread once play resumes.
struct NativeCheatAction {
    let title: String
    let command: PGCheatCommand
    var value: Int32 = 0
}

/// A row on the Cheats page: a switch that stays on (`toggle`), or one or more actions to pick from.
struct NativeCheatGroup {
    let title: String
    let detail: String
    var actions: [NativeCheatAction] = []
    var toggle: PGCheatSwitch? = nil

    static let all: [NativeCheatGroup] = [
        .init(title: "God mode", detail: "Nothing can hurt you until you switch it off. Off again when the app restarts.",
              toggle: PGCheatGodMode),
        .init(title: "Never wanted", detail: "No wanted stars until you switch it off. Off again when the app restarts.",
              toggle: PGCheatNeverWanted),
        .init(title: "Repair vehicle", detail: "Fixes and cleans the vehicle you're in.", actions: [
            .init(title: "Repair vehicle", command: PGCheatRepairVehicle),
        ]),
        .init(title: "Clean vehicle", detail: "Washes the dirt off the vehicle you're in.", actions: [
            .init(title: "Clean vehicle", command: PGCheatCleanVehicle),
        ]),
        .init(title: "Time of day", detail: "Pick a time. The clock keeps running from there.", actions: [
            .init(title: "Morning · 06:00", command: PGCheatSetTime, value: 6),
            .init(title: "Noon · 12:00", command: PGCheatSetTime, value: 12),
            .init(title: "Evening · 18:00", command: PGCheatSetTime, value: 18),
            .init(title: "Midnight · 00:00", command: PGCheatSetTime, value: 0),
        ]),
        // Values are the weather names in NativeCheats.inc (menuWeather).
        .init(title: "Weather", detail: "Pick the weather, or go back to the game's own changing weather. Missions can change it.", actions: [
            .init(title: "Clear skies", command: PGCheatSetWeather, value: 0),
            .init(title: "Extra sunny", command: PGCheatSetWeather, value: 1),
            .init(title: "Cloudy", command: PGCheatSetWeather, value: 2),
            .init(title: "Overcast", command: PGCheatSetWeather, value: 3),
            .init(title: "Rain", command: PGCheatSetWeather, value: 4),
            .init(title: "Thunderstorm", command: PGCheatSetWeather, value: 5),
            .init(title: "Fog", command: PGCheatSetWeather, value: 6),
            .init(title: "Natural weather", command: PGCheatNaturalWeather),
        ]),
    ]
}
