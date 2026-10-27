"""Regression for GTA-107 and GTA-113: a mission failed by the player's death shows Retry/Exit (Accept/Back, no switch
button), Retry restarts the mission, and leaving the replay brings the game back in seconds instead of hanging on
"Loading Story Mode". Needs the 100% save and `gta.py launch story`; about 3 minutes.

    python3 Harness/scenarios/fail_prompt_and_exit.py
"""
import math, os, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import gta

def check(ok, what):
    print(("ok: " if ok else "FAILED: ") + what, flush=True)
    if not ok: sys.exit(1)
def replay_state(): return gta.call("gget %d 1" % gta.REPLAY_STATE)["values"][0]["i"]
def die():
    gta.batch("PLAYER_PED_ID; SET_ENTITY_HEALTH r:0 0 0")
    for _ in range(60):
        time.sleep(0.5)
        if replay_state() == 1: break
    time.sleep(2.5)

if "franklin0" not in [s.lower() for s in gta.call("state")["scripts"]]: gta.command(["mission", "franklin0"])
for _ in range(80):
    s = gta.call("state")
    if s["status"] & 4: gta.batch("STOP_CUTSCENE_IMMEDIATELY")
    if not s["status"] & 5: break
    time.sleep(1)
die()
ui = gta.call("ui")
names = [b["name"] for b in ui["visible"]]
check(ui["prompt"] and "accept" in names and "back" in names, "Mission failed shows Accept/Back")
check("switchCharacter" not in names, "and no switch button")
gta.call("tap @accept 0.2")
for _ in range(40):
    time.sleep(1)
    if replay_state() != 1 and not gta.call("state")["status"] & 1: break
check("franklin0" in [s.lower() for s in gta.call("state")["scripts"]], "Retry restarted the mission")
time.sleep(5)
die()
gta.call("tap @back 0.2"); time.sleep(2); gta.call("tap @accept 0.2")   # Exit, then Yes
start = time.time()
while time.time() - start < 90:
    s = gta.call("state")
    if math.hypot(*s.get("pos", [0, 0, 0])[:2]) > 50 and not s["status"] & 17 and "mission_repeat_controller" not in s["scripts"]: break
    time.sleep(2)
took = time.time() - start
check(took < 90, "leaving the replay came back to free roam in %.0f s" % took)
print("PASSED")
