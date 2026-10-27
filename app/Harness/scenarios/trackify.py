"""GTA-134: the port-made Trackify in The Merryweather Heist (Offshore, docks_heistB). Starts the heist from the
planning board (browse, then confirm), jumps to checkpoint 3 (Trevor in the submersible off Paleto Bay; the phone
opens Trackify by itself), and checks that the port's Trackify runs ("Trackify: opened" in the log) with the
mission's target (Global 22831, the device about 670 m north, 180 m down). Then it drives the sub at the target
and checks that the distance falls. Needs the 100% save and `gta.py launch story`; about 4 minutes.

    python3 Harness/scenarios/trackify.py [--stay]   (--stay: stop once Trackify is open)
"""
import math, os, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import gta

TARGET, PHONE_STATE = 22831, 20384
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "trackify-out")
os.makedirs(OUT, exist_ok=True)

def check(ok, what):
    print(("ok: " if ok else "FAILED: ") + what, flush=True)
    if not ok: gta.shot(os.path.join(OUT, "failed.png")); sys.exit(1)
def g(index): return gta.call("gget %d 1" % index)["values"][0]
def heist(): return [x for x in gta.call("state")["scripts"] if x.lower() in ("docks_heista", "docks_heistb")]
def log_has(text, since):
    with open(gta.log_path(), "rb") as f:
        f.seek(since); return text.encode() in f.read()

gta.dismiss_autosave()
if not heist():
    for index, value in ((gta.REPEAT_PLAY, 75), (gta.REPEAT_PLAY + 1, 1), (gta.REPEAT_PLAY + 2, 5)): gta.call("gset %d %d" % (index, value))
    end = time.time() + 90
    while time.time() < end and "heist_ctrl_docks" not in gta.call("state")["scripts"]:
        if gta.call("state")["status"] & 8: gta.call("tap @accept 0.15")
        time.sleep(1)
    # The board takes input once its intro has settled: browse to the approaches, then confirm; again if needed.
    for attempt in range(3):
        if heist(): break
        time.sleep(8)
        for button in ("down", "right", "down"): gta.call("pad %s 0.2" % button); time.sleep(2)
        for _ in range(6):
            if heist(): break
            gta.call("pad a 0.2"); time.sleep(4)
check(heist() == ["docks_heistB"], "the Offshore approach (docks_heistB) is running")
since = os.path.getsize(gta.log_path())
gta.command(["checkpoint", "3"])
for _ in range(40):
    state = gta.call("state")
    if state["status"] & 4: gta.call("pad a 0.3")
    if state["vehicleModel"] == "0x2dff622f" and g(PHONE_STATE)["i"] == 7: break
    time.sleep(1)
time.sleep(3)
target = [g(TARGET + k)["f"] for k in range(3)]
check(target[2] < -50 and abs(target[1]) > 1000, "the mission's Trackify target is set: %s" % [round(v, 1) for v in target])
check(log_has("Trackify: opened", since), "the port's Trackify opened with the phone")
gta.shot(os.path.join(OUT, "trackify.png"))
if "--stay" in sys.argv: print("Trackify open"); sys.exit(0)
def distance():
    _, at, _ = gta.player(); return math.dist(at[:2], target[:2])
start = distance()
# Head for the target: turn the sub to face it, then full ahead for 15 s.
_, at, sub = gta.player()
heading = math.degrees(math.atan2(-(target[0] - at[0]), target[1] - at[1]))
gta.batch("SET_ENTITY_HEADING %d %.1f" % (sub, heading))
for _ in range(15): gta.call("pad - 1.1 0 0 0 0 0 1"); time.sleep(1)
check(distance() < start - 20, "driving at it closes the distance (%.0f m -> %.0f m)" % (start, distance()))
gta.shot(os.path.join(OUT, "trackify-closer.png"))
print("PASSED")
