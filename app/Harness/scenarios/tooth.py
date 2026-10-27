"""GTA-94: By the Book's torture, on touch. The tool cart keeps the D-pad (its help offers the stick or the D-pad),
the pliers are picked with the arrows and Accept, "Hold RT to grip a tooth" shows an RT button, and at "Rotate
~PAD_RSTICK_ROTATE~ to pull it out" RT stays on screen and held while a second finger circling on the right side
pulls the tooth. Needs the 100% save and `gta.py launch story`; about 4 minutes.

    python3 Harness/scenarios/tooth.py
"""
import math, os, re, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import gta

def check(ok, what):
    print(("ok: " if ok else "FAILED: ") + what, flush=True)
    if not ok: gta.shot(os.path.join(os.path.dirname(os.path.abspath(__file__)), "tooth-failed.png")); sys.exit(1)
def buttons(): return [b["name"] for b in gta.call("ui")["visible"]]
def at(name): return next(b["at"] for b in gta.call("ui")["visible"] if b["name"] == name)
def wait_help(pattern, timeout=30):
    end = time.time() + timeout
    while time.time() < end:
        if gta.call("gget %d 1" % gta.REPLAY_STATE)["values"][0]["i"] == 1:
            # Mission failed ("Dave was abandoned": the autopilot can follow the GPS marker to Michael's house):
            # Retry restarts in the torture room.
            time.sleep(2); gta.call("tap @accept 0.2"); time.sleep(5); end += 30; continue
        help = gta.call("state").get("help") or ""
        if re.search(pattern, help, re.I): return help
        time.sleep(0.5)
    return gta.call("state").get("help") or ""

gta.dismiss_autosave()
if "fbi3" not in [s.lower() for s in gta.call("state")["scripts"]]: gta.command(["mission", "fbi3"])
gta.autopilot(r"pick a torture weapon|Pick a weapon from the cart|ECG monitor|adrenaline", timeout=600)
help = wait_help(r"pick a torture weapon", 60)
check("pick a torture weapon" in help, "the tool cart asks to pick a weapon")
time.sleep(1)
check({"left", "right", "accept"} <= set(buttons()), "the cart keeps the D-pad arrows and Accept")
gta.call("tap @right 0.2"); time.sleep(2)   # the cart starts on the jerry can; one step right is the pliers
gta.call("tap @accept 0.2")
help = wait_help(r"grip a tooth", 30)
check("grip a tooth" in help, "the pliers were picked: \"Hold RT to grip a tooth\"")
time.sleep(1)
check("script_rt" in buttons(), "an RT button is there")
rt = at("script_rt")
gta.call("touch 1 down %.3f %.3f" % (rt[0], rt[1]))   # a thumb holds RT, as a player does
help = wait_help(r"rotate", 10)
check("rotate" in help.lower(), "gripped: \"Rotate ... to pull it out\"")
time.sleep(1)
check("script_rt" in buttons(), "RT stays on screen while rotating")
# A free spot on the right (not on Accept/Back/Option), circled fast: the scene measures the turn between frames
# and counts only a quick rotation (about 3-4 turns a second, as a thumb spins), one direction.
cx, cy, rx, ry = 0.62, 0.5, 0.05, 0.11
pulled = False
for direction in (-1, 1):
    gta.call("touch 2 down %.3f %.3f" % (cx, cy)); gta.call("touch 2 move %.3f %.3f" % (cx + rx, cy))
    t, k = time.time(), 0
    while time.time() - t < 8 and not pulled:
        k += 1; a = direction * k * math.pi / 4
        gta.call("touch 2 move %.3f %.3f" % (cx + rx * math.cos(a), cy + ry * math.sin(a))); time.sleep(0.03)
        if k % 8 == 0:
            state = gta.call("state"); help = state.get("help") or ""
            # Pulled: the help goes away or moves on, then the scene's cutscene.
            if (help and not re.search(r"rotate|grip a tooth", help, re.I)) or state["status"] & 4 or (not help and k > 16): pulled = True
    gta.call("touch 2 up")
    if pulled: print("pulled turning %s" % ("one way" if direction < 0 else "the other way"), flush=True); break
    time.sleep(1)
gta.call("touch 2 up"); gta.call("touch 1 up")
check(pulled, "rotating on the right side pulled the tooth (the help moved on: %r)" % help[:80])
print("PASSED")
