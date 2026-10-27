// Settings › Cheats: one-off actions (repair, clean, time, weather) and the God mode and Never wanted
// switches. Included by EngineHost.c. The app only queues requests here; pg_native_cheats_tick runs
// them on the game thread during the "main" script's turn (pg_asi_script_tick), where game commands
// are safe. Running them from UIKit or the controller thread races the engine's own updates.
u32 PG_FN_PED_VEHICLE(w2c_game*, u32, u32);
void PG_FN_REPAIR_VEHICLE(w2c_game*, u32);
void PG_FN_VEHICLE_ENGINE_HEALTH(w2c_game*, u32, f32);
void PG_FN_VEHICLE_DIRT(w2c_game*, u32, f32);
void PG_FN_SET_CLOCK(w2c_game*, u32, u32, u32);
void PG_FN_SET_WEATHER(w2c_game*, u64);
void PG_FN_CLEAR_WEATHER(w2c_game*);
void PG_FN_CLEAR_WEATHER_OVERRIDE(w2c_game*);
u32 PG_FN_PLAYER_ID(w2c_game*);
void PG_FN_SET_PLAYER_INVINCIBLE(w2c_game*, u32, u32);
u32 PG_FN_GET_PLAYER_INVINCIBLE(w2c_game*, u32);
void PG_FN_CLEAR_WANTED(w2c_game*, u32);
u32 PG_FN_GET_WANTED(w2c_game*, u32);
void PG_FN_SET_MAX_WANTED(w2c_game*, u32);
u32 PG_FN_GET_MAX_WANTED(w2c_game*);

static pthread_mutex_t cheatMenuLock = PTHREAD_MUTEX_INITIALIZER;
static PGCheatStatus cheatMenuStatus;
static PGCheatCommand cheatMenuCommand;
static int32_t cheatMenuValue;
static double cheatMenuQueuedAt;
static bool cheatGodMode, cheatNeverWanted; // What the game has now: written on the game thread, read under the lock.
static bool cheatGodRequested, cheatWantedRequested; // What the switches show: under the lock.
static unsigned cheatSwitchPending; // Bit 0 god mode, bit 1 never wanted: flipped since the game last applied them. Under the lock.
static u32 cheatPreviousMaxWanted; // The wanted limit before Never wanted, given back when it's turned off. Game thread only.
static const char *const menuWeather[] = {"CLEAR", "EXTRASUNNY", "CLOUDS", "OVERCAST", "RAIN", "THUNDER", "FOGGY"};

