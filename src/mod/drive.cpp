#include "mod/drive.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/log.h"
#include "mod/engine.h"
#include "mod/game.h"
#include "mod/hud.h"
#include "mod/link.h"
#include "mod/motions.h"
#include "mod/patch.h"
#include "mod/players.h"
#include "mod/revive.h"

namespace net {
uint8_t* entity_by_id(uint16_t id);
int chain_owner(const uint8_t* block);
void tag_chain(uint8_t* block, int slot);
void fire_as(uint8_t* output, int slot);
}  // namespace net

namespace drive {
namespace {

// --- the game ------------------------------------------------------------------------------------------------
// "actions/entity/play motion control": made by exe+6D7330 (0x13C bytes). + 32 the list of its targets (the things
// it moves), + 264 / + 272 the motions forwards / backwards, + 300 float speed (acc multiplies it by + 304, decc
// divides by + 308). Its inputs are {object, function} slots; the functions (one each, exe+486D0 ...) put the
// command into the script's job queue or change the speed. Its outputs: fwd_end at + 168, bwd_end + 200,
// pause_end + 232, fired by the delegates the motion calls at its end (exe+488F0, +48950, +489B0).
constexpr uintptr_t kClass = 0xBE8A80;
constexpr uintptr_t kMade = 0x6D7330;
constexpr size_t kTargets = 32;
constexpr uintptr_t kCommandFns[] = {0x486D0, 0x48730, 0x48790, 0x487F0, 0x48850, 0x488A0};
const char* const kCommandNames[] = {"fwd", "pause", "bwd", "stop", "acc", "decc"};
constexpr int kCommands = 6;
constexpr uintptr_t kEndFns[] = {0x488F0, 0x48950, 0x489B0};
constexpr size_t kEndOutputs[] = {168, 200, 232};
const char* const kEndNames[] = {"fwd_end", "bwd_end", "pause_end"};
constexpr uintptr_t kTargetCount = 0x705F30;   // unsigned (list*, 0)
constexpr uintptr_t kTargetAt = 0x706030;      // (handle** out, list*, unsigned i, 0, 0): a counted reference
constexpr uintptr_t kHandleDrop = 0x7F00;      // (handle**)
// The game's input (object at exe+D01EA8): vtbl + 0x148 gives an action to every listener (the level script's
// "triggers/player/input": action, flags 1 pressed / 2 let go, the player's handle) and then to the player;
// vtbl + 0x158 = exe+3E7CF0: is the action's key down now (a trigger asks it when it starts).
constexpr uintptr_t kInput = 0xD01EA8;
constexpr size_t kInputGive = 0x148;
constexpr uintptr_t kActionDown = 0x3E7CF0;

#pragma pack(push, 1)
struct DriveNote {
  uint8_t slot;        // the player whose use it is
  uint8_t command;     // 0 fwd, 1 pause, 2 bwd, 3 stop, 4 acc, 5 decc
  uint8_t n;           // how many target ids follow
  uint16_t ids[4];     // the block's targets, smallest first
  uint8_t from_end;    // which of the blocks on the same targets, counted from the one made last
  uint16_t motion;     // the first target's motion there before the command (0xFFFF: none)
  float time;          // ... and its time
};
struct BusyNote {
  uint8_t slot;
  uint8_t on;
  uint8_t n;
  uint16_t ids[4];
};
#pragma pack(pop)

Config g_cfg;
using BlockFn = void (*)(uint8_t*);
using MadeFn = uint8_t* (*)(uint8_t*);
using DownFn = int64_t (*)(void*, int64_t);
MadeFn g_made = nullptr;
BlockFn g_command[kCommands] = {};
BlockFn g_end[3] = {};
DownFn g_down = nullptr;
thread_local bool t_remote = false;  // the mod gives another PC's command here

std::vector<uint8_t*> g_blocks;  // the level's motion control blocks, in the order they were made
uint64_t g_block_at = 0;

struct Busy {
  int slot;
  uint64_t until;
};
std::unordered_map<uint16_t, Busy> g_busy;  // things other players are at
uint64_t g_slot_until[33] = {};             // per slot: until when its use goes on (its action, not a scene)

struct Mine {
  uint16_t ids[4] = {};
  int n = 0;
  bool on = false;
  uint64_t last = 0;   // the last sign of this player still being at it
  uint64_t sent = 0;
} g_mine;

std::atomic<unsigned> g_hold_action{0};
std::atomic<uint64_t> g_hold_until{0};
int g_told = 0;

int my_slot() { return game::ref<int>(game::kNetLocalPlayer); }

bool in_session() {
  return game::ref<uintptr_t>(game::kNetHost) > 1 && game::ref<uint8_t>(game::kNetGame) && my_slot() >= 0 && link::peers() > 0;
}

bool alive(const uint8_t* block) {
  return block && !IsBadReadPtr(block, 0x140) && *reinterpret_cast<const uintptr_t*>(block) == game::at(kClass);
}

// The ids of the things a block moves, smallest first (-1: the block cannot be read).
int targets(uint8_t* block, uint16_t* ids, int max) {
  __try {
    unsigned n = reinterpret_cast<unsigned (*)(uint8_t*, unsigned)>(game::at(kTargetCount))(block + kTargets, 0);
    int got = 0;
    for (unsigned i = 0; i < n && i < 16; ++i) {
      void* handle = nullptr;
      reinterpret_cast<void* (*)(void**, uint8_t*, unsigned, unsigned, unsigned)>(game::at(kTargetAt))(&handle, block + kTargets, i, 0, 0);
      uint8_t* target = handle ? *reinterpret_cast<uint8_t**>(handle) : nullptr;
      if (target && got < max) ids[got++] = *reinterpret_cast<uint16_t*>(target - game::kEntityHandle + game::kEntityId);
      if (handle) reinterpret_cast<void (*)(void**)>(game::at(kHandleDrop))(&handle);
    }
    for (int i = 1; i < got; ++i)
      for (int j = i; j > 0 && ids[j] < ids[j - 1]; --j) {
        uint16_t t = ids[j];
        ids[j] = ids[j - 1];
        ids[j - 1] = t;
      }
    return got;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}

std::vector<uint8_t*> blocks_on(const uint16_t* ids, int n) {
  std::vector<uint8_t*> out;
  for (uint8_t* b : g_blocks) {
    if (!alive(b)) continue;
    uint16_t got[4];
    if (targets(b, got, 4) == n && !memcmp(got, ids, n * sizeof(uint16_t))) out.push_back(b);
  }
  return out;
}

const char* name_of(uint16_t id) {
  uint8_t* e = net::entity_by_id(id);
  const uint8_t* str = e ? *reinterpret_cast<uint8_t* const*>(e + game::kEntityName) : nullptr;
  return str ? reinterpret_cast<const char*>(str) + 0x14 : "?";
}

// Another player at one of these things now: its slot, else -1.
int busy_by(const uint16_t* ids, int n, uint64_t now) {
  for (int i = 0; i < n; ++i) {
    auto b = g_busy.find(ids[i]);
    if (b != g_busy.end() && b->second.until > now && b->second.slot != my_slot()) return b->second.slot;
  }
  return -1;
}

// A thing near the one this PC's player is at now (within 20 m): the wheel's gate is moved by a "motion control" the
// script gives commands to from the held key itself (exe+6FF530, "locker:on_input_def"), not in the use's chain.
bool near_mine(const uint16_t* ids, int n) {
  if (!g_mine.on || n <= 0) return false;
  for (int i = 0; i < n; ++i)
    for (int k = 0; k < g_mine.n; ++k)
      if (ids[i] == g_mine.ids[k]) return true;
  uint8_t* at = g_mine.n ? net::entity_by_id(g_mine.ids[0]) : nullptr;
  if (!at) return false;
  const float* a = reinterpret_cast<const float*>(at + game::kEntityPosition);
  for (int i = 0; i < n; ++i) {
    uint8_t* e = net::entity_by_id(ids[i]);
    if (!e) continue;
    const float* b = reinterpret_cast<const float*>(e + game::kEntityPosition);
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    if (dx * dx + dy * dy + dz * dz < 20.f * 20.f) return true;
  }
  return false;
}

void send_busy(bool on) {
  BusyNote note = {};
  note.slot = static_cast<uint8_t>(my_slot());
  note.on = on ? 1 : 0;
  note.n = static_cast<uint8_t>(g_mine.n);
  memcpy(note.ids, g_mine.ids, sizeof(note.ids));
  link::send(link::kBusy, &note, sizeof(note), true);
}

void mark_busy(int slot, const uint16_t* ids, int n, uint64_t until) {
  for (int i = 0; i < n; ++i) g_busy[ids[i]] = {slot, until};
  if (slot >= 0 && slot < 33 && g_slot_until[slot] < until) g_slot_until[slot] = until;
}

// --- the blocks ------------------------------------------------------------------------------------------------
uint8_t* on_made(uint8_t* block) {
  uint8_t* made = g_made(block);
  uint64_t now = GetTickCount64();
  if (now - g_block_at > 3000) g_blocks.clear();  // (a level's blocks are made in one go while it loads)
  g_block_at = now;
  if (made && g_blocks.size() < 4096) {
    for (size_t i = 0; i < g_blocks.size(); ++i)
      if (g_blocks[i] == made) {
        g_blocks.erase(g_blocks.begin() + static_cast<std::ptrdiff_t>(i));
        break;
      }
    g_blocks.push_back(made);
  }
  return made;
}

std::string stack(int skip, int count) {
  void* frames[16] = {};
  USHORT n = RtlCaptureStackBackTrace(skip, count, frames, nullptr);
  std::string s;
  for (USHORT i = 0; i < n; ++i) {
    uintptr_t a = reinterpret_cast<uintptr_t>(frames[i]);
    char one[24];
    if (a >= game::at(0) && a < game::at(0) + game::kSizeOfImage) snprintf(one, sizeof(one), " +%llX", static_cast<unsigned long long>(a - game::at(0)));
    else snprintf(one, sizeof(one), " mod");
    s += one;
  }
  return s;
}

void command(int c, uint8_t* block) {
  if (g_cfg.lab && g_told < 400 && alive(block)) {
    uint16_t lab_ids[4] = {};
    int ln = targets(block, lab_ids, 4);
    ++g_told;
    LOGI("drive lab: '%s' for block %p (%d: %s %X) chain %d%s, by%s", kCommandNames[c], static_cast<void*>(block), ln, ln > 0 ? name_of(lab_ids[0]) : "-",
         ln > 0 ? lab_ids[0] : 0, net::chain_owner(block), t_remote ? " (another PC's)" : "", stack(1, 8).c_str());
  }
  if (t_remote || !in_session() || !alive(block)) return g_command[c](block);
  int me = my_slot();
  int owner = net::chain_owner(block);
  uint16_t ids[4] = {};
  int n = targets(block, ids, 4);
  if (n < 0) n = 0;
  uint64_t now = GetTickCount64();
  int busy = busy_by(ids, n, now);
  if ((owner >= 0 && owner != me) || busy >= 0) {  // another player's thing now: its PC gives the commands
    if (g_told++ < 80)
      LOGI("drive: '%s' is player %d's now: this PC's own '%s' for it is left out", n ? name_of(ids[0]) : "?", owner >= 0 && owner != me ? owner : busy,
           kCommandNames[c]);
    return;
  }
  if (n == 0 || (owner != me && !near_mine(ids, n))) return g_command[c](block);  // the level's own script (each PC's)
  DriveNote note = {};
  note.slot = static_cast<uint8_t>(me);
  note.command = static_cast<uint8_t>(c);
  note.n = static_cast<uint8_t>(n);
  memcpy(note.ids, ids, sizeof(ids));
  note.from_end = 0xFF;
  std::vector<uint8_t*> same = blocks_on(ids, n);
  for (size_t i = 0; i < same.size(); ++i)
    if (same[i] == block) note.from_end = static_cast<uint8_t>(same.size() - 1 - i);
  uint16_t motion = 0xFFFF;
  float time = 0.f;
  if (!motions::time_of(ids[0], &motion, &time)) motion = 0xFFFF;
  note.motion = motion;
  note.time = time;
  g_command[c](block);
  for (int i = 0; i < n; ++i) motions::mark_driven(ids[i]);
  g_mine.last = now;
  bool sent = link::send(link::kDrive, &note, sizeof(note), true);
  if (g_told++ < 120)
    LOGI("drive: this player's '%s' for '%s' (%d thing%s; motion #%u at %.2f s)%s", kCommandNames[c], name_of(ids[0]), n, n == 1 ? "" : "s", note.motion,
         note.motion == 0xFFFF ? 0.f : note.time, sent ? ": the others are told" : "");
}

void cmd_fwd(uint8_t* b) { command(0, b); }
void cmd_pause(uint8_t* b) { command(1, b); }
void cmd_bwd(uint8_t* b) { command(2, b); }
void cmd_stop(uint8_t* b) { command(3, b); }
void cmd_acc(uint8_t* b) { command(4, b); }
void cmd_decc(uint8_t* b) { command(5, b); }
void (*const kCommandHooks[])(uint8_t*) = {cmd_fwd, cmd_pause, cmd_bwd, cmd_stop, cmd_acc, cmd_decc};

// The end of the motion: in another player's chain (its commands came from that PC), what the script does next
// is that player's chain here too - its scene blocks only finished, the story going on.
void end(int which, uint8_t* block) {
  int owner = in_session() && alive(block) ? net::chain_owner(block) : -1;
  if (g_cfg.lab && g_told < 400) {
    ++g_told;
    LOGI("drive lab: '%s' of block %p, chain %d", kEndNames[which], static_cast<void*>(block), owner);
  }
  if (owner < 0 || owner == my_slot()) return g_end[which](block);
  if (g_told++ < 120) LOGI("drive: '%s' of a thing player %d turns came here too: the story goes on in that player's chain", kEndNames[which], owner);
  net::fire_as(block + kEndOutputs[which], owner);
}

void end_fwd(uint8_t* b) { end(0, b); }
void end_bwd(uint8_t* b) { end(1, b); }
void end_pause(uint8_t* b) { end(2, b); }
void (*const kEndHooks[])(uint8_t*) = {end_fwd, end_bwd, end_pause};

// --- the messages ------------------------------------------------------------------------------------------------
void on_drive(int from, const uint8_t* data, size_t size) {
  DriveNote note;
  if (size < sizeof(note)) return;
  memcpy(&note, data, sizeof(note));
  if (my_slot() == 0 && from >= 1 && link::peers() > 1) link::send(link::kDrive, data, size, true);  // to the other joined games
  if (note.slot == static_cast<uint8_t>(my_slot()) || note.command >= kCommands || note.n == 0 || note.n > 4) return;
  uint64_t now = GetTickCount64();
  mark_busy(note.slot, note.ids, note.n, now + 4000);
  std::vector<uint8_t*> same = blocks_on(note.ids, note.n);
  uint8_t* block = nullptr;
  if (note.from_end < same.size()) block = same[same.size() - 1 - note.from_end];
  else if (!same.empty()) block = same.back();
  if (!block) {
    if (g_told++ < 120) LOGW("drive: player %d's '%s' for '%s': no such block here", note.slot, kCommandNames[note.command], name_of(note.ids[0]));
    return;
  }
  net::tag_chain(block, note.slot);
  for (int i = 0; i < note.n; ++i) motions::mark_driven(note.ids[i]);
  if (note.motion != 0xFFFF) motions::set_time(note.ids[0], note.motion, note.time);
  t_remote = true;
  g_command[note.command](block);
  t_remote = false;
  if (g_told++ < 120)
    LOGI("drive: player %d's '%s' for '%s' given here too (motion #%u at %.2f s there)", note.slot, kCommandNames[note.command], name_of(note.ids[0]),
         note.motion, note.motion == 0xFFFF ? 0.f : note.time);
}

void on_busy(int from, const uint8_t* data, size_t size) {
  BusyNote note;
  if (size < sizeof(note)) return;
  memcpy(&note, data, sizeof(note));
  if (my_slot() == 0 && from >= 1 && link::peers() > 1) link::send(link::kBusy, data, size, true);
  if (note.slot == static_cast<uint8_t>(my_slot()) || note.n > 4) return;
  uint64_t now = GetTickCount64();
  if (note.on) {
    mark_busy(note.slot, note.ids, note.n, now + 3500);
  } else {
    for (int i = 0; i < note.n; ++i) {
      auto b = g_busy.find(note.ids[i]);
      if (b != g_busy.end() && b->second.slot == note.slot) g_busy.erase(b);
    }
    if (note.slot < 33) g_slot_until[note.slot] = now + 1000;
  }
}

// --- the lab's held input ----------------------------------------------------------------------------------------
int64_t on_down(void* input, int64_t action) {
  if (g_hold_action.load() && static_cast<unsigned>(action) == g_hold_action.load() && GetTickCount64() < g_hold_until.load()) return 1;
  return g_down(input, action);
}

void give_action(unsigned action, int flags) {
  uint8_t* input = game::ref<uint8_t*>(kInput);
  uint8_t* level = game::ref<uint8_t*>(game::kLevel);
  uint8_t* own = level ? *reinterpret_cast<uint8_t**>(level + game::kLevelControlled) : nullptr;
  void* handle = own ? *reinterpret_cast<void**>(own + game::kEntityHandle) : nullptr;
  if (!input || !handle) {
    LOGI("lab: hold: no input or no body");
    return;
  }
  using GiveFn = char (*)(void*, unsigned, unsigned, unsigned, void*, int);
  char r = reinterpret_cast<GiveFn>((*reinterpret_cast<void***>(input))[kInputGive / 8])(input, action, static_cast<unsigned>(flags), 0, &handle, 0);
  LOGI("lab: input action %u %s (taken: %d)", action, flags == 1 ? "pressed" : "let go", static_cast<int>(r));
}

}  // namespace

void install(const Config& cfg) {
  g_cfg = cfg;
  if (cfg.lab) patch::hook("input: action down (lab)", game::at(kActionDown), reinterpret_cast<void*>(&on_down), reinterpret_cast<void**>(&g_down));
  if (!cfg.net_drive || !cfg.net_use) return;
  patch::hook("motion control made", game::at(kMade), reinterpret_cast<void*>(&on_made), reinterpret_cast<void**>(&g_made));
  for (int c = 0; c < kCommands; ++c) {
    char what[48];
    snprintf(what, sizeof(what), "motion control: %s", kCommandNames[c]);
    patch::hook(what, game::at(kCommandFns[c]), reinterpret_cast<void*>(kCommandHooks[c]), reinterpret_cast<void**>(&g_command[c]));
  }
  for (int e = 0; e < 3; ++e) {
    char what[48];
    snprintf(what, sizeof(what), "motion control: %s", kEndNames[e]);
    patch::hook(what, game::at(kEndFns[e]), reinterpret_cast<void*>(kEndHooks[e]), reinterpret_cast<void**>(&g_end[e]));
  }
  link::on(link::kDrive, &on_drive);
  link::on(link::kBusy, &on_busy);
}

void tick() {
  uint64_t now = GetTickCount64();
  unsigned held = g_hold_action.load();
  if (held && now >= g_hold_until.load()) {
    g_hold_action.store(0);
    give_action(held, 2);
  }
  if (!g_mine.on) return;
  if (!in_session()) {
    g_mine.on = false;
    return;
  }
  if (revive::use_held()) g_mine.last = now;  // still holding Use: still at it
  {  // ... or still standing at it (the use hangs the player on a wheel: within a step of it)
    uint8_t* level = game::ref<uint8_t*>(game::kLevel);
    uint8_t* own = level ? *reinterpret_cast<uint8_t**>(level + game::kLevelControlled) : nullptr;
    uint8_t* thing = g_mine.n ? net::entity_by_id(g_mine.ids[0]) : nullptr;
    if (own && thing) {
      const float* a = reinterpret_cast<const float*>(own + game::kEntityPosition);
      const float* b = reinterpret_cast<const float*>(thing + game::kEntityPosition);
      float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
      if (dx * dx + dy * dy + dz * dz < 2.5f * 2.5f) g_mine.last = now;
    }
  }
  if (now - g_mine.last > 3000) {
    send_busy(false);
    g_mine.on = false;
    LOGI("drive: this player has let go of '%s'", name_of(g_mine.ids[0]));
  } else if (now - g_mine.sent > 1000) {
    send_busy(true);
    g_mine.sent = now;
  }
}

bool may_use(const uint16_t* ids, int n, int* by) {
  if (!g_cfg.net_drive || !in_session() || n <= 0) return true;
  int slot = busy_by(ids, n, GetTickCount64());
  if (slot < 0) return true;
  if (by) *by = slot;
  std::string who = players::name(slot);
  if (who.empty()) who = "Другой игрок";
  hud::notice(who + " уже использует это", (who == "Другой игрок" ? std::string("Another player") : who) + " is using it", 3);
  LOGI("drive: '%s' is player %d's now: this player's use is left out", name_of(ids[0]), slot);
  return false;
}

void used(const uint16_t* ids, int n) {
  if (!g_cfg.net_drive || n <= 0) return;
  uint64_t now = GetTickCount64();
  if (g_mine.on && (g_mine.n != n || memcmp(g_mine.ids, ids, n * sizeof(uint16_t)))) send_busy(false);  // (another thing now)
  g_mine.n = n > 4 ? 4 : n;
  memset(g_mine.ids, 0, sizeof(g_mine.ids));
  memcpy(g_mine.ids, ids, g_mine.n * sizeof(uint16_t));
  g_mine.on = true;
  g_mine.last = now;
  g_mine.sent = now;
  send_busy(true);
}

void remote_used(int slot, const uint16_t* ids, int n) {
  if (!g_cfg.net_drive) return;
  mark_busy(slot, ids, n > 4 ? 4 : n, GetTickCount64() + 3500);
}

bool acting(int slot) {
  if (!g_cfg.net_drive || slot < 0 || slot >= 33) return true;
  return GetTickCount64() < g_slot_until[slot] + 1000;
}

void lab_may_use(unsigned id) {
  engine::on_main([id] {
    uint16_t one = static_cast<uint16_t>(id);
    int by = -1;
    bool ok = may_use(&one, 1, &by);
    LOGI("lab: may this player use %X now: %s", id, ok ? "yes" : "no");
  });
}

void lab_hold(unsigned action, unsigned ms) {
  engine::on_main([action, ms] {
    g_hold_until.store(GetTickCount64() + ms);
    g_hold_action.store(action);
    if (action == 35) revive::lab_hold_use(ms);
    give_action(action, 1);
  });
}

}  // namespace drive
