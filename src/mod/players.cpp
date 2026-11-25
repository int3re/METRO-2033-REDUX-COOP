#include "mod/players.h"

#include <windows.h>

#include <cstring>
#include <mutex>
#include <vector>

#include "common/log.h"
#include "mod/engine.h"
#include "mod/game.h"
#include "mod/hud.h"
#include "mod/link.h"
#include "mod/net.h"
#include "mod/steamnet.h"

namespace players {
namespace {

constexpr int kSlots = 32;
constexpr size_t kLongest = 48;  // bytes of a name

Config g_cfg;
std::mutex g_lock;
std::string g_names[kSlots];      // the list as this PC has it
// the host
std::string g_told[kSlots];       // what the player in a slot said its name is
void* g_peer[kSlots] = {};        // ... and the connection it said it on
void* g_gone[kSlots] = {};        // the connection of a slot whose player has left through Steam: the engine itself
                                  // notices only when its silence limit runs out (about a minute)
uint32_t g_sent_for = ~0u;        // link::connections() at the last send of the list
// a joined game
uint32_t g_said_for = ~0u;        // link::connections() when this game told its name
std::string g_said;

// Cuts a name to what a list entry may hold: no control characters, kLongest bytes at most (whole UTF-8 letters).
std::string tidy(std::string s) {
  for (char& c : s)
    if (static_cast<unsigned char>(c) < 0x20) c = ' ';
  if (s.size() > kLongest) {
    size_t n = kLongest;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    s.resize(n);
  }
  size_t a = s.find_first_not_of(' ');
  size_t b = s.find_last_not_of(' ');
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::string mine() {
  std::string name = steamnet::my_name();
  return tidy(name.empty() ? g_cfg.net_name : name);
}

void say(const std::string* names) {
  std::string line;
  for (int i = 0; i < kSlots; ++i) {
    if (names[i].empty()) continue;
    if (!line.empty()) line += ", ";
    line += std::to_string(i + 1) + " " + names[i];
  }
  LOGI("players: %s", line.empty() ? "nobody" : line.c_str());
}

// Takes the new list, logs it and tells the player on the screen who came and who went.
void take(const std::string* names) {
  std::string before[kSlots];
  bool changed = false, had_any = false;
  {
    std::lock_guard<std::mutex> hold(g_lock);
    for (int i = 0; i < kSlots; ++i) {
      before[i] = g_names[i];
      had_any |= !before[i].empty();
      changed |= names[i] != g_names[i];
      g_names[i] = names[i];
    }
  }
  if (!changed) return;
  say(names);
  int me = game::ref<int>(game::kNetLocalPlayer);
  if (!had_any) {  // the first list of a session: who is here already
    std::string others;
    for (int i = 0; i < kSlots; ++i) {
      if (i == me || names[i].empty()) continue;
      if (!others.empty()) others += ", ";
      others += names[i];
    }
    if (!others.empty()) hud::notice("В игре: " + others, "In the game: " + others);
    return;
  }
  for (int i = 0; i < kSlots; ++i) {
    if (i == me) continue;
    if (before[i].empty() && !names[i].empty()) hud::notice(names[i] + " присоединяется к игре", names[i] + " is joining the game");
    if (!before[i].empty() && names[i].empty()) hud::notice(before[i] + " покидает игру", before[i] + " has left the game");
  }
}

void on_my_name(int from, const uint8_t* data, size_t size) {
  if (from < 1 || from >= kSlots) return;
  g_told[from] = tidy(std::string(reinterpret_cast<const char*>(data), size));
}

void on_names(int, const uint8_t* data, size_t size) {
  if (game::ref<int>(game::kNetLocalPlayer) == 0 || size < 1) return;  // the host's own list is the list
  std::string names[kSlots];
  size_t at = 1;
  for (unsigned i = 0; i < data[0] && at + 2 <= size; ++i) {
    unsigned slot = data[at], length = data[at + 1];
    at += 2;
    if (at + length > size) break;
    if (slot < kSlots) names[slot] = tidy(std::string(reinterpret_cast<const char*>(data + at), length));
    at += length;
  }
  take(names);
}

void host_tick() {
  net::Players& p = game::ref<net::Players>(game::kNetPlayers);
  // Who left through Steam: the body goes and the name comes off the list now, not a minute later.
  uint32_t left_ip;
  while (steamnet::next_left(left_ip)) {
    for (int i = 1; i < p.size && i < kSlots && p.array; ++i) {
      void* peer = *reinterpret_cast<void**>(p.array + game::kNetPlayerSize * i + game::kNetPlayerPeer);
      if (!peer || link::address_of_slot(i) != left_ip) continue;
      g_gone[i] = peer;
      bool body = net::remove_body(i);
      LOGI("players: player %d left through Steam%s", i, body ? ", its body is taken out" : "");
    }
  }
  std::string names[kSlots];
  names[0] = mine();
  if (names[0].empty()) names[0] = "Игрок 1";
  for (int i = 1; i < p.size && i < kSlots && p.array; ++i) {
    void* peer = *reinterpret_cast<void**>(p.array + game::kNetPlayerSize * i + game::kNetPlayerPeer);
    if (peer != g_peer[i]) {
      g_peer[i] = peer;
      g_told[i].clear();
    }
    unsigned state = (*reinterpret_cast<const uint32_t*>(p.array + game::kNetPlayerSize * i + 0x68) >> 16) & 7;
    if (g_gone[i] && (peer != g_gone[i] || state == 0 || state == 6)) g_gone[i] = nullptr;  // the engine has let the slot go
    if (!peer || g_gone[i]) continue;
    uint32_t ip = link::address_of_slot(i);
    // Whoever came through Steam is who Steam says, whatever its game tells.
    std::string name = steamnet::is_ours(ip) ? tidy(steamnet::name_at(ip)) : "";
    if (name.empty()) name = g_told[i];
    if (name.empty()) name = "Игрок " + std::to_string(i + 1);
    names[i] = name;
  }
  bool changed = false;
  {
    std::lock_guard<std::mutex> hold(g_lock);
    for (int i = 0; i < kSlots; ++i) changed |= names[i] != g_names[i];
  }
  take(names);
  if (!changed && g_sent_for == link::connections()) return;
  std::vector<uint8_t> list(1, 0);
  for (int i = 0; i < kSlots; ++i) {
    if (names[i].empty()) continue;
    ++list[0];
    list.push_back(static_cast<uint8_t>(i));
    list.push_back(static_cast<uint8_t>(names[i].size()));
    list.insert(list.end(), names[i].begin(), names[i].end());
  }
  if (link::peers() == 0 || link::send(link::kNames, list.data(), list.size(), true)) g_sent_for = link::connections();
}

void join_tick() {
  std::string name = mine();
  if (name.empty() || link::peers() == 0) return;
  if (g_said_for == link::connections() && g_said == name) return;
  if (link::send(link::kMyName, name.data(), name.size(), true)) {
    g_said_for = link::connections();
    g_said = name;
  }
}

// Every frame: a name over the head of every other player. A slot's "network position" is its player's eyes
// (the camera), sent by that player's game.
void frame() {
  std::vector<hud::Tag> list;
  if (g_cfg.net_names && game::ref<uintptr_t>(game::kNetHost) > 1) {
    net::Players& p = game::ref<net::Players>(game::kNetPlayers);
    int me = game::ref<int>(game::kNetLocalPlayer);
    std::lock_guard<std::mutex> hold(g_lock);
    for (int i = 0; i < p.size && i < kSlots && p.array; ++i) {
      if (i == me || g_names[i].empty()) continue;
      const uint8_t* slot = p.array + game::kNetPlayerSize * i;
      if (*reinterpret_cast<const uint16_t*>(slot + 0x64) == 0xFFFF) continue;  // no body (yet)
      const float* eyes = reinterpret_cast<const float*>(slot + 0x48);
      list.push_back({eyes[0], eyes[1] + 0.32f, eyes[2], g_names[i]});
    }
  }
  hud::tags(std::move(list));
}

void tick() {
  frame();
  static uint64_t last = 0;
  uint64_t now = GetTickCount64();
  if (now - last < 500) return;
  last = now;
  if (game::ref<uintptr_t>(game::kNetHost) <= 1) {  // no session
    bool had = false;
    {
      std::lock_guard<std::mutex> hold(g_lock);
      for (std::string& n : g_names) {
        had |= !n.empty();
        n.clear();
      }
    }
    if (had) LOGI("players: the session is over");
    return;
  }
  if (game::ref<int>(game::kNetLocalPlayer) == 0)
    host_tick();
  else
    join_tick();
}

}  // namespace

std::string name(int slot) {
  if (slot < 0 || slot >= kSlots) return "";
  std::lock_guard<std::mutex> hold(g_lock);
  return g_names[slot];
}

void install(const Config& cfg) {
  g_cfg = cfg;
  link::on(link::kMyName, &on_my_name);
  link::on(link::kNames, &on_names);
  engine::on_tick(&tick);
}

}  // namespace players
