#include "mod/physics.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "common/log.h"
#include "mod/game.h"
#include "mod/link.h"

namespace net {
uint8_t* entity_by_id(uint16_t id);
uint8_t* parent_of(uint8_t* e);
}  // namespace net

namespace physics {
namespace {

// --- the game ------------------------------------------------------------------------------------------------
// A plain physical object of a level (the class whose "play a motion by name", vtbl + 0xAE0, is exe+236980 - the
// one that makes the object's shell kinematic for a motion) keeps its physical shell at + 0x318; the shell's
// vtbl + 0x90 (8) answers whether it is kinematic now (a door playing its motion, a lift: the motions module's
// business, not this one's).
constexpr uintptr_t kPlainPlay = 0x236980;
constexpr size_t kVtblPlay = 0xAE0;
constexpr size_t kShell = 0x318;
constexpr size_t kShellKinematic = 0x90;

#pragma pack(push, 1)
struct Note {
  uint16_t id;
  uint32_t name;  // a hash of the thing's name (to be sure it is the same thing)
  float m[12];    // its matrix: right, up, forward (x, y, z each), then the place
};
#pragma pack(pop)
static_assert(sizeof(Note) == 54, "Note");
constexpr size_t kMostPerPack = 40;

struct Track {
  float m[12] = {};
  bool known = false;
  uint64_t led_until = 0;  // another PC moves it: this PC only follows until then
  uint64_t said_at = 0;
};

Config g_cfg;
std::unordered_map<uint16_t, Track> g_tracks;
void* g_level = nullptr;
uint64_t g_level_at = 0;  // when this level (or the session in it) began: its physics settles by itself first
uint64_t g_last_scan = 0;
int g_sent = 0, g_applied = 0;

int my_slot() { return game::ref<int>(game::kNetLocalPlayer); }

bool in_session() {
  return game::ref<uintptr_t>(game::kNetHost) > 1 && game::ref<uint8_t>(game::kNetGame) && my_slot() >= 0 && link::peers() > 0;
}

uint32_t hash(const char* s) {
  uint32_t h = 2166136261u;
  for (; *s; ++s) h = (h ^ static_cast<uint8_t>(*s)) * 16777619u;
  return h;
}

const char* name_of(const uint8_t* e) {
  const uint8_t* str = *reinterpret_cast<uint8_t* const*>(e + game::kEntityName);
  return str ? reinterpret_cast<const char*>(str) + 0x14 : "";
}

// How many motions the thing's skeleton has (doors, cabinets, hatches have theirs: such a thing is the motions
// module's, never this one's - 0.4.2: a door's pose sent by physics opened it on every PC).
unsigned motion_count(uint8_t* e) {
  __try {
    uint8_t* holder = *reinterpret_cast<uint8_t**>(e + 0x100);
    if (!holder) return 0;
    using ModelFn = uint8_t* (*)(uint8_t*);
    uint8_t* model = reinterpret_cast<ModelFn>((*reinterpret_cast<void***>(holder))[3])(holder);
    uint8_t* a = model ? *reinterpret_cast<uint8_t**>(model + 64) : nullptr;
    uint8_t* b = a ? *reinterpret_cast<uint8_t**>(a + 24) : nullptr;
    uint8_t* table = b ? *reinterpret_cast<uint8_t**>(b + 2488) : nullptr;
    return table ? *reinterpret_cast<uint16_t*>(table + 50) : 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 1;
  }
}

// A loose physical thing of the level that its physics moves now (not a character, nothing carried, not a copy
// the session made, nothing of the players').
bool loose(uint8_t* e) {
  if (!e || *reinterpret_cast<uint16_t*>(e + game::kEntityId) == 0xFFFF) return false;
  if (*reinterpret_cast<uint32_t*>(e + game::kEntityFlags) & (4u | 0x10000u)) return false;
  if ((*reinterpret_cast<void***>(e))[kVtblPlay / 8] != reinterpret_cast<void*>(game::at(kPlainPlay))) return false;
  if (net::parent_of(e)) return false;
  if (!strncmp(name_of(e), "player", 6)) return false;
  uint8_t* shell = *reinterpret_cast<uint8_t**>(e + kShell);
  if (!shell) return false;
  if (motion_count(e) > 0) return false;
  using KinematicFn = uint32_t (*)(uint8_t*, int);
  return !reinterpret_cast<KinematicFn>((*reinterpret_cast<void***>(shell))[kShellKinematic / 8])(shell, 8);
}

void read_matrix(const uint8_t* e, float out[12]) {
  const float* m = reinterpret_cast<const float*>(e + game::kEntityMatrix);
  for (int row = 0; row < 4; ++row)
    for (int k = 0; k < 3; ++k) out[row * 3 + k] = m[row * 4 + k];
}

// How far apart two matrices are: metres of place, plus the rotation's difference
float apart(const float a[12], const float b[12]) {
  float d = 0.f;
  for (int k = 9; k < 12; ++k) d += (a[k] - b[k]) * (a[k] - b[k]);
  float r = 0.f;
  for (int k = 0; k < 9; ++k) r += fabsf(a[k] - b[k]);
  return sqrtf(d) + r * 0.1f;
}

void put(uint8_t* e, const float n[12]) {
  float m[16];
  memcpy(m, e + game::kEntityMatrix, sizeof(m));
  for (int row = 0; row < 4; ++row)
    for (int k = 0; k < 3; ++k) m[row * 4 + k] = n[row * 3 + k];
  using SetMatrix = void (*)(uint8_t*, const float*, int);
  (*reinterpret_cast<SetMatrix**>(e))[game::kEntitySetMatrix / 8](e, m, 0);
}

void send(const std::vector<Note>& notes, uint8_t from) {
  for (size_t i = 0; i < notes.size(); i += kMostPerPack) {
    size_t k = notes.size() - i < kMostPerPack ? notes.size() - i : kMostPerPack;
    std::vector<uint8_t> pack(2 + k * sizeof(Note));
    pack[0] = from;
    pack[1] = static_cast<uint8_t>(k);
    memcpy(pack.data() + 2, notes.data() + i, k * sizeof(Note));
    link::send(link::kPhysics, pack.data(), pack.size(), false);
  }
}

// Another PC's physics moved things: this PC puts its copies there (and the host passes the word on).
void on_note(int, const uint8_t* data, size_t size) {
  if (size < 2 || !g_cfg.net_physics) return;
  uint8_t from = data[0];
  size_t k = data[1];
  if (size < 2 + k * sizeof(Note)) return;
  if (from == static_cast<uint8_t>(my_slot())) return;  // its own word, back from the host
  if (my_slot() == 0 && link::peers() > 1) link::send(link::kPhysics, data, 2 + k * sizeof(Note), true);
  if (!game::ref<void*>(game::kLevel) || game::ref<int>(game::kMapState) == 2) return;
  uint64_t now = GetTickCount64();
  for (size_t i = 0; i < k; ++i) {
    Note n;
    memcpy(&n, data + 2 + i * sizeof(Note), sizeof(n));
    uint8_t* e = net::entity_by_id(n.id);
    if (!e || hash(name_of(e)) != n.name || net::parent_of(e)) continue;
    if (*reinterpret_cast<uint32_t*>(e + game::kEntityFlags) & (4u | 0x10000u)) continue;
    if ((*reinterpret_cast<void***>(e))[kVtblPlay / 8] != reinterpret_cast<void*>(game::at(kPlainPlay))) continue;
    if (motion_count(e) > 0) continue;  // a door, a cabinet: never put by physics
    Track& t = g_tracks[n.id];
    put(e, n.m);
    read_matrix(e, t.m);
    t.known = true;
    t.led_until = now + 1500;
    if (g_applied++ < 20) LOGI("physics: %X '%s' put where player %u's physics has it", n.id, name_of(e), from);
  }
}

}  // namespace

void install(const Config& cfg) {
  g_cfg = cfg;
  if (!cfg.net_physics) return;
  link::on(link::kPhysics, &on_note);
}

void tick() {
  if (!g_cfg.net_physics) return;
  uint64_t now = GetTickCount64();
  if (now - g_last_scan < 100) return;
  g_last_scan = now;
  void* level = game::ref<void*>(game::kLevel);
  if (level != g_level) {
    g_level = level;
    g_level_at = now;
    g_tracks.clear();
  }
  if (!in_session() || !level || game::ref<int>(game::kMapState) == 2) {
    g_level_at = now;
    return;
  }
  bool settling = now - g_level_at < 5000;
  uint8_t* server = game::ref<uint8_t*>(game::kServer);
  if (!server) return;
  uint8_t* table = *reinterpret_cast<uint8_t**>(server + game::kServerEntities);
  unsigned count = *reinterpret_cast<uint16_t*>(server + game::kServerEntityCount);
  if (!table) return;
  std::vector<Note> moved;
  for (unsigned i = 0; i < count; ++i) {
    uint8_t* e = *reinterpret_cast<uint8_t**>(table + i * 16 + 8);
    if (!e) continue;
    uint16_t id = *reinterpret_cast<uint16_t*>(e + game::kEntityId);
    auto found = g_tracks.find(id);
    if (!loose(e)) {
      if (found != g_tracks.end()) g_tracks.erase(found);
      continue;
    }
    float m[12];
    read_matrix(e, m);
    if (found == g_tracks.end()) {  // first seen: where it lies is what the level (the host's save) put there
      Track& t = g_tracks[id];
      memcpy(t.m, m, sizeof(m));
      t.known = true;
      continue;
    }
    Track& t = found->second;
    if (apart(m, t.m) < 0.02f) continue;
    memcpy(t.m, m, sizeof(m));
    if (now < t.led_until || settling) continue;  // another PC moves it, or the level is just settling
    Note n{id, hash(name_of(e)), {}};
    memcpy(n.m, m, sizeof(m));
    moved.push_back(n);
    t.said_at = now;
  }
  // The last word about a thing that has come to rest must not be lost (the messages above may be): once it lies
  // still for half a second after moving, it is said once more, reliably.
  std::vector<Note> rest;
  for (auto& [id, t] : g_tracks) {
    if (!t.said_at || now - t.said_at < 500) continue;
    t.said_at = 0;
    uint8_t* e = net::entity_by_id(id);
    if (!e) continue;
    Note n{id, hash(name_of(e)), {}};
    memcpy(n.m, t.m, sizeof(n.m));
    rest.push_back(n);
  }
  uint8_t me = static_cast<uint8_t>(my_slot());
  if (!moved.empty()) {
    send(moved, me);
    if ((g_sent += static_cast<int>(moved.size())) < 200)
      LOGD("physics: %zu things moved here, said to the others", moved.size());
  }
  for (size_t i = 0; i < rest.size(); i += kMostPerPack) {
    size_t k = rest.size() - i < kMostPerPack ? rest.size() - i : kMostPerPack;
    std::vector<uint8_t> pack(2 + k * sizeof(Note));
    pack[0] = me;
    pack[1] = static_cast<uint8_t>(k);
    memcpy(pack.data() + 2, rest.data() + i, k * sizeof(Note));
    link::send(link::kPhysics, pack.data(), pack.size(), true);
  }
}

}  // namespace physics
