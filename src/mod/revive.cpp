#include "mod/revive.h"

#include <windows.h>

#include <atomic>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <cmath>
#include <cstring>
#include <string>

#include "common/log.h"
#include "mod/engine.h"
#include "mod/game.h"
#include "mod/hud.h"
#include "mod/netmenu.h"
#include "mod/screenshot.h"
#include "mod/link.h"
#include "mod/net.h"
#include "mod/patch.h"
#include "mod/players.h"

namespace revive {
namespace {

constexpr size_t kFloor = 0x418;       // entity + : float, the health a hit does not take below (-1 = none)
constexpr size_t kFloorBits = 0x420;   // entity + : u32; bit 1 lets a blow (hit kind 2) through the floor
constexpr float kDownHealth = 0.01f;   // the health of a player who is down
constexpr float kRaisedHealth = 0.35f; // ... and of one a teammate raised
constexpr float kReach = 2.2f;         // metres: how near a teammate must be to raise
constexpr uint64_t kRaiseMs = 3000;    // how long "use" is held to raise
constexpr uint64_t kAllDownMs = 2500;  // every player down this long: the last checkpoint, for everybody
constexpr int kSlots = 32;
constexpr int kUseAction = 35;         // the game's input action "use" (the pad's X; the lab, 2026-11-25)
// The player's input flags (player + 0x134C: the game's input handler exe+289F40 keeps them at its object +1324,
// the object being player + 3616): 0x10 - crouching (actions 12 / 13 / 14: hold, toggle, let go). A fallen player
// is held crouching: lower for their own eyes, and their figure crouches for the others.
constexpr size_t kInputFlags = 0x134C;
constexpr uint32_t kCrouch = 0x10;

Config g_cfg;
std::atomic<bool> g_down[kSlots] = {};     // who is down (this PC's own slot too)
uint64_t g_down_since[kSlots] = {};

// this PC's own player
uint8_t* g_body = nullptr;                 // the body whose floor the mod set (this PC's own, as an object:
                                           // an id is given again at once to another entity of a new level)
float g_old_floor = -1.f;
uint32_t g_old_bits = 0;
bool g_floor_set = false;
uint64_t g_raise_started = 0;              // when "use" next to a fallen teammate began
int g_raising = -1;                        // ... and whom
int g_raised_by = -1;                      // who raises this PC's fallen player now (their word), and since when
uint64_t g_raised_since = 0;
bool g_crouched = false;                   // the mod holds this PC's player crouching
uint64_t g_last_reload = 0;

std::atomic<bool> g_use_held{false};
std::atomic<uint64_t> g_lab_use_until{0};  // the lab: "use" held without a pad until then
std::atomic<uint64_t> g_lab_actions_until{0};

#pragma pack(push, 1)
struct DownNote {
  uint8_t slot;
  uint8_t down;  // 1: down, 0: up again
};
struct RaiseNote {
  uint8_t slot;  // the one raised
  uint8_t by;    // the one who raised
};
struct RaisingNote {
  uint8_t slot;  // the one being raised
  uint8_t by;
  uint8_t on;    // 1: holds Use over them now, 0: let go
};
#pragma pack(pop)

// --- the player's input actions (exe+4F8D30: the inventory of the player this PC controls gets them) ---------
using ActionFn = char (*)(uint8_t*, int, char);
ActionFn g_action = nullptr;

char on_action(uint8_t* inventory, int action, char flags) {
  char r = g_action(inventory, action, flags);
  if (action == kUseAction && inventory && *reinterpret_cast<uintptr_t*>(inventory) == game::at(game::kPlayerInventory)) {
    if (flags & 1) g_use_held.store(true);
    if (flags & 2) g_use_held.store(false);
  }
  if (GetTickCount64() < g_lab_actions_until.load()) LOGI("lab: input action %d, flags %d -> %d", action, flags, r);
  return r;
}

// --- a fallen player does not walk, jump, crouch, fire or use: the input is held back ------------------------
// The keyboard and the mouse come through DirectInput 8 (the system's dinput8.dll behind the mod's loader): every
// device of one interface kind shares one table of methods, so the methods "get the state" (vtbl 9) and "get the
// buffered data" (vtbl 10) of a keyboard made here are those of the game's devices too. The pad: XInputGetState.
std::atomic<bool> g_me_down{false};

using GetStateFn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, void*);
using GetDataFn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, DIDEVICEOBJECTDATA*, DWORD*, DWORD);
GetStateFn g_get_state_a = nullptr, g_get_state_w = nullptr;
GetDataFn g_get_data_a = nullptr, g_get_data_w = nullptr;

