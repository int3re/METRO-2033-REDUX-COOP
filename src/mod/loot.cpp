#include "mod/loot.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "common/log.h"
#include "mod/engine.h"
#include "mod/game.h"
#include "mod/link.h"
#include "mod/patch.h"

namespace net {
uint8_t* entity_by_id(uint16_t id);
uint8_t* parent_of(uint8_t* e);
}  // namespace net

namespace loot {
namespace {

// --- the game ------------------------------------------------------------------------------------------------
// A player takes a thing (a cabinet's contents, a box, a shelf, the dead) with an "act" event: {who: the player's
// body, what: the thing, kind 0}. The player's PC makes one per thing (exe+68F480): a joined game sends it to the
// host, the host checks it (exe+68C350: may the player take it - which on the host knows nothing of where a joined
// player looks) and puts it into its queue of events; that queue goes to every PC and every PC runs each event
// itself (exe+68BC90 -> the level's vtbl + 312): the thing hangs on the body, the inventory takes it (ammunition
// into its stack, a medkit or a filter counted) and the thing is removed. When the host removes a thing it tells
// every PC so (the "gone" event, exe+68C820). So a thing one player took was gone for all.
constexpr uintptr_t kActEvent = 0x68F480;  // void (net core*, handle** who, handle** what, int kind, char x)
constexpr uintptr_t kActCheck = 0x68C350;  // int64 (net core*, u8* data): the host's check of an act event
constexpr uintptr_t kActRun = 0x68BC90;    // int64 (x, u8* data): {u16; u16 who; u16 what; u8 kind & 0xF}
constexpr uintptr_t kGone = 0x68C820;      // int64 (x, u8* data): {...; u8 count & 0x7F @+4; u16 ids from +5}
constexpr uintptr_t kMedkitClass = 0xAB3870;
constexpr uintptr_t kFilterClass = 0xAB5EC0;
constexpr uint32_t kCopyFlag = 0x10000;  // entity flag: a copy the session spawned
constexpr uint32_t kActorFlag = 4;       // entity flag: a character

Config g_cfg;
std::atomic<uint64_t> g_trace_until{0};
std::atomic<bool> g_loading{true};

// per entity id, for the level now loaded on this PC
std::atomic<uint8_t> g_marks[65536];
constexpr uint8_t kFromCharacter = 1;  // hung on a character (not a player) once: an enemy's weapon, the dead's things
constexpr uint8_t kMine = 2;           // this PC's player took it
constexpr uint8_t kOthers = 4;         // another player took it; it stays here for this PC's player
std::atomic<int> g_kept{0}, g_left{0};

void clear_marks() {
  for (auto& m : g_marks) m.store(0, std::memory_order_relaxed);
}

void on_log(const char* line) {
  if (strstr(line, "loading content\\maps\\") || strstr(line, "load save bin")) {
    g_loading.store(true);
    clear_marks();
  } else if (strstr(line, "loader_finish: mapstate_running")) {
    g_loading.store(false);
  }
}

const char* text_of(const uint8_t* str) { return str ? reinterpret_cast<const char*>(str) + 0x14 : ""; }
uint16_t id_of(const uint8_t* e) { return e ? *reinterpret_cast<const uint16_t*>(e + game::kEntityId) : 0xFFFF; }
uint32_t flags_of(const uint8_t* e) { return e ? *reinterpret_cast<const uint32_t*>(e + game::kEntityFlags) : 0; }
const char* name_of(const uint8_t* e) { return e ? text_of(*reinterpret_cast<uint8_t* const*>(e + game::kEntityName)) : "-"; }
uintptr_t class_of(const uint8_t* e) { return e ? *reinterpret_cast<const uintptr_t*>(e) : 0; }

int my_slot() { return game::ref<int>(game::kNetLocalPlayer); }
bool network_game() { return game::ref<uintptr_t>(game::kNetHost) > 1 && game::ref<uint8_t>(game::kNetGame); }

uint8_t* my_body() {
  uint8_t* level = game::ref<uint8_t*>(game::kLevel);
  return level ? *reinterpret_cast<uint8_t**>(level + game::kLevelControlled) : nullptr;
}

bool player_body(const uint8_t* e) { return e && class_of(e) == game::at(game::kPlayerClass); }
bool character(const uint8_t* e) { return e && (flags_of(e) & kActorFlag) && !player_body(e); }

uint8_t* as_item(uint8_t* e) {
  using AsItem = uint8_t* (*)(uint8_t*);
  return e ? (*reinterpret_cast<AsItem**>(e))[game::kEntityAsItem / 8](e) : nullptr;
}

std::string chain(int skip, int count) {
  void* frames[20] = {};
  USHORT n = RtlCaptureStackBackTrace(skip, count, frames, nullptr);
  std::string s;
  for (USHORT i = 0; i < n; ++i) {
    uintptr_t a = reinterpret_cast<uintptr_t>(frames[i]);
    char one[24];
    if (a >= game::at(0) && a < game::at(0) + game::kSizeOfImage)
      snprintf(one, sizeof(one), " +%llX", static_cast<unsigned long long>(a - game::at(0)));
    else
      snprintf(one, sizeof(one), " mod");
    s += one;
  }
  return s;
}

uint8_t* from_handle(uint8_t** handle) {
  __try {
    if (!handle || !*handle) return nullptr;
    uint8_t* target = *reinterpret_cast<uint8_t**>(*handle);
    return target ? target - game::kEntityHandle : nullptr;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

bool consumable(const uint8_t* x) {
  uintptr_t c = class_of(x);
  return c == game::at(game::kAmmoClass) || c == game::at(kMedkitClass) || c == game::at(kFilterClass);
}

// Where a thing is: on a player's body (taken), on a character (an enemy's, the dead's).
bool on_player(uint8_t* x) {
  for (uint8_t* p = net::parent_of(x); p; p = net::parent_of(p))
    if (player_body(p)) return true;
  return false;
}
bool from_character(uint8_t* x) {
  if (g_marks[id_of(x)].load(std::memory_order_relaxed) & kFromCharacter) return true;
  for (uint8_t* p = net::parent_of(x); p; p = net::parent_of(p))
    if (character(p)) return true;
  return false;
}

// Each player's own: a thing of the level a player picks up - ammunition, medkits, filters from anywhere (a cabinet,
// a box, the dead), and any other thing (a weapon, a throwable) that lies in the level as the level has it (a stash,
// a reward). Every player takes their own; what one took stays for the others until they take it themselves.
// A weapon or another thing an enemy had (it dropped it, the dead hold it) is one for all: who takes it first has it.
bool personal(uint8_t* x) {
  if (!x || (flags_of(x) & kCopyFlag) || !as_item(x) || on_player(x)) return false;
  return consumable(x) || !from_character(x);
}

// --- a thing leaves a character: from then on it is an enemy's thing, one for all -------------------------------
// exe+6B1040: void* (entity*, handle** out, handle** parent, u8* flags) - hang an entity on a parent (null: off)
using AttachFn = void* (*)(uint8_t*, void*, uint8_t**, uint8_t*);
AttachFn g_attach = nullptr;

void* on_attach(uint8_t* e, void* out, uint8_t** parent, uint8_t* flags) {
  if (e) {
    uint8_t* to = from_handle(parent);
    uint8_t* from = net::parent_of(e);
    if (character(from) || character(to)) g_marks[id_of(e)].fetch_or(kFromCharacter, std::memory_order_relaxed);
    if (g_cfg.lab && GetTickCount64() < g_trace_until.load())
      LOGI("lab: attach %X '%s' flags %X to %X '%s' (flags %X), thread %lu, by%s", id_of(e), name_of(e), flags_of(e), id_of(to), name_of(to),
           flags ? *flags : 0xFF, GetCurrentThreadId(), chain(1, 12).c_str());
  }
  return g_attach(e, out, parent, flags);
}

// --- the host's check: a joined player's "take" is taken on trust ----------------------------------------------
// Their own PC asked the game whether they may take it (with what they look at); the host cannot - and with each
// player's own things the host may not even have it any more (its own player took it).
using ActCheckFn = int64_t (*)(void*, uint8_t*);
ActCheckFn g_act_check = nullptr;

int64_t on_act_check(void* core, uint8_t* data) {
  if (data && g_cfg.net_loot && my_slot() == 0 && network_game() && (data[6] & 0xF) == 0) {
    uint8_t* who = net::entity_by_id(*reinterpret_cast<uint16_t*>(data + 2));
    uint8_t* what = net::entity_by_id(*reinterpret_cast<uint16_t*>(data + 4));
    if (player_body(who) && who != my_body() && (!what || !on_player(what))) {
      if (g_cfg.lab && GetTickCount64() < g_trace_until.load())
        LOGI("lab: a joined player's take of %X '%s' is let through", *reinterpret_cast<uint16_t*>(data + 4), name_of(what));
      return 1;
    }
  }
  return g_act_check(core, data);
}

// --- every PC: a player's own take happens on that player's PC only -----------------------------------------------
// The take itself is the host's work: the host hangs the thing on the body and that body's inventory takes it; a
// joined game, running the same event, does nothing - it never learns of the new parent (only of the removal). So a
// joined player never got anything they took. Now the joined game does on its own world what the host does on its:
// the thing comes off what holds it and hangs on the body - the player's own inventory takes it (ammunition into
// its stack, the game's "picked up" line). A thing someone else took (one for all) hangs on their body here, kept
// out of that body's simple inventory (flag 0x10), so it is gone from the floor here as on the host.
void hang(uint8_t* x, uint8_t* body, uint8_t flags) {
  using Attach = void* (*)(uint8_t*, void**, void**, uint8_t*);
  void* out = nullptr;
  void* none = nullptr;
  uint8_t off = 0;
  reinterpret_cast<Attach>(game::at(game::kAttach))(x, &out, &none, &off);
  void* handle = *reinterpret_cast<void**>(body + game::kEntityHandle);
  if (!handle) return;
  out = nullptr;
  reinterpret_cast<Attach>(game::at(game::kAttach))(x, &out, &handle, &flags);
}

using ActRunFn = int64_t (*)(void*, uint8_t*);
ActRunFn g_act_run = nullptr;

int64_t on_act_run(void* x, uint8_t* data) {
  if (data && g_cfg.lab && GetTickCount64() < g_trace_until.load()) {
    uint16_t a = *reinterpret_cast<uint16_t*>(data + 2), b = *reinterpret_cast<uint16_t*>(data + 4);
    LOGI("lab: act event runs: %X '%s' on %X '%s', kind %d (byte %X)", a, name_of(net::entity_by_id(a)), b, name_of(net::entity_by_id(b)),
         data[6] & 0xF, data[6]);
  }
  if (data && g_cfg.net_loot && (data[6] & 0xF) == 0 && network_game()) {
    uint16_t who_id = *reinterpret_cast<uint16_t*>(data + 2), what_id = *reinterpret_cast<uint16_t*>(data + 4);
    uint8_t* who = net::entity_by_id(who_id);
    uint8_t* what = net::entity_by_id(what_id);
    if (player_body(who)) {
      bool mine = who == my_body();
      bool joined = my_slot() > 0;
      if (!what || on_player(what)) {
        if (!mine) return 0;  // gone here already / someone has it: nothing to do here
      } else if (personal(what)) {
        if (!mine) {
          g_marks[what_id].fetch_or(kOthers, std::memory_order_relaxed);
          ++g_kept;
          LOGD("loot: player body %X takes %X '%s': its own, it stays here", who_id, what_id, name_of(what));
          return 0;
        }
        g_marks[what_id].fetch_or(kMine, std::memory_order_relaxed);
        if (joined) {  // this PC's player takes it: on this PC's world, as the host does on its own
          int64_t r = g_act_run(x, data);
          if (!on_player(what)) {
            LOGD("loot: this PC's player takes %X '%s'", what_id, name_of(what));
            hang(what, who, 0);
          }
          return r;
        }
      } else if (joined) {  // one for all (an enemy's weapon): the host takes it out of everybody's reach
        int64_t r = g_act_run(x, data);
        if (!on_player(what)) {
          LOGD("loot: player body %X takes %X '%s' (one for all)%s", who_id, what_id, name_of(what), mine ? ": this PC's player" : "");
          hang(what, who, mine ? 0 : 0x10);
        }
        return r;
      }
    }
  }
  return g_act_run(x, data);
}

// --- every PC: the host's "gone" leaves this PC's own things alone ---------------------------------------------
// When the host's own player takes a thing, the host removes it and tells every PC. A thing this PC's player has
// not taken yet stays here.
using GoneFn = int64_t (*)(void*, uint8_t*);
GoneFn g_gone = nullptr;

void keep_own(uint8_t* data) {
  if (data && g_cfg.net_loot && my_slot() > 0 && network_game()) {
    unsigned count = data[4] & 0x7F, kept = 0;
    uint16_t ids[128];
    for (unsigned i = 0; i < count; ++i) {
      uint16_t id;
      memcpy(&id, data + 5 + 2 * i, 2);
      uint8_t m = g_marks[id].load(std::memory_order_relaxed);
      uint8_t* e = (m & kOthers) && !(m & kMine) ? net::entity_by_id(id) : nullptr;
      if (e && !on_player(e)) {
        ++g_left;
        LOGD("loot: the host took %X '%s' and removes it: this PC's player has not taken it, it stays here", id, name_of(e));
        continue;
      }
      ids[kept++] = id;
    }
    if (kept != count) {
      for (unsigned i = 0; i < kept; ++i) memcpy(data + 5 + 2 * i, &ids[i], 2);
      data[4] = static_cast<uint8_t>((data[4] & 0x80) | kept);
    }
  }
}

int64_t on_gone(void* x, uint8_t* data) {
  keep_own(data);
  return g_gone(x, data);
}

// --- the lab -------------------------------------------------------------------------------------------------------
using ActEventFn = void (*)(void*, uint8_t**, uint8_t**, int, char);
ActEventFn g_act_event = nullptr;

void on_act_event(void* core, uint8_t** who, uint8_t** what, int kind, char x) {
  if (GetTickCount64() < g_trace_until.load()) {
    uint8_t* a = from_handle(who);
    uint8_t* b = from_handle(what);
    LOGI("lab: act event: %X '%s' on %X '%s', kind %d, x %d, by%s", id_of(a), name_of(a), id_of(b), name_of(b), kind, x, chain(1, 9).c_str());
  }
  g_act_event(core, who, what, kind, x);
}

}  // namespace

void install(const Config& cfg) {
  g_cfg = cfg;
  if (cfg.net_loot) engine::on_log(&on_log);
  if (cfg.net_loot || cfg.lab) {
    patch::hook("attach", game::at(game::kAttach), reinterpret_cast<void*>(&on_attach), reinterpret_cast<void**>(&g_attach));
    patch::hook("act event: the host's check", game::at(kActCheck), reinterpret_cast<void*>(&on_act_check), reinterpret_cast<void**>(&g_act_check));
    patch::hook("act event runs", game::at(kActRun), reinterpret_cast<void*>(&on_act_run), reinterpret_cast<void**>(&g_act_run));
    if (!cfg.lab)  // (the lab's trace of net.cpp has this place: it calls filter_gone)
      patch::hook("gone event", game::at(kGone), reinterpret_cast<void*>(&on_gone), reinterpret_cast<void**>(&g_gone));
  }
  if (cfg.lab)
    patch::hook("act event (lab trace)", game::at(kActEvent), reinterpret_cast<void*>(&on_act_event), reinterpret_cast<void**>(&g_act_event));
}

void tick() {}

void filter_gone(uint8_t* data) { keep_own(data); }

void lab_trace(unsigned seconds) {
  g_trace_until.store(GetTickCount64() + seconds * 1000ull);
  LOGI("lab: attaching and taking are logged for %u s (so far: %d things kept here for this PC's player, %d of them the host removed)", seconds,
       g_kept.load(), g_left.load());
}

void lab_take(unsigned id, int kind) {
  uint8_t* body = my_body();
  uint8_t* item = net::entity_by_id(static_cast<uint16_t>(id));
  if (!body || !item) {
    LOGI("lab: take: no body (%p) or no entity %X", body, id);
    return;
  }
  uint8_t* who = *reinterpret_cast<uint8_t**>(body + game::kEntityHandle);
  uint8_t* what = *reinterpret_cast<uint8_t**>(item + game::kEntityHandle);
  if (!who || !what) {
    LOGI("lab: take: no handle (%p, %p)", who, what);
    return;
  }
  LOGI("lab: take: this PC's player %X acts on %X '%s' (an act event, kind %d: 0 take, 1 use)", id_of(body), id, name_of(item), kind);
  reinterpret_cast<ActEventFn>(game::at(kActEvent))(reinterpret_cast<void*>(game::at(game::kNetCore)), &who, &what, kind, 0);
}

void lab_can_take(unsigned id) {
  uint8_t* body = my_body();
  uint8_t* item = net::entity_by_id(static_cast<uint16_t>(id));
  if (!body || !item) return;
  using AsPlayer = uint8_t* (*)(uint8_t*);
  uint8_t* player = reinterpret_cast<AsPlayer>((*reinterpret_cast<void***>(body))[0x6D8 / 8])(body);
  using CanTake = int (*)(uint8_t*, uint8_t*, unsigned, int*, int, int);
  int out = -1;
  int r = player ? reinterpret_cast<CanTake>((*reinterpret_cast<void***>(item))[0x220 / 8])(item, player, 0, &out, 0, 0) : -2;
  uint8_t* level = game::ref<uint8_t*>(game::kLevel);
  LOGI("lab: can %X take %X '%s'? %d (out %d); its own: %d, an enemy's: %d, marks %X; the level's use +%llX", id_of(body), id, name_of(item), r,
       out, personal(item), from_character(item), g_marks[id & 0xFFFF].load(),
       level ? static_cast<unsigned long long>((*reinterpret_cast<uintptr_t**>(level))[312 / 8] - game::at(0)) : 0ull);
}

void lab_on_take(uint8_t* actor, uint8_t* child, uint8_t* flags) {
  if (GetTickCount64() >= g_trace_until.load()) return;
  LOGI("lab: %X '%s' takes %X '%s' (flags %X, vtbl +%llX), thread %lu, by%s", id_of(actor), name_of(actor), id_of(child), name_of(child),
       flags ? *flags : 0xFF, child ? static_cast<unsigned long long>(class_of(child) - game::at(0)) : 0ull, GetCurrentThreadId(),
       chain(1, 14).c_str());
}

}  // namespace loot
