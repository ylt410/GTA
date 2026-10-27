"""GTA-89: "Invalid call_indirect" right after a live resize (a resolution change in Settings, or a TV). Drives through
city traffic while the back buffer is resized every couple of seconds, and fails if the game stops. Needs `gta.py
launch story`; up to SECONDS (default 300).

    python3 Harness/scenarios/resize_stress.py [SECONDS] [GAP]
"""
import os, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import gta

seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 300
gap = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0   # seconds between resizes
for _ in range(60):
    s = gta.call("state")
    if s.get("pos") and not s["status"] & 16: break
    gta.dismiss_autosave(); time.sleep(1)
gta.dismiss_autosave()
gta.teleport(195.0, -933.0, 30.5)   # Legion Square: traffic, pedestrians, the busiest render
time.sleep(4)
gta.batch("REQUEST_MODEL h:buffalo")
for _ in range(40):
    if gta.batch("HAS_MODEL_LOADED h:buffalo")[0]["r"]["i"] & 1: break
    time.sleep(0.25)
gta.batch("PLAYER_PED_ID; GET_ENTITY_COORDS r:0 true; CREATE_VEHICLE h:buffalo 200.0 -930.0 30.5 0.0 false false false; SET_PED_INTO_VEHICLE r:0 r:2 -1;"
          " TASK_VEHICLE_DRIVE_WANDER r:0 r:2 25.0 786603")
log = gta.log_path(); start, n = time.time(), 0
sizes = [(960, 444), (1280, 592), (720, 333), (1440, 666)]
while time.time() - start < seconds:
    w, h = sizes[n % len(sizes)]; n += 1
    try: reply = gta.call("resize %d %d" % (w, h))
    except Exception as error: print("FAILED: the game stopped after %d resizes (%s)" % (n, error)); break
    if not reply.get("ok"): print("FAILED: resize refused: %s" % reply); break
    before = gta.call("state")["frames"]
    time.sleep(gap)
    if gta.call("state")["frames"] <= before: print("FAILED: the game stopped drawing after %d resizes (hang)" % n); break
    text = open(log, errors="replace").read()[-20000:]
    if "Invalid call_indirect" in text or "Native trap" in text or "stopped:" in text:
        print("FAILED: the engine trapped after %d resizes:" % n); print("\n".join(l for l in text.splitlines() if "trap" in l or "call_indirect" in l or "stopped" in l)[-1500:]); break
else:
    restores = open(log, errors="replace").read().count("FINISHED DEVICE RESTORE")
    print("PASSED: %d live resizes (%d device restores) in %.0f s while driving" % (n, restores, time.time() - start))