void hold_back_state(DWORD size, void* data) {
  if (size == 256 && data && (static_cast<uint8_t*>(data)[DIK_SYSRQ] & 0x80)) screenshot::key_seen();  // Print Screen
  if (size == 256 && data && (static_cast<uint8_t*>(data)[DIK_N] & 0x80)) netmenu::key_seen();          // the network page
  if (!g_me_down.load() || !data) return;
  if (size == 256) {  // a keyboard: every key but Escape (the pause menu) and the console
    uint8_t* keys = static_cast<uint8_t*>(data);
    uint8_t esc = keys[DIK_ESCAPE], grave = keys[DIK_GRAVE];
    memset(keys, 0, 256);
    keys[DIK_ESCAPE] = esc;
    keys[DIK_GRAVE] = grave;
  } else if (size == sizeof(DIMOUSESTATE) || size == sizeof(DIMOUSESTATE2)) {  // a mouse: its buttons (the look stays)
    DIMOUSESTATE2* m = static_cast<DIMOUSESTATE2*>(data);
    memset(m->rgbButtons, 0, size == sizeof(DIMOUSESTATE) ? 4 : 8);
  }
}

HRESULT STDMETHODCALLTYPE on_get_state_a(void* dev, DWORD size, void* data) {
  HRESULT r = g_get_state_a(dev, size, data);
  if (SUCCEEDED(r)) hold_back_state(size, data);
  return r;
}
HRESULT STDMETHODCALLTYPE on_get_state_w(void* dev, DWORD size, void* data) {
  HRESULT r = g_get_state_w(dev, size, data);
  if (SUCCEEDED(r)) hold_back_state(size, data);
  return r;
}

// Buffered input: the presses are taken out (a key let go may pass, so nothing stays held).
void hold_back_data(DIDEVICEOBJECTDATA* rows, DWORD* count) {
  if (rows && count)
    for (DWORD i = 0; i < *count; ++i)
      if (rows[i].dwOfs == DIK_SYSRQ && (rows[i].dwData & 0x80)) screenshot::key_seen();  // Print Screen
      else if (rows[i].dwOfs == DIK_N && (rows[i].dwData & 0x80)) netmenu::key_seen();
  if (!g_me_down.load() || !rows || !count) return;
  DWORD kept = 0;
  for (DWORD i = 0; i < *count; ++i) {
    bool press = (rows[i].dwData & 0x80) != 0;
    bool keep = !press || rows[i].dwOfs == DIK_ESCAPE || rows[i].dwOfs == DIK_GRAVE || rows[i].dwOfs < 8 /* mouse axes (DIMOFS_X/Y/Z) */;
    if (keep) rows[kept++] = rows[i];
  }
  *count = kept;
}

HRESULT STDMETHODCALLTYPE on_get_data_a(void* dev, DWORD size, DIDEVICEOBJECTDATA* rows, DWORD* count, DWORD flags) {
  HRESULT r = g_get_data_a(dev, size, rows, count, flags);
  if (SUCCEEDED(r) && size == sizeof(DIDEVICEOBJECTDATA)) hold_back_data(rows, count);
  return r;
}
HRESULT STDMETHODCALLTYPE on_get_data_w(void* dev, DWORD size, DIDEVICEOBJECTDATA* rows, DWORD* count, DWORD flags) {
  HRESULT r = g_get_data_w(dev, size, rows, count, flags);
  if (SUCCEEDED(r) && size == sizeof(DIDEVICEOBJECTDATA)) hold_back_data(rows, count);
  return r;
}

