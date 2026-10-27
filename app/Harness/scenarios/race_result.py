"""GTA-98: the Stock Car Race (Country_Race) result screen, "1st Place ... Retry X / Continue A". The race is
started directly and won by putting the car on each checkpoint in order (the current yellow marker, then the
one that was "next"; the finish is the chequered flag, sprite 38). Then Pause must open over the result screen,
and the touch Accept (Continue) must leave it. Needs the 100% save and `gta.py launch story`; about 4 minutes.

    python3 Harness/scenarios/race_result.py [--retry | --stay]

--retry presses Retry (X) instead of Continue; --stay stops on the result screen and presses nothing. Both modes
first check that Pause opens over the result screen and closes back to it.
"""
import math, os, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import gta

GRID, RACE = (1967.0, 3116.0, 46.0), "Country_Race"

def check(ok, what):
    print(("ok: " if ok else "FAILED: ") + what, flush=True)
    if not ok: sys.exit(1)
def running(): return gta.batch("GET_NUMBER_OF_THREADS_RUNNING_THE_SCRIPT_WITH_THIS_HASH h:%s" % RACE)[0]["r"]["i"]
def yellows(): return [tuple(b["at"]) for b in gta.blips(sprites=[1], each=16) if b["colour"] == 5]
def same(a, b): return math.hypot(a[0] - b[0], a[1] - b[1]) < 3
def place(vehicle, at, heading):
    gta.batch("SET_ENTITY_COORDS %d %.1f %.1f %.1f false false false false; SET_ENTITY_HEADING %d %.1f; SET_VEHICLE_FORWARD_SPEED %d 12.0"
              % (vehicle, at[0], at[1], at[2] + 0.6, vehicle, heading, vehicle))

def start_race():
    for _ in range(120):
        s = gta.call("state")
        if s.get("pos") and not s["status"] & 16: break
        gta.dismiss_autosave(); time.sleep(1)
    if running():
        gta.batch("TERMINATE_ALL_SCRIPTS_WITH_THIS_NAME s:%s" % RACE)
        while running(): time.sleep(1)
    _, _, vehicle = gta.player()
    if not vehicle:
        gta.teleport(*GRID)
        gta.batch("REQUEST_MODEL h:stalion2")
        for _ in range(40):
            if gta.batch("HAS_MODEL_LOADED h:stalion2")[0]["r"]["i"] & 1: break
            time.sleep(0.25)
        r = gta.batch("CREATE_VEHICLE h:stalion2 %.1f %.1f %.1f 0.0 false false false; PLAYER_PED_ID; SET_PED_INTO_VEHICLE r:1 r:0 -1"
                      % (GRID[0] + 6, GRID[1], GRID[2]))
        check(r[0]["r"]["i"] != 0, "a car to race in")
    gta.batch("REQUEST_SCRIPT s:%s" % RACE)
    for _ in range(60):
        if gta.batch("HAS_SCRIPT_LOADED s:%s" % RACE)[0]["r"]["i"] & 1: break
        time.sleep(0.5)
    gta.batch("START_NEW_SCRIPT s:%s 53000; SET_SCRIPT_AS_NO_LONGER_NEEDED s:%s" % (RACE, RACE))
    # The intro (grid camera, countdown) takes control away; the race is on when control comes back with markers.
    for _ in range(120):
        time.sleep(1)
        status = gta.call("state")["status"]
        if status & 8: gta.dismiss_autosave(); continue
        if not status & 1 and len(yellows()) == 2: return
    check(False, "the race started")

def drive():
    """Each checkpoint in order, then the finish line. True when the race hands control away (the result)."""
    ys = yellows(); _, at, _ = gta.player()
    current, passed, end = min(ys, key=lambda p: math.hypot(p[0] - at[0], p[1] - at[1])), 0, time.time() + 300
    while time.time() < end:
        if gta.call("state")["status"] & 8: gta.dismiss_autosave(); continue   # the save prompt can come mid-race
        _, at, vehicle = gta.player()
        ys = yellows()
        if not ys:
            flag = [b for b in gta.blips(sprites=[38], each=4) if math.hypot(b["at"][0] - GRID[0], b["at"][1] - GRID[1]) < 400]
            if flag:
                place(vehicle, flag[0]["at"], gta.batch("GET_ENTITY_HEADING %d" % vehicle)[0]["r"]["f"])
                for _ in range(20):
                    time.sleep(0.5)
                    if gta.call("state")["status"] & 1: print("finished after %d checkpoints" % passed, flush=True); return True
            time.sleep(0.5); continue
        if not any(same(current, y) for y in ys): current = ys[0]
        following = next((y for y in ys if not same(y, current)), current)
        heading = math.degrees(math.atan2(-(following[0] - current[0]), following[1] - current[1]))
        place(vehicle, current, heading)
        for _ in range(20):
            time.sleep(0.3)
            if gta.call("state")["status"] & 8: break
            ys = yellows()
            if not any(same(current, y) for y in ys): break
        else: continue
        passed += 1
        current = next((y for y in ys if same(y, following)), ys[0] if ys else current)
    return False

start_race()
check(drive(), "the race finished")
for _ in range(30):
    time.sleep(1)
    if gta.call("ui")["prompt"]: break
time.sleep(4)   # the result screen's scale-in
ui = gta.call("ui")
check(ui["prompt"] and "accept" in [b["name"] for b in ui["visible"]], "the result screen shows Accept")
if "--stay" in sys.argv: print("on the result screen"); sys.exit(0)
# Pause opens over the result screen (the report: "Pause doesn't open either") and closes back to it. The pause
# menu doesn't set the paused status bit here; the touch controls' "menu" context follows it.
def pause_menu(): return gta.batch("IS_PAUSE_MENU_ACTIVE")[0]["r"]["i"] & 1
gta.call("tap @pause 0.25")
for _ in range(20):
    time.sleep(0.5)
    if pause_menu() and gta.call("ui")["context"] == "menu": break
check(pause_menu() and gta.call("ui")["context"] == "menu", "Pause opened over the result screen")
time.sleep(2); gta.call("tap @pause 0.25")
for _ in range(20):
    time.sleep(0.5)
    if not pause_menu(): break
time.sleep(2)
check(not pause_menu() and gta.call("ui")["prompt"], "closing it went back to the result screen")
if "--retry" in sys.argv:
    gta.call("tap @prompt_option 0.25")   # Retry (X)
    for _ in range(90):
        time.sleep(1)
        status = gta.call("state")["status"]
        if status & 8: gta.dismiss_autosave(); continue
        if not status & 1 and running() and len(yellows()) == 2: break
    check(running() and len(yellows()) == 2 and not gta.call("state")["status"] & 1, "Retry (X) started the race again")
else:
    gta.call("tap @accept 0.25")   # Continue (A)
    for _ in range(20):
        time.sleep(1)
        if not gta.call("state")["status"] & 1: break
    check(not gta.call("state")["status"] & 1, "Continue (Accept) left the result screen")
print("PASSED")
