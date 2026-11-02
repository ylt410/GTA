// Engine-free UIKit harness for TouchControlsView. The runner appends this file
// to the real control source so the checks can inspect its private touch state.
// Engine calls are stubs: these checks verify touch routing and pad output only.
import UIKit

struct PGPadState {
  var buttons: UInt32 = 0
  var leftX: Float = 0
  var leftY: Float = 0
  var rightX: Float = 0
  var rightY: Float = 0
  var leftTrigger: Float = 0
  var rightTrigger: Float = 0
}
struct PGControlHints {
  var specialShown = false, specialReady = false, specialActive = false, canDetonate = false,
    helpContext = false, helpShown = false, helpDetonate = false, passenger = false,
    taxiMenu = false, trailerAttached = false, selfRadioPlaying = false,
    phoneBrowser = false, radioWheelOpen = false
  var helpInputs: UInt32 = 0
  var specialFill: Float = 0
  var vehicleModel: UInt32 = 0, pedModel: UInt32 = 0
}
func PGClearCameraSwipe() {}
func PGQueueCameraSwipe(_ x: Float, _ y: Float) {}
func PGMouseInput(_ x: Int32, _ y: Int32, _ z: Int32) {}
func PGMouseWheel(_ notches: Int32) {}
func PGInputReadWithin(_ action: Int32, _ milliseconds: Double) -> Int32 { 0 }
func PGSetKey(_ virtualKey: UInt8, _ down: Bool) {}
func PGNextAircraftWeapon() {}
func PGSetRadioWheelDirection(_ x: Float, _ y: Float, _ active: Bool) {}
func PGSelfRadioNext() {}
enum AppLanguage { static func choose(_ s: String) -> String { s } }
final class GameSettings {
  static let shared = GameSettings()
  var airplaneRightStick = false
  var sprintButton = false
  var steeringButtons = false
  var controlOpacity = 0.8
  var controlScale = 1.0
  var aimSensitivity = 1.0
  var lookSensitivity = 1.0
  var fireAlsoAims = true
  var mouseAimSensitivity = 1.0
  var mouseLookSensitivity = 1.0
  var swipeAimSensitivity = 1.0
  var swipeLookSensitivity = 1.0
  enum CameraTouchMode { case stick, swipePad, swipeMouse }
  var cameraTouchMode = CameraTouchMode.stick
}
struct ControlsLayout {
  struct Entry { var x: CGFloat, y: CGFloat, scale: CGFloat, hidden: Bool }
  static func entry(group: String, name: String) -> Entry? { nil }
}
enum GameFolder { static var log: (String) -> Void = { _ in } }
enum MinimapGeometry {
  static func rect(_ layout: (dx: Double, dy: Double, scale: Double), in bounds: CGRect) -> CGRect {
    bounds
  }
}
final class TestTouch: UITouch {
  var point = CGPoint.zero
  var previous = CGPoint.zero
  override func location(in view: UIView?) -> CGPoint { point }
  override func previousLocation(in view: UIView?) -> CGPoint { previous }
}
@main final class ControlsChecksDelegate: UIResponder, UIApplicationDelegate {
  var window: UIWindow?
  func application(
    _ a: UIApplication, didFinishLaunchingWithOptions o: [UIApplication.LaunchOptionsKey: Any]?
  ) -> Bool {
    let w = UIWindow(frame: UIScreen.main.bounds)
    let v = UIViewController()
    v.view.backgroundColor = .darkGray
    w.rootViewController = v
    window = w
    w.makeKeyAndVisible()
    let c = TouchControlsView(frame: v.view.bounds)
    c.autoresizingMask = [.flexibleWidth, .flexibleHeight]
    v.view.addSubview(c)
    c.regressionCheck()
    exit(0)
  }
}