void hook_device_methods(bool wide) {
  wchar_t path[MAX_PATH];
  UINT n = GetSystemDirectoryW(path, MAX_PATH);
  if (!n || n > MAX_PATH - 16) return;
  lstrcatW(path, L"\\dinput8.dll");
  HMODULE real = LoadLibraryW(path);
  using CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
  CreateFn create = real ? reinterpret_cast<CreateFn>(GetProcAddress(real, "DirectInput8Create")) : nullptr;
  if (!create) return;
  IUnknown* di = nullptr;
  if (FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, wide ? IID_IDirectInput8W : IID_IDirectInput8A,
                    reinterpret_cast<void**>(&di), nullptr)) || !di)
    return;
  void* dev = nullptr;
  HRESULT r = wide ? static_cast<IDirectInput8W*>(static_cast<void*>(di))->CreateDevice(GUID_SysKeyboard, reinterpret_cast<LPDIRECTINPUTDEVICE8W*>(&dev), nullptr)
                   : static_cast<IDirectInput8A*>(static_cast<void*>(di))->CreateDevice(GUID_SysKeyboard, reinterpret_cast<LPDIRECTINPUTDEVICE8A*>(&dev), nullptr);
  if (SUCCEEDED(r) && dev) {
    void** vt = *static_cast<void***>(dev);
    patch::hook(wide ? "DirectInput W: get state" : "DirectInput A: get state", reinterpret_cast<uintptr_t>(vt[9]),
                reinterpret_cast<void*>(wide ? &on_get_state_w : &on_get_state_a), reinterpret_cast<void**>(wide ? &g_get_state_w : &g_get_state_a));
    patch::hook(wide ? "DirectInput W: get data" : "DirectInput A: get data", reinterpret_cast<uintptr_t>(vt[10]),
                reinterpret_cast<void*>(wide ? &on_get_data_w : &on_get_data_a), reinterpret_cast<void**>(wide ? &g_get_data_w : &g_get_data_a));
    static_cast<IUnknown*>(dev)->Release();
  }
  di->Release();
}

struct Pad {  // XINPUT_STATE
  DWORD packet;
  WORD buttons;
  BYTE left_trigger, right_trigger;
  SHORT lx, ly, rx, ry;
};
using XInputFn = DWORD(WINAPI*)(DWORD, Pad*);
XInputFn g_xinput = nullptr;
DWORD WINAPI on_xinput(DWORD user, Pad* state) {
  DWORD r = g_xinput(user, state);
  if (r == ERROR_SUCCESS) filter_pad(state);
  return r;
}

// --- helpers ------------------------------------------------------------------------------------------------
int my_slot() { return game::ref<int>(game::kNetLocalPlayer); }

bool in_session() {
  return game::ref<uintptr_t>(game::kNetHost) > 1 && game::ref<uint8_t>(game::kNetGame) && my_slot() >= 0 && link::peers() > 0;
}

uint16_t body_of(int slot) {
  net::Players& p = game::ref<net::Players>(game::kNetPlayers);
  if (slot < 0 || slot >= p.size || !p.array) return 0xFFFF;
  return *reinterpret_cast<const uint16_t*>(p.array + game::kNetPlayerSize * slot + 0x64);
}

std::string name_of(int slot) {
  std::string n = players::name(slot);
  return n.empty() ? "Игрок " + std::to_string(slot + 1) : n;
}

const float* place_of(uint16_t id) {
  uintptr_t at = net::entity_field(id, game::kEntityPosition);
  return at ? reinterpret_cast<const float*>(at) : nullptr;
}

void set_down(int slot, bool down, bool tell) {
  if (slot < 0 || slot >= kSlots) return;
  bool was = g_down[slot].exchange(down);
  if (was == down) return;
  g_down_since[slot] = down ? GetTickCount64() : 0;
  LOGI("revive: player %d is %s", slot, down ? "down" : "up again");
  if (tell) {
    DownNote n = {static_cast<uint8_t>(slot), static_cast<uint8_t>(down ? 1 : 0)};
    link::send(link::kDowned, &n, sizeof(n), true);
  }
  if (slot == my_slot()) {
    g_me_down.store(down);
    (void)0;  // (the caption under the middle of the screen says it while this player is down: frame_tick)
  } else if (down) {
    hud::notice(name_of(slot) + " тяжело ранен: подойдите и удерживайте «Использовать»",
                name_of(slot) + " is badly wounded: come close and hold Use", 10);
  }
}

