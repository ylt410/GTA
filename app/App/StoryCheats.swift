import Foundation

/// One press of the game's own controller. Triggers are axes, not buttons.
enum CheatInput {
    case button(PadButtons)
    case leftTrigger
    case rightTrigger

    var state: PGPadState {
        var state = PGPadState()
        switch self {
        case .button(let buttons): state.padButtons = buttons
        case .leftTrigger: state.leftTrigger = 1
        case .rightTrigger: state.rightTrigger = 1
        }
        return state
    }
}

/// Story-mode cheats the game already accepts from a controller. Names match
/// the words people type on PC; the presses are the Xbox sequences.
struct StoryCheat {
    let title: String
    let detail: String
    let inputs: [CheatInput]
}

enum StoryCheats {
    private static func button(_ button: PadButtons) -> CheatInput { .button(button) }

    static let all: [StoryCheat] = [
        StoryCheat(title: "Invincibility", detail: "About 5 minutes. Tap again to refresh. You can still wreck the car.", inputs: invincibility),
        StoryCheat(title: "Health and armor", detail: "Fills health and armor.", inputs: health),
        StoryCheat(title: "Weapons and ammo", detail: "Gives weapons and fills ammo.", inputs: weapons),
        StoryCheat(title: "Raise wanted level", detail: "Adds one star.", inputs: wantedUp),
        StoryCheat(title: "Lower wanted level", detail: "Removes one star.", inputs: wantedDown),
        StoryCheat(title: "Skyfall", detail: "Drops you from the sky. A parachute is a separate cheat.", inputs: skyfall),
        StoryCheat(title: "Spawn Buzzard", detail: "Attack helicopter. Needs open space or it will not appear.", inputs: buzzard),
        StoryCheat(title: "Spawn Comet", detail: "Sports car. Needs room on the road.", inputs: comet),
        StoryCheat(title: "Fast run", detail: "Sprint speed. Tap again to turn it off.", inputs: fastRun),
        StoryCheat(title: "Fast swim", detail: "Swim speed. Tap again to turn it off.", inputs: fastSwim),
        StoryCheat(title: "Parachute", detail: "Puts a parachute on you.", inputs: parachute),
    ]

    private static let invincibility: [CheatInput] = [
        button(.right), button(.a), button(.right), button(.left), button(.right), button(.r1),
        button(.right), button(.left), button(.a), button(.y),
    ]
    private static let health: [CheatInput] = [
        button(.b), button(.l1), button(.y), .rightTrigger, button(.a), button(.x), button(.b),
        button(.right), button(.x), button(.l1), button(.l1), button(.l1),
    ]
    private static let weapons: [CheatInput] = [
        button(.y), .rightTrigger, button(.left), button(.l1), button(.a), button(.right), button(.y),
        button(.down), button(.x), button(.l1), button(.l1), button(.l1),
    ]
    private static let wantedUp: [CheatInput] = [
        button(.r1), button(.r1), button(.b), .rightTrigger, button(.left), button(.right),
        button(.left), button(.right), button(.left), button(.right),
    ]
    private static let wantedDown: [CheatInput] = [
        button(.r1), button(.r1), button(.b), .rightTrigger, button(.right), button(.left),
        button(.right), button(.left), button(.right), button(.left),
    ]
    private static let skyfall: [CheatInput] = [
        button(.l1), .leftTrigger, button(.r1), .rightTrigger, button(.left), button(.right),
        button(.left), button(.right), button(.l1), .leftTrigger, button(.r1), .rightTrigger,
        button(.left), button(.right), button(.left), button(.right),
    ]
    private static let buzzard: [CheatInput] = [
        button(.b), button(.b), button(.l1), button(.b), button(.b), button(.b), button(.l1),
        .leftTrigger, button(.r1), button(.y), button(.b), button(.y),
    ]
    private static let comet: [CheatInput] = [
        button(.r1), button(.b), .rightTrigger, button(.right), button(.l1), .leftTrigger,
        button(.a), button(.a), button(.x), button(.r1),
    ]
    private static let fastRun: [CheatInput] = [
        button(.y), button(.left), button(.right), button(.right), .leftTrigger, button(.l1), button(.x),
    ]
    private static let fastSwim: [CheatInput] = [
        button(.left), button(.left), button(.l1), button(.right), button(.right), .rightTrigger,
        button(.left), .leftTrigger, button(.right),
    ]
    private static let parachute: [CheatInput] = [
        button(.left), button(.right), button(.l1), .leftTrigger, button(.r1), .rightTrigger,
        .rightTrigger, button(.left), button(.left), button(.right), button(.l1),
    ]
}