extension TouchControlsView {
  func regressionCheck() {
    // Both motorcycle and bicycle sticks must preserve full positive/negative
    // lean while turning either way on the same finger.
    for vehicle in [UInt32(12), UInt32(13)] {
      updateContext(vehicle: vehicle, pauseMenu: false)
      layoutIfNeeded()
      for leanDirection in [CGFloat(1), CGFloat(-1)] {
        for turnDirection in [CGFloat(1), CGFloat(-1)] {
          let diagonal = TestTouch()
          diagonal.point = CGPoint(x: bounds.width * 0.35, y: bounds.height * 0.50)
          touchesBegan([diagonal], with: nil)
          diagonal.previous = diagonal.point
          diagonal.point.y -= moveStick.travel * leanDirection
          touchesMoved([diagonal], with: nil)
          diagonal.previous = diagonal.point
          diagonal.point.x += moveStick.travel * turnDirection
          touchesMoved([diagonal], with: nil)
          let pad = padState(elapsed: 1.0 / 60)
          let turnMagnitude = pad.leftX * Float(turnDirection)
          assert(turnMagnitude > 0.6 && turnMagnitude < 0.7)
          assert(pad.leftY * Float(leanDirection) > 0.99)
          touchesEnded([diagonal], with: nil)
          let released = padState(elapsed: 1.0 / 60)
          assert(released.leftX == 0 && released.leftY == 0)
        }
      }
    }

    // Both UIKit cancellation and changing vehicle while a lean is held must
    // release the stick instead of leaving a stale wheelie/stoppie axis.
    updateContext(vehicle: 12, pauseMenu: false)
    let cancelled = TestTouch()
    cancelled.point = CGPoint(x: bounds.width * 0.35, y: bounds.height * 0.50)
    touchesBegan([cancelled], with: nil)
    cancelled.previous = cancelled.point
    cancelled.point.y += moveStick.travel
    touchesMoved([cancelled], with: nil)
    cancelled.previous = cancelled.point
    cancelled.point.x -= moveStick.travel
    touchesMoved([cancelled], with: nil)
    let beforeCancel = padState(elapsed: 1.0 / 60)
    assert(beforeCancel.leftX < -0.6 && beforeCancel.leftX > -0.7)
    assert(beforeCancel.leftY < -0.99)
    touchesCancelled([cancelled], with: nil)
    var released = padState(elapsed: 1.0 / 60)
    assert(released.leftX == 0 && released.leftY == 0)
    let contextChange = TestTouch()
    contextChange.point = CGPoint(x: bounds.width * 0.35, y: bounds.height * 0.50)
    touchesBegan([contextChange], with: nil)
    contextChange.previous = contextChange.point
    contextChange.point.y -= moveStick.travel
    touchesMoved([contextChange], with: nil)
    assert(padState(elapsed: 1.0 / 60).leftY > 0.99)
    updateContext(vehicle: 1, pauseMenu: false)
    released = padState(elapsed: 1.0 / 60)
    assert(released.leftX == 0 && released.leftY == 0)
    touchesEnded([contextChange], with: nil)

    // A car still uses the old circular stick curve in ordinary steering mode.
    updateContext(vehicle: 1, pauseMenu: false)
    let carDiagonal = TestTouch()
    carDiagonal.point = CGPoint(x: bounds.width * 0.35, y: bounds.height * 0.50)
    touchesBegan([carDiagonal], with: nil)
    carDiagonal.previous = carDiagonal.point
    carDiagonal.point.y -= moveStick.travel
    touchesMoved([carDiagonal], with: nil)
    carDiagonal.previous = carDiagonal.point
    carDiagonal.point.x += moveStick.travel
    touchesMoved([carDiagonal], with: nil)
    let carPad = padState(elapsed: 1.0 / 60)
    assert(carPad.leftX > 0.6 && carPad.leftX < 0.7)
    assert(carPad.leftY > 0.54 && carPad.leftY < 0.56)
    touchesEnded([carDiagonal], with: nil)

    // Steering buttons must not disable the left stick's vertical bike-lean
    // axis: a second finger can steer while the wheelie remains held.
    updateContext(vehicle: 12, pauseMenu: false)
    GameSettings.shared.steeringButtons = true
    updateVisibility()
    layoutIfNeeded()
    let lean = TestTouch()
    lean.point = CGPoint(x: bounds.width * 0.35, y: bounds.height * 0.50)
    touchesBegan([lean], with: nil)
    lean.previous = lean.point
    lean.point.y -= moveStick.travel
    touchesMoved([lean], with: nil)
    let turn = TestTouch()
    turn.point = steerRight.center
    touchesBegan([turn], with: nil)
    var bikePad = padState(elapsed: 1.0 / 60)
    assert(bikePad.leftX > 0.9 && bikePad.leftY > 0.9)
    lean.previous = lean.point
    lean.point.x += moveStick.travel
    touchesMoved([lean], with: nil)
    bikePad = padState(elapsed: 1.0 / 60)
    assert(bikePad.leftX > 0.9 && bikePad.leftY > 0.99)
    touchesEnded([turn], with: nil)
    bikePad = padState(elapsed: 1.0 / 60)
    assert(bikePad.leftX == 0 && bikePad.leftY > 0.99)
    let turnLeft = TestTouch()
    turnLeft.point = steerLeft.center
    touchesBegan([turnLeft], with: nil)
    bikePad = padState(elapsed: 1.0 / 60)
    assert(bikePad.leftX < -0.9 && bikePad.leftY > 0.99)
    touchesEnded([lean], with: nil)
    bikePad = padState(elapsed: 1.0 / 60)
    assert(bikePad.leftX < -0.9 && bikePad.leftY == 0)
    touchesEnded([turnLeft], with: nil)
    updateContext(vehicle: 1, pauseMenu: false)
    let carTouch = TestTouch()
    carTouch.point = CGPoint(x: bounds.width * 0.35, y: bounds.height * 0.50)
    touchesBegan([carTouch], with: nil)
    assert(!roles.keys.contains(ObjectIdentifier(carTouch)))
    assert(padState(elapsed: 1.0 / 60).leftY == 0)
    touchesEnded([carTouch], with: nil)
    GameSettings.shared.steeringButtons = false

    updateContext(vehicle: 2, pauseMenu: false)
    layoutIfNeeded()
    assert(
      visible.contains(throttleUp) && visible.contains(yawLeft) && rightStick.idleOrigin == nil)
    GameSettings.shared.airplaneRightStick = false
    updateVisibility()
    let planeStick = TestTouch()
    planeStick.point = CGPoint(x: bounds.width * 0.35, y: bounds.height * 0.50)
    touchesBegan([planeStick], with: nil)
    planeStick.previous = planeStick.point
    planeStick.point.y -= moveStick.travel
    touchesMoved([planeStick], with: nil)
    planeStick.previous = planeStick.point
    planeStick.point.x += moveStick.travel
    touchesMoved([planeStick], with: nil)
    var pad = padState(elapsed: 1.0 / 60)
    assert(pad.leftX > 0.70 && pad.leftX < 0.71)
    assert(pad.leftY > 0.70 && pad.leftY < 0.71)
    touchesEnded([planeStick], with: nil)
    pad = padState(elapsed: 1.0 / 60)
    assert(pad.leftX == 0 && pad.leftY == 0)
    GameSettings.shared.airplaneRightStick = true
    updateVisibility()
    layoutIfNeeded()
    assert(
      !visible.contains(throttleUp) && !visible.contains(yawRight) && rightStick.idleOrigin != nil)
    let finger = TestTouch()
    finger.point = CGPoint(x: bounds.width * 0.8, y: bounds.height * 0.6)
    touchesBegan([finger], with: nil)
    assert(flyingWithRightStick)
    finger.previous = finger.point
    finger.point.y -= rightStick.travel
    touchesMoved([finger], with: nil)
    pad = padState(elapsed: 1.0 / 60)
    assert(abs(pad.rightTrigger - 0.5) < 0.01 && pad.leftTrigger == 0)
    finger.previous = finger.point
    finger.point.x += rightStick.travel
    touchesMoved([finger], with: nil)
    pad = padState(elapsed: 1.0 / 60)
    assert(pad.padButtons.contains(.r1) && pad.rightX == 0 && pad.rightY == 0)
    touchesEnded([finger], with: nil)
    assert(!flyingWithRightStick)
    pad = padState(elapsed: 1.0 / 60)
    assert(abs(pad.rightTrigger - 0.5) < 0.01 && rightStick.value == .zero)
    pressedAt.removeAll()
    lastHeld = []
    assert(!padState(elapsed: 1.0 / 60).padButtons.contains(.r1))
    finger.previous = finger.point
    touchesBegan([finger], with: nil)
    finger.previous = finger.point
    finger.point.y += rightStick.travel * 4
    touchesMoved([finger], with: nil)
    pad = padState(elapsed: 1.0 / 60)
    assert(pad.leftTrigger == 1 && pad.rightTrigger == 0)
    finger.previous = finger.point
    finger.point.y -= rightStick.travel * 8
    touchesMoved([finger], with: nil)
    pad = padState(elapsed: 1.0 / 60)
    assert(pad.rightTrigger == 1 && pad.leftTrigger == 0)
    touchesEnded([finger], with: nil)
    reset(publish: false)
    assert(planeThrottle == 0)
    pressedAt.removeAll()
    lastHeld = []
    updateContext(vehicle: 9, pauseMenu: false)
    layoutIfNeeded()
    assert(
      visible.contains(throttleUp) && visible.contains(yawLeft) && rightStick.idleOrigin == nil)
    throttleUp.setPressed(true)
    yawLeft.setPressed(true)
    pad = padState(elapsed: 1.0 / 60)
    assert(pad.rightTrigger == 1 && pad.padButtons.contains(.l1))
    throttleUp.setPressed(false)
    yawLeft.setPressed(false)
    updateContext(vehicle: 2, pauseMenu: false)
    editing = true
    updateVisibility()
    layoutIfNeeded()
    assert(visible.contains(throttleUp) && rightStick.idleOrigin == nil)
    editing = false
    updateVisibility()
    layoutIfNeeded()
    planeThrottle = 1
    updateGameState(2)
    layoutIfNeeded()
    assert(planeThrottle == 0 && rightStick.idleOrigin == nil)
    updateGameState(0)
    updateContext(vehicle: 2, pauseMenu: true)
    layoutIfNeeded()
    assert(rightStick.idleOrigin == nil && !planeStickEnabled)
    updateContext(vehicle: 2, pauseMenu: false)
    planeThrottle = 1
    GameSettings.shared.airplaneRightStick = false
    updateVisibility()
    layoutIfNeeded()
    assert(planeThrottle == 0 && visible.contains(throttleUp) && rightStick.idleOrigin == nil)
    print("AIRPLANE CONTROL REGRESSION CHECKS PASSED")
    fflush(stdout)
  }
}