bool PGQueueCheat(PGCheatCommand command, int32_t value) {
  if ((unsigned)command > PGCheatNaturalWeather) return false;
  if (command == PGCheatSetTime && (value < 0 || value > 23)) return false;
  if (command == PGCheatSetWeather && (value < 0 || value >= (int32_t)(sizeof menuWeather / sizeof *menuWeather))) return false;
  pthread_mutex_lock(&cheatMenuLock);
  bool accepted = cheatMenuStatus != PGCheatWaiting;
  if (accepted) { cheatMenuCommand = command; cheatMenuValue = value; cheatMenuQueuedAt = timeMS(CLOCK_MONOTONIC); cheatMenuStatus = PGCheatWaiting; }
  pthread_mutex_unlock(&cheatMenuLock);
  return accepted;
}
PGCheatStatus PGGetCheatStatus(void) {
  pthread_mutex_lock(&cheatMenuLock);
  PGCheatStatus status = cheatMenuStatus;
  pthread_mutex_unlock(&cheatMenuLock);
  return status;
}
bool PGCheatEnabled(PGCheatSwitch cheat) {
  pthread_mutex_lock(&cheatMenuLock);
  bool enabled = cheat == PGCheatGodMode ? cheatGodRequested : cheat == PGCheatNeverWanted && cheatWantedRequested;
  pthread_mutex_unlock(&cheatMenuLock);
  return enabled;
}
bool PGSetPersistentCheatEnabled(PGCheatSwitch cheat, bool enabled) {
  if (cheat != PGCheatGodMode && cheat != PGCheatNeverWanted) return false;
  pthread_mutex_lock(&cheatMenuLock);
  if (cheat == PGCheatGodMode) { cheatGodRequested = enabled; cheatSwitchPending |= 1; }
  else { cheatWantedRequested = enabled; cheatSwitchPending |= 2; }
  pthread_mutex_unlock(&cheatMenuLock);
  return true;
}
static void pg_apply_cheat_switch(w2c_game *instance, PGCheatSwitch cheat, bool enabled) {
  if (cheat == PGCheatGodMode) {
    if (enabled != cheatGodMode) PG_FN_SET_PLAYER_INVINCIBLE(instance, PG_FN_PLAYER_ID(instance), enabled);
    pthread_mutex_lock(&cheatMenuLock);
    cheatGodMode = enabled;
    if (!(cheatSwitchPending & 1)) cheatGodRequested = enabled;
    pthread_mutex_unlock(&cheatMenuLock);
  } else {
    if (enabled && !cheatNeverWanted) {
      cheatPreviousMaxWanted = PG_FN_GET_MAX_WANTED(instance);
      PG_FN_SET_MAX_WANTED(instance, 0);
      PG_FN_CLEAR_WANTED(instance, PG_FN_PLAYER_ID(instance));
    } else if (!enabled && cheatNeverWanted && PG_FN_GET_MAX_WANTED(instance) == 0) {
      // Only when the limit is still ours: a mission that set its own limit since then keeps it.
      PG_FN_SET_MAX_WANTED(instance, cheatPreviousMaxWanted);
    }
    pthread_mutex_lock(&cheatMenuLock);
    cheatNeverWanted = enabled;
    if (!(cheatSwitchPending & 2)) cheatWantedRequested = enabled;
    pthread_mutex_unlock(&cheatMenuLock);
  }
}
static void pg_apply_pending_cheat_switches(w2c_game *instance) {
  if (PGPauseMenuActive() || PGGameStatus() != 0 || PGLoadSceneSeconds() != 0 ||
      !PG_FN_PLAYER_PED_ID(instance) || PG_FN_PLAYER_ID(instance) >= 32) return;
  pthread_mutex_lock(&cheatMenuLock);
  unsigned pending = cheatSwitchPending;
  bool god = cheatGodRequested, wanted = cheatWantedRequested;
  cheatSwitchPending = 0;
  pthread_mutex_unlock(&cheatMenuLock);
  if (!pending) return;
  u64 previous = pg_asi_enter_script(instance);
  if (pending & 1) pg_apply_cheat_switch(instance, PGCheatGodMode, god);
  if (pending & 2) pg_apply_cheat_switch(instance, PGCheatNeverWanted, wanted);
  pg_asi_leave_script(instance, previous);
  char line[96];
  snprintf(line, sizeof line, "Cheats: god mode %s, never wanted %s.", cheatGodMode ? "on" : "off", cheatNeverWanted ? "on" : "off");
  pg_log(line);
}
static void pg_native_cheats_action_tick(w2c_game *instance) {
  pthread_mutex_lock(&cheatMenuLock);
  PGCheatStatus status = cheatMenuStatus;
  PGCheatCommand command = cheatMenuCommand;
  int32_t value = cheatMenuValue;
  double queuedAt = cheatMenuQueuedAt;
  pthread_mutex_unlock(&cheatMenuLock);
  if (status != PGCheatWaiting) return;
  // Counted from when it was queued: scripts may not run at all while paused, and an action asked
  // for long ago must not suddenly happen when play resumes.
  if (timeMS(CLOCK_MONOTONIC) - queuedAt >= 10000) status = PGCheatNotReady;
  else if (PGPauseMenuActive() || PGGameStatus() != 0 || PGLoadSceneSeconds() != 0) return;
  else {
    u64 previous = pg_asi_enter_script(instance);
    // The weather name goes on this thread's own engine stack, freed again below.
    u64 stack = instance->w2c_g0;
    instance->w2c_g0 -= 32;
    status = PGCheatSucceeded;
    switch (command) {
      case PGCheatRepairVehicle:
      case PGCheatCleanVehicle: {
        u32 ped = PG_FN_PLAYER_PED_ID(instance);
        u32 vehicle = ped ? PG_FN_PED_VEHICLE(instance, ped, 0) : 0;
        if (!vehicle) { status = PGCheatNoVehicle; break; }
        if (command == PGCheatRepairVehicle) {
          PG_FN_VEHICLE_ENGINE_HEALTH(instance, vehicle, 1000.f);
          PG_FN_REPAIR_VEHICLE(instance, vehicle);
        }
        PG_FN_VEHICLE_DIRT(instance, vehicle, 0.f);
        break;
      }
      case PGCheatSetTime: PG_FN_SET_CLOCK(instance, (u32)value, 0, 0); break;
      case PGCheatSetWeather:
        snprintf(pg_pointer(&env, instance->w2c_g0, 32), 32, "%s", menuWeather[value]);
        PG_FN_CLEAR_WEATHER_OVERRIDE(instance);
        PG_FN_SET_WEATHER(instance, instance->w2c_g0);
        break;
      case PGCheatNaturalWeather:
        PG_FN_CLEAR_WEATHER_OVERRIDE(instance);
        PG_FN_CLEAR_WEATHER(instance);
        break;
    }
    instance->w2c_g0 = stack;
    pg_asi_leave_script(instance, previous);
  }
  char line[96];
  snprintf(line, sizeof line, "Cheats: action %d (value %d) finished with status %d.", command, value, status);
  pg_log(line);
  pthread_mutex_lock(&cheatMenuLock);
  cheatMenuStatus = status;
  pthread_mutex_unlock(&cheatMenuLock);
}

static void pg_native_cheats_tick(w2c_game *instance) {
  pg_native_cheats_action_tick(instance);
  pg_apply_pending_cheat_switches(instance);
  if ((!cheatGodMode && !cheatNeverWanted) || PGPauseMenuActive() ||
      PGGameStatus() != 0 || PGLoadSceneSeconds() != 0) return;
  u64 previous = pg_asi_enter_script(instance);
  u32 player = PG_FN_PLAYER_ID(instance);
  if (player < 32 && PG_FN_PLAYER_PED_ID(instance)) {
    // Asks for the current player every time: a character switch or a respawn makes a new one, and
    // scripts can take invincibility away or raise the wanted limit. Writes only what changed.
    if (cheatGodMode && !PG_FN_GET_PLAYER_INVINCIBLE(instance, player))
      PG_FN_SET_PLAYER_INVINCIBLE(instance, player, 1);
    if (cheatNeverWanted) {
      u32 limit = PG_FN_GET_MAX_WANTED(instance);
      if (limit != 0) {
        cheatPreviousMaxWanted = limit;
        PG_FN_SET_MAX_WANTED(instance, 0);
      }
      if (PG_FN_GET_WANTED(instance, player) != 0) PG_FN_CLEAR_WANTED(instance, player);
    }
  }
  pg_asi_leave_script(instance, previous);
}