// The floor on this PC's own body; back as it was when the session ends or the body changes. Only ever the body
// this PC controls, as the level names it (net::entity_field(0xFFFF)): a slot's actor id may name another entity
// for a moment while levels change, and the floor's two fields written into another kind of entity broke the
// level's loading (the lab, 2026-11-25).
uint8_t* own_body() {
  uintptr_t at = net::entity_field(0xFFFF, 0);
  return reinterpret_cast<uint8_t*>(at);
}

void restore_floor() {
  if (!g_floor_set) return;
  g_floor_set = false;
  if (g_body && g_body == own_body()) {  // still the body it was set on
    *reinterpret_cast<float*>(g_body + kFloor) = g_old_floor;
    *reinterpret_cast<uint32_t*>(g_body + kFloorBits) = g_old_bits;
    LOGI("revive: the floor of this PC's body is back as it was");
  }
}

void own_tick(uint64_t now) {
  uint8_t* body = own_body();
  if (body != g_body) {
    restore_floor();
    g_body = body;
    set_down(my_slot(), false, true);
  }
  if (!body) return;
  if (!g_floor_set) {
    g_old_floor = *reinterpret_cast<float*>(body + kFloor);
    g_old_bits = *reinterpret_cast<uint32_t*>(body + kFloorBits);
    if (g_old_floor < -1.5f || g_old_floor > 0.5f) {  // not the health block of a player as known: left alone
      static int said = 0;
      if (said++ < 3) LOGW("revive: this PC's body has an unexpected floor (%.3f): left as it is", g_old_floor);
      return;
    }
    g_floor_set = true;
    LOGI("revive: this PC's body falls at %.0f %% instead of dying (its floor was %.3f, bits %X)", kDownHealth * 100.f, g_old_floor,
         g_old_bits);
  }
  *reinterpret_cast<float*>(body + kFloor) = kDownHealth;
  *reinterpret_cast<uint32_t*>(body + kFloorBits) &= ~2u;
  float& health = *reinterpret_cast<float*>(body + game::kEntityHealth);
  int me = my_slot();
  // (the game's regrowth lifts the health a little within a tick - 0.01 a second: "down" is up to 2 %)
  if (!g_down[me].load() && health > 0.f && health <= kDownHealth + 0.01f) set_down(me, true, true);
  if (g_down[me].load() && health > kDownHealth + 0.0005f && health < kRaisedHealth - 0.01f) {
    // the game's own regrowth would raise a fallen player by itself: kept down until a teammate comes
    health = kDownHealth;
  }
  if (health <= 0.f && g_down[me].load()) set_down(me, false, true);  // died for real (a fall): not "down" any more
  (void)now;
}

// This PC's player raises a fallen teammate: holds "use" within reach.
void tell_raising(int slot, bool on) {
  if (slot < 0) return;
  RaisingNote n = {static_cast<uint8_t>(slot), static_cast<uint8_t>(my_slot()), static_cast<uint8_t>(on ? 1 : 0)};
  link::send(link::kRaising, &n, sizeof(n), true);
}

void stop_raising() {
  if (g_raising >= 0) tell_raising(g_raising, false);
  g_raise_started = 0;
  g_raising = -1;
}

