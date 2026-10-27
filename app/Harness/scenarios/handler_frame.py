"""Regression for report 922 (GTA-97): in Scouting the Port the Handler's frame must lower after every checkpoint
retry. Each retry makes a new Handler; before the fix some came out of reused memory unable to lower (frame flag
+155 neither 0 nor 1). Needs the 100% save and `gta.py launch story`; about 6 minutes, most of it Floyd's walk.

    python3 Harness/scenarios/handler_frame.py [RETRIES]
"""
import os, struct, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import gta

def word(address): return struct.unpack("<Q", bytes.fromhex(gta.call("peek %d 8" % address)["hex"]))[0]
def frame_flags():
    # The player's vehicle (ped +5576) keeps its gadgets at +3184; the Handler frame's flags are at +152..+155.
    vehicle = word(word(word(20159720) + 8) + 5576)
    gadget = word(word(vehicle + 3184))
    return bytes.fromhex(gta.call("peek %d 4" % (gadget + 152))["hex"])
def frame_height():
    r = gta.batch("PLAYER_PED_ID; GET_VEHICLE_PED_IS_IN r:0 false; GET_WORLD_POSITION_OF_ENTITY_BONE r:1 17")
    v, at = r[1]["r"]["i"], r[2]["v"]
    return gta.batch("GET_OFFSET_FROM_ENTITY_GIVEN_WORLD_COORDS %d %.3f %.3f %.3f" % (v, *at))[0]["v"][2]

retries = int(sys.argv[1]) if len(sys.argv) > 1 else 5
if "docks_setup" not in [s.lower() for s in gta.call("state")["scripts"]]: gta.command(["mission", "docks_setup"])
in_handler = gta.player()[2] and gta.call("state").get("vehicleModel") == "0x1a7fcefa"
if not in_handler: gta.autopilot("handler over a container", verbose=False)
failures = 0
for attempt in range(retries + 1):
    if attempt or in_handler: gta.retry_checkpoint()
    gta.call("tap @towArmUp 2.5"); time.sleep(3)
    top = frame_height()
    gta.call("tap @towArmDown 2"); time.sleep(2.4)
    bottom = frame_height()
    flags = frame_flags()
    ok = top - bottom > 0.5 and flags[3] in (0, 1)
    failures += not ok
    print("Handler %d: frame %.2f -> %.2f m, flags %s %s" % (attempt, top, bottom, flags.hex(), "ok" if ok else "STUCK"), flush=True)
print("PASSED" if not failures else "FAILED: %d of %d Handlers couldn't lower" % (failures, retries + 1))
sys.exit(1 if failures else 0)
