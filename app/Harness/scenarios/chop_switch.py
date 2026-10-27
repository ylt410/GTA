"""Regression for report 956 (GTA-81): in Chop the camera switches to Chop's point of view with player control off,
and the touch controls must offer the switch button to get back to Franklin. Needs the 100% save and
`gta.py launch story`; about 1 minute.

    python3 Harness/scenarios/chop_switch.py
"""
import os, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import gta

def check(ok, what):
    print(("ok: " if ok else "FAILED: ") + what, flush=True)
    if not ok: sys.exit(1)

if "franklin0" not in [s.lower() for s in gta.call("state")["scripts"]]: gta.command(["mission", "franklin0"])
gta.command(["checkpoint", "2"])   # the rail yard: "SWITCH to Chop to see from his point of view"
for _ in range(40):
    s = gta.call("state")
    if s["status"] & 4: gta.batch("STOP_CUTSCENE_IMMEDIATELY")
    if "switch" in (s.get("help") or "").lower(): break
    time.sleep(1)
check("switch" in (gta.call("state").get("help") or "").lower(), "the mission asks to switch to Chop")
gta.call("tap @switchCharacter 0.15"); time.sleep(5)
ui = gta.call("ui")
check(ui["prompt"], "in Chop's view the prompt layout shows (control off)")
check("switchCharacter" in [b["name"] for b in ui["visible"]], "the switch button is there")
gta.call("tap @switchCharacter 0.15")
for _ in range(10):
    time.sleep(1)
    if not gta.call("ui")["prompt"]: break
check(not gta.call("ui")["prompt"] and not gta.call("state")["status"] & 1, "back to Franklin with control on")
print("PASSED")
