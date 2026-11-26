#include "mod/motions.h"

#include <windows.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/log.h"
#include "mod/engine.h"
#include "mod/game.h"
#include "mod/link.h"
#include "mod/patch.h"

namespace net {
uint8_t* entity_by_id(uint16_t id);
uint8_t* parent_of(uint8_t* e);
}  // namespace net

namespace motions {
namespace {

// --- the game ------------------------------------------------------------------------------------------------
// Every motion of a thing with a simple skeleton starts here: the level script's "play motion" (through the
// entity's vtbl + 0xAE0, exe+236980 for plain objects, then exe+6A7410 by name), the visual script's motion
// control (exe+7247B0), and others. Returns the motion object (or null): + 8 float time (s), + 0xC float length
// (s), + 0x10 the motion's number, + 0x67 bits (2: looped). The start frame (30 a second) gives its first time.
constexpr uintptr_t kStartMotion = 0x6A71A0;  // motion* (entity*, u16 motion, u16 bone part, float speed, u16 start frame,
                                              //          char looped (0 the motion's own, 1 yes, 2 no), params*, int raw)
constexpr uintptr_t kPlainPlay = 0x236980;    // the plain object's "play by name" (vtbl + 0xAE0): makes a physical
                                              // object's shell kinematic for the motion and back at its end
constexpr uintptr_t kShellBack = 0x2368F0;    // ... the end of such a motion: the shell back, then the caller's delegate
constexpr size_t kVtblPlay = 0xAE0;
constexpr size_t kModelHolder = 0x100;        // entity + : object whose vtbl[3]() is the entity's model
constexpr size_t kShell = 0x318;              // entity + : the physical shell (vtbl + 0x90 (8): kinematic?; + 0x68 (on))
constexpr size_t kEndDelegate = 0x358;        // entity + : the caller's delegate kept while kPlainPlay's motion runs
constexpr size_t kEntityLock = 0xA8;          // entity + : u32 thread id holding the entity (script actions take it)
constexpr size_t kMotionTime = 8, kMotionLength = 0xC, kMotionNumber = 0x10;

#pragma pack(push, 1)
struct Note {          // one motion the host started on a thing
  uint16_t id;         // the thing (an entity of the level: the same id on every PC)
  uint32_t name;       // a hash of the thing's name (to be sure it is the same thing)
  uint16_t motion;     // the motion's number in the thing's skeleton
  uint16_t part;       // bone part
  float speed;
  float at;            // the motion's time when sent (s): a joined game starts it there
  uint8_t looped;      // as the game's argument
  uint8_t raw;         // as the game's argument
};
#pragma pack(pop)
static_assert(sizeof(Note) == 20, "Note");

// What the game hands kStartMotion as its last but one argument when it has nothing to say (exe+7247B0 does the
// same: a delegate of nobody, an empty value, a one at the end).
struct Params {
  uint64_t delegate_object = 0;
  uint64_t delegate_fn = 0;
  uint64_t delegate_more = 0;
  uint8_t value[32] = {};  // the type at + 24 = 0: nothing
  uint64_t name = 0;
  uint64_t more[3] = {};
  int32_t one = 1;
  int32_t pad = 0;
};
static_assert(sizeof(Params) == 0x60, "Params");

Config g_cfg;
std::atomic<uint64_t> g_trace_until{0};
std::atomic<int> g_traced{0};
thread_local bool t_applying = false;  // the mod itself starts a motion: not this PC's own start

struct Started {       // the last motion started on a thing on this PC
  uint16_t motion = 0xFFFF;
  uint64_t tick = 0;
  bool synced = false; // started by the mod for the host
  uint8_t* object = nullptr;
  float at = 0;        // its time when started
  float length = 0;
};

std::mutex g_lock;
std::unordered_map<uint16_t, Started> g_started;    // this PC: per thing
std::unordered_map<uint16_t, Note> g_state;         // the host: the last motion sent per thing (for a late joiner)
std::unordered_map<uint16_t, uint64_t> g_sent_at;   // the host: when it was sent
std::unordered_map<uint16_t, uint8_t*> g_state_object;
std::vector<Note> g_outbox;                         // the host: to send at the next tick
std::vector<std::pair<Note, uint64_t>> g_inbox;     // to play when the level runs (note, when it came)
std::vector<uint8_t> g_relay;                       // the host: joined games' notes to pass on to the others
std::atomic<bool> g_loading{false};
std::atomic<uint32_t> g_level_seq{0};
uint32_t g_asked_seq = 0;                           // a joined game: the level it asked the host's state for
std::atomic<int> g_sent{0}, g_played{0}, g_skipped{0};

const char* text_of(const uint8_t* str) { return str ? reinterpret_cast<const char*>(str) + 0x14 : ""; }

uint16_t id_of(const uint8_t* e) { return *reinterpret_cast<const uint16_t*>(e + game::kEntityId); }
uint32_t flags_of(const uint8_t* e) { return *reinterpret_cast<const uint32_t*>(e + game::kEntityFlags); }
const char* name_of(const uint8_t* e) { return text_of(*reinterpret_cast<uint8_t* const*>(e + game::kEntityName)); }

uint32_t hash(const char* s) {
  uint32_t h = 2166136261u;
  for (; *s; ++s) h = (h ^ static_cast<uint8_t>(*s)) * 16777619u;
  return h;
}

int my_slot() { return game::ref<int>(game::kNetLocalPlayer); }

bool in_session() {
  return game::ref<uintptr_t>(game::kNetHost) > 1 && game::ref<uint8_t>(game::kNetGame) && my_slot() >= 0 && link::peers() > 0;
}

bool level_runs() { return game::ref<void*>(game::kLevel) && game::ref<int>(game::kMapState) != 2 && !g_loading.load(); }

// The motion table of the thing's skeleton (null when it has none).
uint8_t* motion_table(uint8_t* e) {
  __try {
    uint8_t* holder = *reinterpret_cast<uint8_t**>(e + kModelHolder);
    if (!holder) return nullptr;
    using ModelFn = uint8_t* (*)(uint8_t*);
    uint8_t* model = reinterpret_cast<ModelFn>((*reinterpret_cast<void***>(holder))[3])(holder);
    if (!model) return nullptr;
    uint8_t* a = *reinterpret_cast<uint8_t**>(model + 64);
    uint8_t* b = a ? *reinterpret_cast<uint8_t**>(a + 24) : nullptr;
    return b ? *reinterpret_cast<uint8_t**>(b + 2488) : nullptr;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

// The name of a motion of the thing's skeleton by its number (null when there is none).
const char* motion_text(uint8_t* e, uint16_t motion) {
  uint8_t* table = motion_table(e);
  if (!table) return nullptr;
  __try {
    if (*reinterpret_cast<uint16_t*>(table + 50) <= motion) return nullptr;
    uint8_t* entry = *reinterpret_cast<uint8_t**>(table + 40) + 16ull * motion;
    uint8_t* str = *reinterpret_cast<uint8_t**>(entry + 8);
    return str ? reinterpret_cast<const char*>(str) + 0x14 : nullptr;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

std::string motion_name(uint8_t* e, uint16_t motion) {
  const char* t = motion_text(e, motion);
  return t ? std::string(t) : std::string("?");
}

// An idle: a motion a thing plays on by itself (a wire swaying, a cobweb) - each PC's own.
bool ambient(uint8_t* e, uint16_t motion) {
  const char* t = motion_text(e, motion);
  if (!t) return true;
  char low[96];
  size_t i = 0;
  for (; t[i] && i + 1 < sizeof(low); ++i) low[i] = static_cast<char>(tolower(static_cast<unsigned char>(t[i])));
  low[i] = 0;
  return strstr(low, "idle") != nullptr;
}

// A motion object's time and length, if it still is the motion it was (it may be gone: read with care).
bool read_motion(uint8_t* m, uint16_t motion, float* time, float* length) {
  if (!m) return false;
  __try {
    if (*reinterpret_cast<uint16_t*>(m + kMotionNumber) != motion) return false;
    *time = *reinterpret_cast<float*>(m + kMotionTime);
    *length = *reinterpret_cast<float*>(m + kMotionLength);
    return *length > 0.f && *length < 36000.f && *time >= 0.f;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// A thing of the level whose motions every PC should see alike: not a character, nothing a character or a player
// carries, not a copy the session spawned, nothing of the player's own ("player_*": hands, death cameras).
bool shared_thing(uint8_t* e) {
  if (!e) return false;
  uint16_t id = id_of(e);
  if (id == 0xFFFF) return false;
  if (flags_of(e) & (4u | 0x10000u)) return false;
  if (!strncmp(name_of(e), "player", 6)) return false;
  uint8_t* p = e;
  for (int depth = 0; depth < 8; ++depth) {
    p = net::parent_of(p);
    if (!p) break;
    if (flags_of(p) & 4u) return false;
    if (*reinterpret_cast<uintptr_t*>(p) == game::at(game::kPlayerClass)) return false;
  }
  return true;
}

// A thing this PC's own player moved (0.4.3): the level runs, the body has been in the game 10 s or more (what the
// level's load and the first moment set is not news), and the thing is within 10 m of the body.
uint8_t* g_body_seen = nullptr;
uint64_t g_body_since = 0;

bool near_own_body(uint8_t* e, uint64_t now) {
  uint8_t* level = game::ref<uint8_t*>(game::kLevel);
  uint8_t* own = level ? *reinterpret_cast<uint8_t**>(level + game::kLevelControlled) : nullptr;
  if (own != g_body_seen) {
    g_body_seen = own;
    g_body_since = now;
  }
  if (!own || now - g_body_since < 10000) return false;
  const float* a = reinterpret_cast<const float*>(own + game::kEntityPosition);
  const float* b = reinterpret_cast<const float*>(e + game::kEntityPosition);
  float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
  return dx * dx + dy * dy + dz * dz < 10.f * 10.f;
}

std::string chain(int skip, int count) {
  void* frames[16] = {};
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

// --- every motion a thing starts --------------------------------------------------------------------------------
using StartMotionFn = uint8_t* (*)(uint8_t*, uint16_t, uint16_t, float, uint16_t, char, void*, int);
StartMotionFn g_start_motion = nullptr;

uint8_t* on_start_motion(uint8_t* e, uint16_t motion, uint16_t part, float speed, uint16_t start, char looped, void* params,
                         int raw) {
  bool mine = !t_applying;
  uint16_t id = e ? id_of(e) : 0xFFFF;
  uint64_t now = GetTickCount64();
  // The same motion the host's note started a moment ago: this PC's own script has come to it later. The motion
  // starts anew for the script (its delegate at the end), but at the time the host's has come to, so that the thing
  // does not jump back. (Read before the start: the game may reuse the motion's object.)
  float catch_up = -1.f;
  if (mine && e) {
    std::lock_guard<std::mutex> hold(g_lock);
    auto s = g_started.find(id);
    if (s != g_started.end() && s->second.synced && s->second.motion == motion && now - s->second.tick > 100) {
      float t = 0, l = 0;
      catch_up = read_motion(s->second.object, motion, &t, &l) ? t : s->second.at + (now - s->second.tick) / 1000.f;
    }
  }
  uint8_t* m = g_start_motion(e, motion, part, speed, start, looped, params, raw);
  if (!e || !m) return m;
  float time = 0, length = 0;
  read_motion(m, motion, &time, &length);
  if (catch_up > time) {
    if (length > 0.f && catch_up > length) catch_up = length;
    *reinterpret_cast<float*>(m + kMotionTime) = catch_up;
    LOGD("motions: %X '%s' starts '%s' itself after the host's: on at %.2f s", id, name_of(e), motion_name(e, motion).c_str(),
         catch_up);
    time = catch_up;
  }

  uint16_t prev_motion = 0xFFFF;  // what this PC started on the thing before
  uint64_t prev_tick = 0;
  if (mine) {
    std::lock_guard<std::mutex> hold(g_lock);
    Started& s = g_started[id];
    prev_motion = s.motion;
    prev_tick = s.tick;
    s.motion = motion;
    s.tick = now;
    s.synced = false;
    s.object = m;
    s.at = time;
    s.length = length;
  }

  // The host tells the joined games: a new motion of a thing, or the same one again after a while (a lever pulled
  // twice). A thing whose script starts its idle anew every second or two is not news.
  bool joined = game::ref<uint8_t>(game::kNetGame) && my_slot() > 0;
  if (mine && !joined && shared_thing(e)) {  // (also before any session: a later joiner asks for all of it)
    std::lock_guard<std::mutex> hold(g_lock);
    auto last = g_state.find(id);
    uint64_t& sent_at = g_sent_at[id];
    bool again = prev_motion == motion && now - prev_tick > 5000;
    if (last == g_state.end() || last->second.motion != motion || again) {
      Note n{id, hash(name_of(e)), motion, part, speed, time, static_cast<uint8_t>(looped), static_cast<uint8_t>(raw)};
      g_state[id] = n;
      g_state_object[id] = m;
      sent_at = now;
      if (in_session() && g_outbox.size() < 4096) g_outbox.push_back(n);
    }
  }
  // A joined game tells the host what its own script moved (a door its player's zone or use opened, which the
  // host's script may never come to): one-off motions only - an idle that plays on by itself is each PC's own.
  if (g_cfg.net_motions_back && mine && joined && in_session() && level_runs() && shared_thing(e) && !ambient(e, motion) &&
      near_own_body(e, now)) {
    std::lock_guard<std::mutex> hold(g_lock);
    bool again = prev_motion == motion && now - prev_tick > 5000;
    if (prev_motion != motion || again) {
      Note n{id, hash(name_of(e)), motion, part, speed, time, static_cast<uint8_t>(looped), static_cast<uint8_t>(raw)};
      if (g_outbox.size() < 4096) g_outbox.push_back(n);
    }
  }

  if (GetTickCount64() < g_trace_until.load()) {
    static uint16_t last[65536];  // the lab: only changes are logged
    if (last[id] != motion + 1 && g_traced.fetch_add(1) < 600)
      LOGI("lab: motion %X '%s' flags %X: #%u '%s' part %u speed %.2f start %u looped %d raw %d -> time %.2f of %.2f%s, by%s", id,
           name_of(e), flags_of(e), motion, motion_name(e, motion).c_str(), part, speed, start, looped, raw, time, length,
           mine ? "" : " (for the host)", chain(1, 7).c_str());
    last[id] = motion + 1;
  }
  return m;
}

// --- a joined game plays what the host started --------------------------------------------------------------------
// As the game does it: the entity's lock (a script action holds it while it plays), a physical object's shell
// kinematic for the motion as exe+236980 makes it.
std::unordered_map<uint16_t, uint64_t> g_own_use;  // thing -> until when this PC's own use moves it

bool play(const Note& n, uint64_t came) {
  uint8_t* e = net::entity_by_id(n.id);
  if (!e) return false;
  {
    std::lock_guard<std::mutex> hold(g_lock);
    auto own = g_own_use.find(n.id);
    if (own != g_own_use.end() && GetTickCount64() < own->second) {
      LOGD("motions: '%s' is in this player's own use: the other PC's '%s' is not played", name_of(e), motion_name(e, n.motion).c_str());
      return true;
    }
  }
  if (hash(name_of(e)) != n.name || !shared_thing(e)) {
    LOGD("motions: the host's %X is another thing here ('%s'): left alone", n.id, name_of(e));
    return true;
  }
  if (!motion_text(e, n.motion)) {
    LOGD("motions: '%s' has no motion %u here: left alone", name_of(e), n.motion);
    return true;
  }
  uint64_t now = GetTickCount64();
  {
    std::lock_guard<std::mutex> hold(g_lock);
    auto s = g_started.find(n.id);
    // this PC's own script started the same motion lately: the same thing happened here already - only, if the
    // host's is further on (this PC's script came to it later), this one is set to where the host's is
    if (s != g_started.end() && !s->second.synced && s->second.motion == n.motion && now - s->second.tick < 20000) {
      ++g_skipped;
      float host_at = n.at + (now - came) / 1000.f, t = 0, l = 0;
      if (read_motion(s->second.object, n.motion, &t, &l) && host_at > t + 0.15f && t < l) {
        float to = host_at < l ? host_at : l;
        *reinterpret_cast<float*>(s->second.object + kMotionTime) = to;
        LOGD("motions: '%s' plays '%s' here already: on from %.2f to the host's %.2f s", name_of(e), motion_name(e, n.motion).c_str(), t, to);
      } else {
        LOGD("motions: '%s' plays '%s' here already (%.1f s ago)", name_of(e), motion_name(e, n.motion).c_str(),
             (now - s->second.tick) / 1000.f);
      }
      return true;
    }
  }
  float at = n.at + (now - came) / 1000.f;  // (the time it waited here)
  if (at < 0.f) at = 0.f;
  if (at > 2000.f) at = 2000.f;
  Params params;
  bool kinematic = false;
  uintptr_t play_fn = (*reinterpret_cast<uintptr_t**>(e))[kVtblPlay / 8];
  uint8_t* shell = *reinterpret_cast<uint8_t**>(e + kShell);
  if (play_fn == game::at(kPlainPlay) && !n.raw && shell) {
    using IsFn = int (*)(uint8_t*, int);
    using SetFn = void (*)(uint8_t*, char);
    void** vt = *reinterpret_cast<void***>(shell);
    if (!reinterpret_cast<IsFn>(vt[0x90 / 8])(shell, 8)) {
      reinterpret_cast<SetFn>(vt[0x68 / 8])(shell, 1);
      uintptr_t self = reinterpret_cast<uintptr_t>(e);
      params.delegate_object = (self & ~3ull) | 2ull;
      params.delegate_object &= 0x7FFFFFFFFFFFFFFFull;
      params.delegate_fn = game::at(kShellBack);
      *reinterpret_cast<uint64_t*>(e + kEndDelegate) = 0;
      *reinterpret_cast<uint64_t*>(e + kEndDelegate + 8) = 0;
      kinematic = true;
    }
  }
  volatile LONG* lock = reinterpret_cast<volatile LONG*>(e + kEntityLock);
  LONG me = static_cast<LONG>(GetCurrentThreadId());
  bool took = false;
  for (int spin = 0; spin < 100000; ++spin) {
    LONG was = InterlockedCompareExchange(lock, me, 0);
    if (was == 0) {
      took = true;
      break;
    }
    if (was == me) break;
    YieldProcessor();
  }
  t_applying = true;
  uint16_t frame = static_cast<uint16_t>(at * 30.f > 65535.f ? 65535.f : at * 30.f);
  uint8_t* m = reinterpret_cast<StartMotionFn>(game::at(kStartMotion))(e, n.motion, n.part, n.speed, frame, static_cast<char>(n.looped),
                                                                        &params, n.raw);
  t_applying = false;
  if (took) *lock = 0;
  float time = 0, length = 0;
  read_motion(m, n.motion, &time, &length);
  {
    std::lock_guard<std::mutex> hold(g_lock);
    Started& s = g_started[n.id];
    s.motion = n.motion;
    s.tick = now;
    s.synced = true;
    s.object = m;
    s.at = time;
    s.length = length;
  }
  ++g_played;
  LOGD("motions: '%s' (%X) plays '%s' as on the host, from %.2f s of %.2f%s%s", name_of(e), n.id, motion_name(e, n.motion).c_str(),
       time, length, kinematic ? " (its shell kinematic for it)" : "", m ? "" : " - FAILED");
  return true;
}

void on_note(int from, const uint8_t* data, size_t size) {
  // a joined game plays the host's; the host plays a joined game's and passes it on to the others
  if (size < 1 || (my_slot() != 0 && from != 0) || (my_slot() == 0 && from < 1)) return;
  if (my_slot() == 0 && !g_cfg.net_motions_back) return;  // (a 0.4.1 joined game still sends them: not taken)
  unsigned count = data[0];
  if (size < 1 + count * sizeof(Note)) return;
  uint64_t now = GetTickCount64();
  std::lock_guard<std::mutex> hold(g_lock);
  for (unsigned i = 0; i < count && g_inbox.size() < 8192; ++i) {
    Note n;
    memcpy(&n, data + 1 + i * sizeof(Note), sizeof(Note));
    g_inbox.push_back({n, now});
  }
  if (my_slot() == 0) g_relay.insert(g_relay.end(), data, data + size);
}

// A joined game whose level runs asks the host for the motions of the level's things so far (the host's save
// does not carry a running motion: a door opened before the join).
void on_ask(int from, const uint8_t*, size_t) {
  if (my_slot() != 0 || from < 1) return;
  std::vector<Note> all;
  {
    std::lock_guard<std::mutex> hold(g_lock);
    for (auto& [id, n] : g_state) {
      Note now = n;
      float t = 0, l = 0;
      if (read_motion(g_state_object[id], n.motion, &t, &l)) now.at = t;
      all.push_back(now);
    }
  }
  LOGI("motions: player %d asks for the things' motions: %zu", from, all.size());
  for (size_t i = 0; i < all.size(); i += 60) {
    size_t k = all.size() - i < 60 ? all.size() - i : 60;
    std::vector<uint8_t> pack(1 + k * sizeof(Note));
    pack[0] = static_cast<uint8_t>(k);
    memcpy(pack.data() + 1, all.data() + i, k * sizeof(Note));
    link::send(link::kMotions, pack.data(), pack.size(), true, from);
  }
}

void on_log(const char* line) {
  if (strstr(line, "loading content\\maps\\") || strstr(line, "load save bin")) {
    g_loading.store(true);
    std::lock_guard<std::mutex> hold(g_lock);
    g_started.clear();
    g_state.clear();
    g_sent_at.clear();
    g_state_object.clear();
    g_outbox.clear();
    g_inbox.clear();
  } else if (strstr(line, "loader_finish: mapstate_running")) {
    g_loading.store(false);
    ++g_level_seq;
  }
}

}  // namespace

void install(const Config& cfg) {
  g_cfg = cfg;
  if (!cfg.net_motions && !cfg.lab) return;
  patch::hook("start a motion", game::at(kStartMotion), reinterpret_cast<void*>(&on_start_motion), reinterpret_cast<void**>(&g_start_motion));
  if (!cfg.net_motions) return;
  link::on(link::kMotions, &on_note);
  link::on(link::kMotionsAsk, &on_ask);
  engine::on_log(&on_log);
}

void tick() {
  if (!g_cfg.net_motions || !g_start_motion) return;
  if (!(game::ref<uint8_t>(game::kNetGame) && my_slot() > 0)) {  // the host (or not in a session yet)
    std::vector<Note> out;
    {
      std::lock_guard<std::mutex> hold(g_lock);
      out.swap(g_outbox);
    }
    std::vector<uint8_t> relay;
    std::vector<std::pair<Note, uint64_t>> in;
    {
      std::lock_guard<std::mutex> hold(g_lock);
      relay.swap(g_relay);
      in.swap(g_inbox);
    }
    if (!in_session() || !level_runs()) return;
    for (size_t i = 0; i < out.size(); i += 60) {
      size_t k = out.size() - i < 60 ? out.size() - i : 60;
      std::vector<uint8_t> pack(1 + k * sizeof(Note));
      pack[0] = static_cast<uint8_t>(k);
      memcpy(pack.data() + 1, out.data() + i, k * sizeof(Note));
      link::send(link::kMotions, pack.data(), pack.size(), true);
    }
    g_sent += static_cast<int>(out.size());
    if (!relay.empty()) link::send(link::kMotions, relay.data(), relay.size(), true);  // (the one who sent it skips its own)
    for (auto& [n, came] : in) {  // what a joined game's own script moved: the host's thing moves too
      if (!play(n, came)) continue;
      uint8_t* e = net::entity_by_id(n.id);
      std::lock_guard<std::mutex> hold(g_lock);
      auto s = g_started.find(n.id);
      if (e && s != g_started.end() && s->second.synced) {  // (played here: a later joiner gets it from the host)
        g_state[n.id] = n;
        g_state_object[n.id] = s->second.object;
      }
    }
    return;
  }
  // a joined game
  if (!in_session() || !level_runs() || !game::ref<uint8_t*>(game::kLevel)) return;
  uint8_t* level = game::ref<uint8_t*>(game::kLevel);
  if (!*reinterpret_cast<uint8_t**>(level + game::kLevelControlled)) return;  // no body of its own yet
  uint32_t seq = g_level_seq.load();
  if (g_asked_seq != seq) {
    g_asked_seq = seq;
    link::send(link::kMotionsAsk, nullptr, 0, true);
  }
  {
    std::vector<Note> out;
    {
      std::lock_guard<std::mutex> hold(g_lock);
      out.swap(g_outbox);
    }
    for (size_t i = 0; i < out.size(); i += 60) {  // this game's own one-off motions, to the host
      size_t k = out.size() - i < 60 ? out.size() - i : 60;
      std::vector<uint8_t> pack(1 + k * sizeof(Note));
      pack[0] = static_cast<uint8_t>(k);
      memcpy(pack.data() + 1, out.data() + i, k * sizeof(Note));
      link::send(link::kMotions, pack.data(), pack.size(), true);
    }
  }
  std::vector<std::pair<Note, uint64_t>> in;
  {
    std::lock_guard<std::mutex> hold(g_lock);
    in.swap(g_inbox);
  }
  uint64_t now = GetTickCount64();
  std::vector<std::pair<Note, uint64_t>> later;
  for (auto& [n, came] : in)
    if (!play(n, came) && now - came < 30000) later.push_back({n, came});  // the thing is not there yet
  if (!later.empty()) {
    std::lock_guard<std::mutex> hold(g_lock);
    g_inbox.insert(g_inbox.begin(), later.begin(), later.end());
  }
}

void own_use(uint16_t id) {
  std::lock_guard<std::mutex> hold(g_lock);
  g_own_use[id] = GetTickCount64() + 15000;
}

void lab_trace(unsigned seconds) {
  g_traced.store(0);
  g_trace_until.store(GetTickCount64() + seconds * 1000ull);
  LOGI("lab: the motions of things are logged for %u s (sent %d, played for the host %d, already played here %d)", seconds,
       g_sent.load(), g_played.load(), g_skipped.load());
}

void lab_play(unsigned id, const std::string& name) {
  uint8_t* e = net::entity_by_id(static_cast<uint16_t>(id));
  if (!e) {
    LOGI("lab: play: no entity %X", id);
    return;
  }
  uint8_t* table = motion_table(e);
  unsigned count = table ? *reinterpret_cast<uint16_t*>(table + 50) : 0;
  for (unsigned i = 0; i < count; ++i) {
    const char* t = motion_text(e, static_cast<uint16_t>(i));
    if (t && name == t) {
      Params params;
      uint8_t* m = reinterpret_cast<StartMotionFn>(game::at(kStartMotion))(e, static_cast<uint16_t>(i), 0, 0.f, 0, 0, &params, 1);
      LOGI("lab: play: '%s' (%X) starts '%s' (#%u): %s", name_of(e), id, t, i, m ? "on" : "FAILED");
      return;
    }
  }
  std::string all;
  for (unsigned i = 0; i < count && all.size() < 600; ++i) {
    all += ' ';
    all += motion_name(e, static_cast<uint16_t>(i));
  }
  LOGI("lab: play: '%s' (%X) has no motion '%s'; it has %u:%s", name_of(e), id, name.c_str(), count, all.c_str());
}

void lab_list(unsigned id) {
  uint8_t* e = net::entity_by_id(static_cast<uint16_t>(id));
  uint8_t* table = e ? motion_table(e) : nullptr;
  unsigned count = table ? *reinterpret_cast<uint16_t*>(table + 50) : 0;
  char path[64];
  snprintf(path, sizeof(path), "motions-%X.txt", id);
  FILE* f = fopen(path, "w");
  if (!f) return;
  for (unsigned i = 0; i < count; ++i) fprintf(f, "%u\t%s\n", i, motion_name(e, static_cast<uint16_t>(i)).c_str());
  fclose(f);
  LOGI("lab: %u motions of %X '%s' written to %s", count, id, e ? name_of(e) : "-", path);
}

void lab_state(unsigned id) {
  std::lock_guard<std::mutex> hold(g_lock);
  auto s = g_started.find(static_cast<uint16_t>(id));
  uint8_t* e = net::entity_by_id(static_cast<uint16_t>(id));
  if (s == g_started.end() || !e) {
    LOGI("lab: motion of %X: none started here", id);
    return;
  }
  float t = -1, l = -1;
  bool live = read_motion(s->second.object, s->second.motion, &t, &l);
  LOGI("lab: motion of %X '%s': '%s' (#%u)%s, %.1f s ago, time %.2f of %.2f%s", id, name_of(e), motion_name(e, s->second.motion).c_str(),
       s->second.motion, s->second.synced ? " for the host" : " by this PC", (GetTickCount64() - s->second.tick) / 1000.f, t, l,
       live ? "" : " (its object is gone)");
}

}  // namespace motions
