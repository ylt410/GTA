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
    helpContext = false, helpShown = false, passenger = false, taxiMenu = false,
    radioWheelOpen = false
  var specialFill: Float = 0
  var vehicleModel: UInt32 = 0
}
func PGClearCameraSwipe() {}
func PGQueueCameraSwipe(_ x: Float, _ y: Float) {}
func PGMouseInput(_ x: Int32, _ y: Int32, _ z: Int32) {}
func PGNextAircraftWeapon() {}
func PGSetRadioWheelDirection(_ x: Float, _ y: Float, _ active: Bool) {}
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
    updateContext(vehicle: 2, pauseMenu: false)
    layoutIfNeeded()
    assert(
      visible.contains(throttleUp) && visible.contains(yawLeft) && rightStick.idleOrigin == nil)
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
    var pad = padState(elapsed: 1.0 / 60)
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
