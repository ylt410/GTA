"""GTA-143: Nervous Ron (trevor2), the sniper section with real touch shots only. From checkpoint 0 (Trevor's
perch): Aim held, the + zoom held to FOV 5, Ron watched, the biker shot, then after the truck parks and Ron says
"shoot the lights" (TR2_SHOOTL) both prop_wall_light_02a lights shot. Passes when both count as broken and Ron's
TR2_LIGHTS line plays. Zoomed out (FOV above about 16) the lights don't exist and shots pass through them.
Needs the 100% save: `gta.py launch story --root <save>`, `gta.py mission trevor2`, `gta.py checkpoint 0`.

    python3 Harness/scenarios/nervous_ron_lights.py
"""
import math, os, re, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import gta

LIGHTS = [(1700.91, 3295.05, 45.89), (1697.82, 3293.07, 47.24)]
T0 = time.time()

def say(*a): print("%5.1f" % (time.time() - T0), *a, flush=True)

def r(text):
    return gta.batch(text)

def i(res): return res["r"]["i"] if res.get("ok") else None

def touch(*words): gta.command(["touch", *map(str, words)])

def aim_at(x, y, z):
    cam = r("GET_FINAL_RENDERED_CAM_COORD")[0]["v"]
    dx, dy, dz = x - cam[0], y - cam[1], z - cam[2]
    h = math.degrees(math.atan2(-dx, dy)) % 360
    p = math.degrees(math.atan2(dz, math.hypot(dx, dy)))
    r("PLAYER_PED_ID; SET_ENTITY_HEADING r:0 %.3f; SET_GAMEPLAY_CAM_RELATIVE_HEADING 0.0; SET_GAMEPLAY_CAM_RELATIVE_PITCH %.3f 1.0" % (h, p))
    time.sleep(0.5)

def fire():
    touch(2, "down", "@fire"); time.sleep(0.2); touch(2, "up"); time.sleep(1.2)

def mission_blips():
    return [b for b in gta.blips(sprites=[1], each=8) if abs(b["at"][0] - 1690) < 200 and abs(b["at"][1] - 3320) < 200]

def coords(e): return r("GET_ENTITY_COORDS %d true" % e)[0]["v"]

def log_lines(start):
    with open(gta.log_path(), errors="replace") as f:
        f.seek(start); text = f.read(); return f.tell(), text

def failed():
    return gta.call("state").get("status", 0) & 1 and "Mission failed" in open(gta.log_path(), errors="replace").read()[-4000:]

ron_model = i(r("GET_HASH_KEY s:ig_nervousron")[0])
touch(1, "down", "@aim"); time.sleep(1.5)
touch(2, "down", "@zoomIn"); time.sleep(4); touch(2, "up")
say("FOV", r("GET_GAMEPLAY_CAM_FOV")[0]["r"]["f"])

ron = None
for b in mission_blips():
    if b["entity"] and i(r("GET_ENTITY_MODEL %d" % b["entity"])[0]) == ron_model: ron = b["entity"]
say("ron", ron)
x, y, z = coords(ron); aim_at(x, y, z + 0.3)

# Bikers: blipped peds that aren't Ron, until the light blips show.
deadline = time.time() + 240
lights_blipped = False
while time.time() < deadline and not lights_blipped:
    bl = mission_blips()
    for b in bl:
        e = b["entity"]
        if not e or e == ron: continue
        if i(r("IS_ENTITY_A_PED %d" % e)[0]) != 1 or i(r("IS_ENTITY_DEAD %d false" % e)[0]) == 1: continue
        if i(r("GET_ENTITY_MODEL %d" % e)[0]) == ron_model: continue
        x, y, z = coords(e); aim_at(x, y, z + 0.5); fire()
        say("shot biker", e, "health now", i(r("GET_ENTITY_HEALTH %d" % e)[0]))
    lights_blipped = sum(1 for b in bl if b["colour"] == 2 and abs(b["at"][0] - 1699) < 5) >= 1
    if not lights_blipped:
        x, y, z = coords(ron); aim_at(x, y, z + 0.3); time.sleep(2)
say("light blips", lights_blipped)

# Wait for Ron to hide (the truck has parked, TR2_DRIVEIN), then for the next conversation line (Ron's "shoot the lights").
pos, _ = log_lines(0)
hid = False
for _ in range(90):
    if i(r("IS_ENTITY_PLAYING_ANIM %d s:misstrevor2ig_4 s:hide 3" % ron)[0]) == 1: hid = True; break
    time.sleep(1)
say("ron hiding", hid)
pos, _ = log_lines(0)
contexts = []
for _ in range(40):
    pos, text = log_lines(pos)
    for m in re.finditer(r"Context: (TR2_[A-Z]+) Subtitle: (\S+)", text):
        if not contexts or contexts[-1][0] != m.group(1): contexts.append((m.group(1), m.group(2)))
    if len(contexts) >= 2: break
    time.sleep(1)
for c, sub in contexts:
    say("line", c, r("GET_FILENAME_FOR_AUDIO_CONVERSATION s:%s" % sub)[0].get("s"))

# Both lights.
for n, (lx, ly, lz) in enumerate(LIGHTS):
    for attempt in range(3):
        aim_at(lx, ly, lz + 0.03)
        exists = i(r("DOES_OBJECT_OF_TYPE_EXIST_AT_COORDS %f %f %f 1.0 h:prop_wall_light_02a false" % (lx, ly, lz))[0])
        fire()
        broken = i(r("HAS_CLOSEST_OBJECT_OF_TYPE_BEEN_BROKEN %f %f %f 1.0 h:prop_wall_light_02a true" % (lx, ly, lz))[0])
        say("light", n + 1, "attempt", attempt + 1, "exists", exists, "broken", broken)
        if broken == 1: break
    st = gta.call("state")
    if st.get("status", 0) & 1: say("control off after light", n + 1, "status", st.get("status")); break

time.sleep(12)
_, text = log_lines(0)
passed = "Subtitle: TR2_LIGHTS" in text[-30000:] and "Mission failed" not in text[-6000:]
say("ok: both lights out, Ron's TR2_LIGHTS line played" if passed else "FAILED: no TR2_LIGHTS line (or Mission failed)")
sys.exit(0 if passed else 1)