void raise_tick(uint64_t now) {
  int me = my_slot();
  if (now < g_lab_use_until.load()) g_use_held.store(true);
  else if (g_lab_use_until.exchange(0)) g_use_held.store(false);
  if (g_down[me].load() || !g_use_held.load()) {
    stop_raising();
    return;
  }
  const float* mine = place_of(body_of(me));
  if (!mine) return;
  int nearest = -1;
  float best = kReach;
  for (int s = 0; s < kSlots; ++s) {
    if (s == me || !g_down[s].load()) continue;
    const float* p = place_of(body_of(s));
    if (!p) continue;
    float d = std::sqrt((p[0] - mine[0]) * (p[0] - mine[0]) + (p[1] - mine[1]) * (p[1] - mine[1]) + (p[2] - mine[2]) * (p[2] - mine[2]));
    if (d < best) {
      best = d;
      nearest = s;
    }
  }
  if (nearest < 0) {
    stop_raising();
    return;
  }
  if (nearest != g_raising) {
    stop_raising();
    g_raising = nearest;
    g_raise_started = now;
    tell_raising(nearest, true);
  }
  if (now - g_raise_started >= kRaiseMs) {
    RaiseNote n = {static_cast<uint8_t>(nearest), static_cast<uint8_t>(me)};
    link::send(link::kRevive, &n, sizeof(n), true);
    LOGI("revive: this player raised player %d", nearest);
    hud::notice("Вы подняли: " + name_of(nearest), "You raised " + name_of(nearest), 5);
    g_raise_started = 0;
    g_raising = -1;  // (the raised one's "up again" ends their bar)
    g_use_held.store(false);
  }
}

// The host: everybody down - the last checkpoint, for everybody (the game's own "load the last checkpoint").
void host_tick(uint64_t now) {
  if (my_slot() != 0) return;
  int players = 0, down = 0;
  uint64_t since = now;
  for (int s = 0; s < kSlots; ++s) {
    if (body_of(s) == 0xFFFF) continue;
    ++players;
    if (g_down[s].load()) {
      ++down;
      if (g_down_since[s] < since) since = g_down_since[s];
    }
  }
  if (players < 2 || down < players || now - since < kAllDownMs || now - g_last_reload < 15000) return;
  g_last_reload = now;
  LOGI("revive: every player is down: the last checkpoint, for everybody");
  hud::notice("Все ранены. Загрузка последней контрольной точки", "Everybody is down. Loading the last checkpoint", 8);
  for (int s = 0; s < kSlots; ++s) set_down(s, false, false);
  engine::command("gameload auto_save", true);
}

void on_downed(int from, const uint8_t* data, size_t size) {
  if (size < sizeof(DownNote)) return;
  DownNote n;
  memcpy(&n, data, sizeof(n));
  if (n.slot >= kSlots || n.slot == my_slot()) return;
  if (my_slot() == 0) {
    if (from != n.slot) return;  // the host takes a player's own word only, and tells the others
    link::send(link::kDowned, data, size, true);
  }
  set_down(n.slot, n.down != 0, false);
}

void on_revive(int, const uint8_t* data, size_t size) {
  if (size < sizeof(RaiseNote)) return;
  RaiseNote n;
  memcpy(&n, data, sizeof(n));
  if (my_slot() == 0) link::send(link::kRevive, data, size, true);  // to the other joined games (and back: ignored)
  if (n.slot != my_slot() || !g_down[n.slot].load()) return;
  uint8_t* body = own_body();
  if (body) *reinterpret_cast<float*>(body + game::kEntityHealth) = kRaisedHealth;
  set_down(my_slot(), false, true);
  hud::notice("Вас поднял " + name_of(n.by), name_of(n.by) + " raised you", 6);
}

void on_raising(int from, const uint8_t* data, size_t size) {
  if (size < sizeof(RaisingNote)) return;
  RaisingNote n;
  memcpy(&n, data, sizeof(n));
  if (my_slot() == 0) {
    if (from != n.by) return;  // a player's own word only; the host tells the others
    link::send(link::kRaising, data, size, true);
  }
  if (n.slot != my_slot() || !g_down[n.slot].load()) return;
  if (n.on) {
    LOGI("revive: player %d raises this player", n.by);
    g_raised_by = n.by;
    g_raised_since = GetTickCount64();
  } else if (g_raised_by == n.by) {
    g_raised_by = -1;
  }
}

