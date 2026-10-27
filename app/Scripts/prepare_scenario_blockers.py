"""Fix a crash when pedestrians spawn at a crowded scenario point.

CScenarioManager::PerformCollisionChecksForScenarioPoint (f72580) zeroes a list of 6 entity
pointers and asks CPedPlacement::IsPositionClearForPed (f41459) for up to 6 blocking entities,
then walks the list until it reaches an empty slot. When all 6 slots are filled there is no empty
slot, and the walk reads past the end and uses whatever follows as an entity. Asking for at most 5
keeps the last slot empty. (Found by the Muguet project; this is our own patch.)
"""
from pathlib import Path
from engine_names import fn
root = Path(__file__).resolve().parents[1]
SCENARIO = fn('CScenarioManager::PerformCollisionChecksForScenarioPoint(CScenarioManager::CollisionCheckParams const&)')
IS_CLEAR = fn('CPedPlacement::IsPositionClearForPed(rage::Vector3 const&, float, bool, int, CEntity**, bool, bool, bool)')
OLD = f'  var_i2 = 6u;\n  var_j3 = var_l1;\n  var_i0 = w2c_game_f{IS_CLEAR}(instance, var_j0, var_f1, var_i2, var_j3);\n'
NEW = OLD.replace('var_i2 = 6u;', 'var_i2 = 5u; /* scenario blockers: keep the 6th slot empty */')
for path in sorted((root / 'Generated').glob('game_*.c')):
    s = path.read_text()
    start = s.find(f'\nu32 w2c_game_f{SCENARIO}(w2c_game* instance, u64 var_p0) {{\n')
    if start < 0: continue
    end = s.index('\n}\n', start)
    body = s[start:end]
    if NEW in body: print('scenario blockers: already patched'); break
    if body.count(OLD) != 1: raise RuntimeError(f'Pinned f{SCENARIO} (PerformCollisionChecksForScenarioPoint) changed')
    path.write_text(s[:start] + body.replace(OLD, NEW) + s[end:])
    print(f'scenario blockers: patched {path.name}')
    break
else:
    raise RuntimeError(f'f{SCENARIO} (PerformCollisionChecksForScenarioPoint) not found')