// Every frame: this PC's fallen player crouches (the game's own crouch: lower eyes, a crouching figure for the
// others), and the caption with the bar: who raises whom and how far it has come.
void frame_tick(uint64_t now) {
  uint8_t* body = in_session() ? own_body() : nullptr;
  bool down = body && g_me_down.load();
  if (down) {
    *reinterpret_cast<uint32_t*>(body + kInputFlags) |= kCrouch;
    g_crouched = true;
  } else if (g_crouched) {
    if (body) *reinterpret_cast<uint32_t*>(body + kInputFlags) &= ~kCrouch;
    g_crouched = false;
  }
  if (down) {
    g_raising = -1;
    if (g_raised_by >= 0 && now - g_raised_since < kRaiseMs + 1500) {
      float f = static_cast<float>(now - g_raised_since) / kRaiseMs;
      hud::progress(name_of(g_raised_by) + " поднимает вас", name_of(g_raised_by) + " is raising you", f);
    } else {
      g_raised_by = -1;
      hud::progress("Вы тяжело ранены — ждите товарища", "You are badly wounded - wait for a teammate", -1.f);
    }
  } else if (g_raising >= 0 && g_raise_started) {
    float f = static_cast<float>(now - g_raise_started) / kRaiseMs;
    hud::progress("Поднимаете: " + name_of(g_raising), "Raising " + name_of(g_raising), f);
  } else {
    g_raised_by = -1;
    hud::progress("", "", -1.f);
  }
}

void tick() {
  static uint64_t last = 0;
  uint64_t now = GetTickCount64();
  frame_tick(now);
  if (now - last < 100) return;
  last = now;
  if (!in_session()) {
    g_me_down.store(false);
    restore_floor();
    for (int s = 0; s < kSlots; ++s) g_down[s].store(false);
    g_body = nullptr;
    return;
  }
  own_tick(now);
  raise_tick(now);
  host_tick(now);
}

}  // namespace

bool downed(int slot) { return slot >= 0 && slot < kSlots && g_down[slot].load(); }

bool me_down() { return g_me_down.load(); }

void filter_pad(void* xinput_state) {
  if (xinput_state && (static_cast<Pad*>(xinput_state)->buttons & 0x8000)) netmenu::key_seen();  // Y: the network page (desk only)
  if (!g_me_down.load() || !xinput_state) return;
  Pad* p = static_cast<Pad*>(xinput_state);
  p->buttons &= 0x0010 | 0x0020;  // START, BACK (the menus)
  p->left_trigger = p->right_trigger = 0;
  p->lx = p->ly = 0;  // the right stick (the look) stays
}

bool use_held() { return g_use_held.load(); }

void lab_hold_use(unsigned ms) {
  g_lab_use_until.store(GetTickCount64() + ms);
  LOGI("lab: \"use\" is held for %u ms", ms);
}

void lab_actions(unsigned seconds) {
  g_lab_actions_until.store(GetTickCount64() + seconds * 1000ull);
  LOGI("lab: input actions are logged for %u s", seconds);
}

void install(const Config& cfg) {
  g_cfg = cfg;
  if (cfg.net_mode == Config::NetMode::Off && !cfg.lab) return;
  patch::hook("input actions", game::at(game::kInventoryAction), reinterpret_cast<void*>(&on_action), reinterpret_cast<void**>(&g_action));
  if (cfg.net_mode == Config::NetMode::Off || !cfg.net_revive) return;
  engine::on_main([] {
    hook_device_methods(false);
    hook_device_methods(true);
  });
  if (!cfg.lab) {  // (the lab's own pad hook asks filter_pad itself)
    if (HMODULE xinput = LoadLibraryW(L"xinput1_3.dll"))
      if (FARPROC get_state = GetProcAddress(xinput, MAKEINTRESOURCEA(2)))
        patch::hook("XInputGetState (revive)", reinterpret_cast<uintptr_t>(get_state), reinterpret_cast<void*>(&on_xinput), reinterpret_cast<void**>(&g_xinput));
  }
  link::on(link::kDowned, &on_downed);
  link::on(link::kRevive, &on_revive);
  link::on(link::kRaising, &on_raising);
  engine::on_tick(&tick);
}

}  // namespace revive
