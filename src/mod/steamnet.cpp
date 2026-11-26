#include "mod/steamnet.h"

#include <winsock2.h>
#include <windows.h>
#include <timeapi.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#include "common/log.h"
#include "common/version.h"
#include "mod/hud.h"
#include "mod/link.h"
#include "mod/patch.h"
#include "mod/steam.h"

namespace steamnet {
namespace {

Config g_cfg;
enum class Role { Host, Join, NameOnly, Find } g_role = Role::NameOnly;
bool g_auto = false;  // mode = auto: the role changes while the game runs (Find <-> Host, Find <-> Join)

// --- what the game's threads and the Steam thread share ----------------------------------------------------
struct Packet {
  uint32_t ip = 0;
  std::vector<uint8_t> data;
};
std::mutex g_lock;
std::map<uint32_t, uint64_t> g_users;     // made-up address -> SteamID (every user this game has heard of)
std::map<uint64_t, std::string> g_names;  // SteamID -> name
std::string g_my_name;
std::string g_host_address;
std::string g_level;
std::deque<Packet> g_out;                 // from the game, to Steam
std::deque<Packet> g_in;                  // from Steam, to the game
std::atomic<int> g_in_count{0};
std::atomic<bool> g_up{false};
uint16_t g_port = 0;                      // the port (network order) the game talks to on made-up addresses
HANDLE g_wake = nullptr;
// mode = auto: what the game's side asks of the Steam thread
std::atomic<bool> g_play_host{false};
std::atomic<bool> g_knocked{false};
std::atomic<bool> g_joining{false};
std::atomic<bool> g_leave{false};
std::atomic<uint64_t> g_offer_yes{0}, g_offer_no{0};
Offer g_offer;            // g_lock
bool g_have_offer = false;
// Every friend's game of this mod found lately (the main menu's "СЕТЕВАЯ ИГРА" page lists them): lobby -> game.
struct Seen {
  Offer game;
  uint64_t at = 0;
};
std::map<uint64_t, Seen> g_games;
std::vector<uint32_t> g_left;              // g_lock: made-up addresses of players who have left
std::atomic<uint64_t> g_host_heard_at{0};  // a joined game: when the host was heard last

// The made-up address of a Steam user: 27 bits mixed out of the account number (the low half of the SteamID),
// first byte 240 - 247. The same on every PC. g_lock held; the user is remembered for the way back.
uint32_t ip_of_user(uint64_t user) {
  uint32_t h = static_cast<uint32_t>(user) * 2654435761u;
  h ^= h >> 15;
  h &= 0x07FFFFFF;
  if (!(h & 0x00FFFFFF)) h |= 1;  // never x.0.0.0
  uint32_t ip = (240u + (h >> 24)) | ((h >> 16) & 0xFF) << 8 | ((h >> 8) & 0xFF) << 16 | (h & 0xFF) << 24;
  auto it = g_users.find(ip);
  if (it == g_users.end()) {
    g_users[ip] = user;
  } else if (it->second != user) {
    static bool said = false;
    if (!said) LOGE("steam: two Steam users of this session got the same made-up address (%llu and %llu): the second one cannot play",
                    static_cast<unsigned long long>(it->second), static_cast<unsigned long long>(user));
    said = true;
    return 0;
  }
  return ip;
}

uint64_t user_of_ip(uint32_t ip) {
  auto it = g_users.find(ip);
  return it == g_users.end() ? 0 : it->second;
}

std::string text_of_ip(uint32_t ip) {
  char text[32];
  snprintf(text, sizeof(text), "%u.%u.%u.%u", ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, ip >> 24);
  return text;
}

// --- the engine's two socket calls -------------------------------------------------------------------------
using SendToFn = int(WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const sockaddr*, int, LPWSAOVERLAPPED,
                               LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using RecvFromFn = int(WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, sockaddr*, LPINT, LPWSAOVERLAPPED,
                                 LPWSAOVERLAPPED_COMPLETION_ROUTINE);
SendToFn g_send_to = nullptr;
RecvFromFn g_recv_from = nullptr;

int WSAAPI on_send_to(SOCKET s, LPWSABUF bufs, DWORD count, LPDWORD sent, DWORD flags, const sockaddr* to, int tolen,
                      LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE routine) {
  if (to && tolen >= static_cast<int>(sizeof(sockaddr_in)) && to->sa_family == AF_INET) {
    const sockaddr_in* a = reinterpret_cast<const sockaddr_in*>(to);
    if (is_ours(a->sin_addr.s_addr)) {
      Packet p;
      p.ip = a->sin_addr.s_addr;
      size_t total = 0;
      for (DWORD i = 0; i < count; ++i) total += bufs[i].len;
      p.data.reserve(total);
      for (DWORD i = 0; i < count; ++i)
        p.data.insert(p.data.end(), reinterpret_cast<uint8_t*>(bufs[i].buf), reinterpret_cast<uint8_t*>(bufs[i].buf) + bufs[i].len);
      {
        std::lock_guard<std::mutex> hold(g_lock);
        g_port = a->sin_port;
        if (g_out.size() >= 4096) g_out.pop_front();  // Steam is not taking them: the oldest goes, as on a full socket
        g_out.push_back(std::move(p));
      }
      SetEvent(g_wake);
      if (sent) *sent = static_cast<DWORD>(total);
      return 0;
    }
  }
  return g_send_to(s, bufs, count, sent, flags, to, tolen, overlapped, routine);
}

int WSAAPI on_recv_from(SOCKET s, LPWSABUF bufs, DWORD count, LPDWORD received, LPDWORD flags, sockaddr* from, LPINT fromlen,
                        LPWSAOVERLAPPED overlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE routine) {
  if (g_in_count.load(std::memory_order_relaxed) > 0 && bufs && count && received) {
    Packet p;
    bool have = false;
    uint16_t port = 0;
    {
      std::lock_guard<std::mutex> hold(g_lock);
      if (!g_in.empty()) {
        p = std::move(g_in.front());
        g_in.pop_front();
        --g_in_count;
        have = true;
        port = g_port ? g_port : htons(1000);
      }
    }
    if (have) {
      size_t done = 0;
      for (DWORD i = 0; i < count && done < p.data.size(); ++i) {
        size_t n = p.data.size() - done < bufs[i].len ? p.data.size() - done : bufs[i].len;
        memcpy(bufs[i].buf, p.data.data() + done, n);
        done += n;
      }
      if (from && fromlen && *fromlen >= static_cast<int>(sizeof(sockaddr_in))) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = port;
        a.sin_addr.s_addr = p.ip;
        memcpy(from, &a, sizeof(a));
        *fromlen = sizeof(a);
      }
      *received = static_cast<DWORD>(done);
      if (flags) *flags = 0;
      return 0;
    }
  }
  return g_recv_from(s, bufs, count, received, flags, from, fromlen, overlapped, routine);
}

// --- the Steam thread ------------------------------------------------------------------------------------
std::unique_ptr<steam::Backend> g_steam;
uint64_t g_me = 0;
uint64_t g_lobby = 0;             // the host: its lobby; a joining game: the lobby it sits in
std::set<uint64_t> g_allowed;     // whose messages reach the game
std::map<uint64_t, std::string> g_link_said;  // per user: the connection's state said last
std::set<uint64_t> g_strangers;   // said once
std::map<uint64_t, uint64_t> g_heard_at;   // per user: when a message came last
std::map<uint64_t, uint64_t> g_going;      // users who are out of the lobby but were not silent long enough yet

void remember_name(uint64_t user) {
  std::string name = g_steam->name_of(user);
  if (name.empty()) return;
  std::lock_guard<std::mutex> hold(g_lock);
  g_names[user] = name;
}

std::string known_name(uint64_t user) {
  std::lock_guard<std::mutex> hold(g_lock);
  auto it = g_names.find(user);
  if (it != g_names.end() && !it->second.empty()) return it->second;
  char text[32];
  snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(user));
  return text;
}

// Who sits in the lobby now: their messages reach the game (the host talks to every player, and the players
// talk to each other).
void refresh_members() {
  if (!g_lobby) return;
  std::vector<uint64_t> members = g_steam->lobby_members(g_lobby);
  std::set<uint64_t> now;
  for (uint64_t m : members)
    if (m != g_me) now.insert(m);
  for (uint64_t m : now) {
    remember_name(m);
    if (!g_allowed.count(m)) {
      {
        std::lock_guard<std::mutex> hold(g_lock);
        ip_of_user(m);
      }
      LOGI("steam: %s came into the lobby", known_name(m).c_str());
    }
  }
  // Somebody who is out of the lobby is gone for the game only once nothing has come from it for a while: Steam
  // may drop a member for a moment while its game plays on.
  uint64_t tick = GetTickCount64();
  for (uint64_t m : g_allowed) {
    if (now.count(m)) {
      g_going.erase(m);
      continue;
    }
    if (!g_going.count(m)) g_going[m] = tick;
    uint64_t heard = g_heard_at.count(m) ? g_heard_at[m] : 0;
    if (tick - g_going[m] < 6000 || tick - heard < 6000) {
      now.insert(m);  // still let through
      continue;
    }
    LOGI("steam: %s left the lobby", known_name(m).c_str());
    g_steam->close(m);
    g_link_said.erase(m);
    g_going.erase(m);
    std::lock_guard<std::mutex> hold(g_lock);
    g_left.push_back(ip_of_user(m));
  }
  g_allowed.swap(now);
}

// --- the host's side ---
bool g_creating = false;
uint64_t g_creating_at = 0;
std::string g_level_said;

void host_tick(uint64_t now) {
  if (!g_lobby) {
    if (!g_creating || now - g_creating_at > 20000) {
      g_steam->create_lobby(32);  // the engine's own limit of players
      g_creating = true;
      g_creating_at = now;
    }
    return;
  }
  refresh_members();
  std::string level;
  {
    std::lock_guard<std::mutex> hold(g_lock);
    level = g_level;
  }
  if (level != g_level_said) {
    g_level_said = level;
    g_steam->set_lobby_data(g_lobby, "level", level);
  }
}

void host_lobby_created(const steam::Event& e, uint64_t now) {
  g_creating = false;
  if (!e.ok) {
    LOGW("steam: the lobby was not created (%s); the next try in 20 s", e.text.c_str());
    g_creating = true;  // keeps the 20 s
    g_creating_at = now;
    return;
  }
  if (g_lobby) return;  // Steam tells it twice
  g_lobby = e.id;
  g_steam->set_lobby_data(g_lobby, "metronet", METRONET_VERSION);
  g_steam->set_lobby_data(g_lobby, "link", std::to_string(link::kProtocol));
  g_steam->set_lobby_data(g_lobby, "host", std::to_string(g_me));
  char connect[64];
  snprintf(connect, sizeof(connect), "+connect_lobby %llu", static_cast<unsigned long long>(g_lobby));
  g_steam->set_presence("connect", connect);
  g_steam->set_presence("status", "Metro 2033 Redux: co-op (metronet)");
  LOGI("steam: this game is open to your Steam friends (lobby %llu); a friend's game with mode = join and address = steam finds it by itself",
       static_cast<unsigned long long>(g_lobby));
  hud::notice("Steam: игра открыта для друзей", "Steam: the game is open to your friends");
}

// --- a joining game's side ---
enum class Find { Searching, Entering, Ready } g_find = Find::Searching;
uint64_t g_find_at = 0;
uint64_t g_entering = 0;
uint64_t g_host = 0;
uint64_t g_asked = 0;                       // a lobby the user chose in Steam itself (an invitation)
std::map<uint64_t, uint64_t> g_skip_until;  // lobbies that are not this game's, not looked at again for a while
uint64_t g_search_said_at = 0;

std::string lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
  return s;
}

// [net] address = steam:<what>: "" = any friend's game.
std::string wanted() {
  size_t colon = g_cfg.net_address.find(':');
  return colon == std::string::npos ? "" : g_cfg.net_address.substr(colon + 1);
}

uint64_t wanted_lobby() {
  std::string w = wanted();
  if (w.size() < 17 || w.find_first_not_of("0123456789") != std::string::npos) return 0;
  return strtoull(w.c_str(), nullptr, 10);
}

void enter(uint64_t lobby, uint64_t now) {
  g_entering = lobby;
  g_find = Find::Entering;
  g_find_at = now;
  g_steam->join_lobby(lobby);
}

void leave_and_search(uint64_t lobby, uint64_t skip_ms, uint64_t now) {
  if (lobby) {
    g_steam->leave_lobby(lobby);
    if (skip_ms) g_skip_until[lobby] = now + skip_ms;
  }
  if (g_lobby == lobby) g_lobby = 0;
  g_allowed.clear();
  g_find = Find::Searching;
  g_find_at = now;
}

void join_tick(uint64_t now) {
  switch (g_find) {
    case Find::Searching: {
      if (g_auto && !g_asked) {  // the game the player chose is not there (any more): back to looking around
        g_leave.store(true);
        break;
      }
      if (now - g_find_at < 2000) break;
      g_find_at = now;
      if (g_asked) {
        uint64_t lobby = g_asked;
        g_asked = 0;
        enter(lobby, now);
        break;
      }
      if (uint64_t lobby = wanted_lobby()) {
        if (g_skip_until[lobby] <= now) enter(lobby, now);
        break;
      }
      std::string part = lower(wanted());
      std::vector<steam::FriendGame> games = g_steam->friends_in_lobbies();
      size_t named = 0;
      for (const steam::FriendGame& g : games) {
        if (!part.empty() && lower(g.name).find(part) == std::string::npos) continue;
        ++named;
        if (g_skip_until[g.lobby] > now) continue;
        LOGI("steam: %s sits in a lobby, looking whether it is a metronet game", g.name.empty() ? "a friend" : g.name.c_str());
        enter(g.lobby, now);
        return;
      }
      if (now - g_search_said_at > 30000) {
        g_search_said_at = now;
        hud::notice("Steam: ищу игру друга (друг должен запустить игру хостом)", "Steam: looking for a friend's game...", 10);
        LOGI("steam: looking for a friend's game%s%s - none yet (%zu of your friends sit in a Spacewar lobby now)",
             part.empty() ? "" : " with the name ", part.c_str(), named);
      }
      break;
    }
    case Find::Entering:
      if (now - g_find_at > 15000) {
        LOGW("steam: no answer about the lobby in 15 s, looking again");
        leave_and_search(g_entering, 10000, now);
      }
      break;
    case Find::Ready:
      if (now - g_find_at < 2000) break;
      g_find_at = now;
      if (g_steam->lobby_owner(g_lobby) != g_host) {
        LOGW("steam: %s is not in the lobby any more (its game was closed?)%s", known_name(g_host).c_str(),
             g_auto ? "" : ", looking for the game again");
        leave_and_search(g_lobby, 0, now);
      } else {
        refresh_members();
      }
      break;
  }
}

void join_lobby_entered(const steam::Event& e, uint64_t now) {
  if (g_find != Find::Entering || e.id != g_entering) return;
  if (!e.ok) {
    LOGW("steam: the lobby did not let this game in (%s)", e.text.c_str());
    leave_and_search(e.id, 15000, now);
    return;
  }
  std::string version = g_steam->lobby_data(e.id, "metronet");
  if (version.empty()) {
    LOGI("steam: that lobby is not a metronet game (a real Spacewar or another mod), left");
    leave_and_search(e.id, 60000, now);
    return;
  }
  std::string protocol = g_steam->lobby_data(e.id, "link");
  if (protocol != std::to_string(link::kProtocol)) {
    LOGE("steam: the host plays with metronet %s (protocol %s), this game has %s (protocol %u): both need the same version of the mod",
         version.c_str(), protocol.c_str(), METRONET_VERSION, link::kProtocol);
    hud::notice("У хоста другая версия мода (" + version + "), у вас " METRONET_VERSION ", нужна одинаковая",
                "The host has another version of the mod (" + version + "), yours is " METRONET_VERSION, 15);
    leave_and_search(e.id, 30000, now);
    return;
  }
  uint64_t host = g_steam->lobby_owner(e.id);
  if (!host || host == g_me) {
    LOGW("steam: that lobby has no host any more, left");
    leave_and_search(e.id, 15000, now);
    return;
  }
  g_lobby = e.id;
  g_host = host;
  g_allowed.clear();
  refresh_members();
  g_allowed.insert(host);
  remember_name(host);
  g_steam->accept(host);
  std::string address, level = g_steam->lobby_data(e.id, "level");
  {
    std::lock_guard<std::mutex> hold(g_lock);
    address = text_of_ip(ip_of_user(host));
    g_host_address = address;
  }
  g_find = Find::Ready;
  g_find_at = now;
  LOGI("steam: found the game of %s (metronet %s%s%s), the game connects to it as %s", known_name(host).c_str(), version.c_str(),
       level.empty() ? "" : ", level ", level.c_str(), address.c_str());
  hud::notice("Steam: найдена игра: " + known_name(host) + ", подключение", "Steam: found the game of " + known_name(host) + ", connecting");
}

// --- mode = auto ---
uint64_t g_scan_at = 0;
std::map<uint64_t, uint64_t> g_data_asked_at;  // lobby -> when its data was asked for

void drop_offer(uint64_t lobby) {
  std::lock_guard<std::mutex> hold(g_lock);
  if (g_have_offer && (!lobby || g_offer.lobby == lobby)) g_have_offer = false;
}

// Out of whatever lobby this game keeps or sits in.
void close_lobby() {
  if (g_lobby) g_steam->leave_lobby(g_lobby);
  g_lobby = 0;
  g_creating = false;
  g_level_said.clear();
  for (uint64_t user : g_allowed) g_steam->close(user);
  g_allowed.clear();
  g_link_said.clear();
  g_steam->set_presence(nullptr, nullptr);
}

// In the menu: which friends play a game of this mod right now?
void find_tick(uint64_t now) {
  if (now - g_scan_at < 3000) return;
  g_scan_at = now;
  uint64_t offered;
  {
    std::lock_guard<std::mutex> hold(g_lock);
    offered = g_have_offer ? g_offer.lobby : 0;
  }
  bool offered_lives = false;
  for (const steam::FriendGame& g : g_steam->friends_in_lobbies()) {
    {  // a game already known stays on the menu's network page while its friend sits in that lobby
      std::lock_guard<std::mutex> hold(g_lock);
      auto known = g_games.find(g.lobby);
      if (known != g_games.end()) known->second.at = now;
    }
    if (g.lobby == offered) {
      offered_lives = true;
      continue;
    }
    if (!g.name.empty()) {
      std::lock_guard<std::mutex> hold(g_lock);
      g_names[g.who] = g.name;
    }
    if (g_skip_until[g.lobby] > now) continue;
    uint64_t& asked = g_data_asked_at[g.lobby];
    if (asked && now - asked < 10000) continue;
    asked = now;
    g_steam->request_lobby_data(g.lobby);
  }
  if (offered && !offered_lives) {
    LOGI("steam: the friend's game is not there any more");
    drop_offer(offered);
  }
}

void find_lobby_data(const steam::Event& e, uint64_t now) {
  g_data_asked_at.erase(e.id);
  if (!e.ok) {
    g_skip_until[e.id] = now + 30000;
    return;
  }
  std::string version = g_steam->lobby_data(e.id, "metronet");
  if (version.empty()) {  // a real Spacewar, another mod
    g_skip_until[e.id] = now + 120000;
    return;
  }
  uint64_t host = strtoull(g_steam->lobby_data(e.id, "host").c_str(), nullptr, 10);
  if (!host || host == g_me) {
    g_skip_until[e.id] = now + 30000;
    return;
  }
  remember_name(host);
  if (g_steam->lobby_data(e.id, "link") != std::to_string(link::kProtocol)) {
    g_skip_until[e.id] = now + 120000;
    LOGE("steam: %s plays with metronet %s, this game has %s: both need the same version of the mod", known_name(host).c_str(),
         version.c_str(), METRONET_VERSION);
    hud::notice(known_name(host) + ": другая версия мода (" + version + "), у вас " METRONET_VERSION ", нужна одинаковая",
                known_name(host) + " has another version of the mod (" + version + "), yours is " METRONET_VERSION, 15);
    return;
  }
  std::lock_guard<std::mutex> hold(g_lock);
  {
    Seen& s = g_games[e.id];
    s.game.lobby = e.id;
    s.game.level = g_steam->lobby_data(e.id, "level");
    auto nm = g_names.find(host);
    s.game.name = nm != g_names.end() ? nm->second : "";
    s.at = now;
  }
  if (g_have_offer) return;
  g_offer.lobby = e.id;
  g_offer.level = g_steam->lobby_data(e.id, "level");
  auto it = g_names.find(host);
  g_offer.name = it != g_names.end() ? it->second : "";
  g_have_offer = true;
  LOGI("steam: %s plays now (%s): this game can join", g_offer.name.c_str(), g_offer.level.c_str());
}

// What the game's side asked for since the last look.
void auto_switch(uint64_t now) {
  bool play = g_play_host.load();
  uint64_t yes = g_offer_yes.exchange(0), no = g_offer_no.exchange(0);
  if (no) {
    g_skip_until[no] = now + 600000;
    drop_offer(no);
  }
  if (g_leave.exchange(false) && g_role == Role::Join) {
    close_lobby();
    g_find = Find::Searching;
    g_asked = 0;
    g_host = 0;
    {
      std::lock_guard<std::mutex> hold(g_lock);
      g_host_address.clear();
    }
    g_joining.store(false);
    g_role = Role::Find;
    LOGI("steam: out of the friend's game");
  }
  if (play && g_role == Role::Find) {
    drop_offer(0);
    g_knocked.store(false);
    g_role = Role::Host;
  } else if (!play && g_role == Role::Host) {
    close_lobby();
    g_knocked.store(false);
    g_role = Role::Find;
    LOGI("steam: this game is not played now, its lobby is closed");
  }
  if (yes && g_role == Role::Find) {
    drop_offer(0);
    {
      std::lock_guard<std::mutex> hold(g_lock);
      g_host_address.clear();
    }
    g_asked = yes;
    g_skip_until.erase(yes);
    g_find = Find::Searching;
    g_find_at = 0;
    g_role = Role::Join;
  }
}

// --- both sides ---
void on_event(const steam::Event& e, uint64_t now) {
  switch (e.kind) {
    case steam::Event::SessionRequest:
      if (!g_allowed.count(e.id)) refresh_members();
      if (g_allowed.count(e.id)) {
        g_steam->accept(e.id);
      } else if (g_strangers.insert(e.id).second) {
        LOGW("steam: a message from %llu, who is not in this game's lobby, is not answered", static_cast<unsigned long long>(e.id));
      }
      break;
    case steam::Event::SessionFailed:
      LOGW("steam: the connection to %s broke: %s", known_name(e.id).c_str(), e.text.c_str());
      g_link_said.erase(e.id);
      break;
    case steam::Event::LobbyCreated:
      if (g_role == Role::Host) host_lobby_created(e, now);
      break;
    case steam::Event::LobbyEntered:
      if (g_role == Role::Join) join_lobby_entered(e, now);
      break;
    case steam::Event::LobbyMembers:
      if (g_role == Role::Host || g_find == Find::Ready) refresh_members();
      break;
    case steam::Event::LobbyData:
      if (g_role == Role::Find) find_lobby_data(e, now);
      break;
    case steam::Event::JoinRequested:
      if (g_auto && g_role == Role::Find) {
        LOGI("steam: you chose a friend's game in Steam, going there");
        g_joining.store(true);
        g_offer_yes.store(e.id);
      } else if (g_auto) {
        LOGI("steam: an invitation to a friend's game came, but this game plays a level now (go to the main menu first)");
        hud::notice("Приглашение в игру друга: сначала выйдите в главное меню", "An invitation to a friend's game: go to the main menu first", 10);
      } else if (g_role == Role::Join) {
        LOGI("steam: you chose a friend's game in Steam, going there");
        if (g_find == Find::Ready && g_lobby != e.id) leave_and_search(g_lobby, 0, now);
        g_asked = e.id;
        g_skip_until.erase(e.id);
        g_find_at = 0;
      } else {
        LOGI("steam: an invitation to a friend's game came, but this game is not set to join (mode = join, address = steam)");
      }
      break;
  }
}

void say_links() {
  for (uint64_t user : g_allowed) {
    std::string state = g_steam->link_state(user);
    std::string kind = state.substr(0, state.find(", ping"));
    if (kind.empty() || kind == "no session") continue;
    auto it = g_link_said.find(user);
    if (it != g_link_said.end() && it->second == kind) continue;
    g_link_said[user] = kind;
    LOGI("steam: the connection to %s: %s", known_name(user).c_str(), state.c_str());
  }
}

DWORD WINAPI run(void*) {
  std::string said;
  for (;;) {
    std::string why;
    bool later = false;
    if (g_cfg.lab_steam > 0)
      g_steam = steam::make_mock(g_cfg, why);
    else
      g_steam = steam::make_real(why, later);
    if (g_steam) break;
    if (why != said) {
      said = why;
      if (later && g_role == Role::Join)
        hud::notice("Steam не запущен: запустите Steam, игра подождёт", "Steam is not running - start Steam, the game waits", 20);
      LOGW("steam: not available: %s%s", why.c_str(),
           !later ? "; the game goes by IP address only"
                  : g_role == Role::Join ? "; start Steam - the game waits for it" : "; start Steam to let friends in through it (by IP address the game works as it is)");
    }
    if (!later) return 0;
    Sleep(5000);
  }
  g_me = g_steam->my_id();
  {
    std::lock_guard<std::mutex> hold(g_lock);
    g_my_name = g_steam->my_name();
    g_names[g_me] = g_my_name;
  }
  LOGI("steam: ready%s, this player is '%s' (%llu)", g_cfg.lab_steam > 0 ? " (the lab's stand-in)" : " under Spacewar (480)", my_name().c_str(),
       static_cast<unsigned long long>(g_me));
  g_up.store(true);
  if (g_role == Role::NameOnly) {
    // Only the name was wanted; Steam's callbacks still have to be taken now and then.
    for (;;) {
      Sleep(250);
      g_steam->pump();
      steam::Event e;
      while (g_steam->next_event(e)) {
      }
    }
  }
  timeBeginPeriod(1);
  uint64_t pumped = 0, ticked = 0, links = 0, dropped = 0;
  std::vector<steam::Message> batch(32);
  for (;;) {
    WaitForSingleObject(g_wake, g_lobby ? 1 : 50);
    uint64_t now = GetTickCount64();
    // the game -> Steam
    std::deque<Packet> out;
    {
      std::lock_guard<std::mutex> hold(g_lock);
      out.swap(g_out);
    }
    for (const Packet& p : out) {
      uint64_t user;
      {
        std::lock_guard<std::mutex> hold(g_lock);
        user = user_of_ip(p.ip);
      }
      if (user) g_steam->send(user, p.data.data(), static_cast<uint32_t>(p.data.size()), false, 0);
    }
    // Steam -> the game
    for (int round = 0; round < 16; ++round) {
      int n = g_steam->receive(0, batch.data(), static_cast<int>(batch.size()));
      if (n <= 0) break;
      std::lock_guard<std::mutex> hold(g_lock);
      for (int i = 0; i < n; ++i) {
        if (!g_allowed.count(batch[i].from)) {
          ++dropped;
          continue;
        }
        if (g_role == Role::Host) g_knocked.store(true, std::memory_order_relaxed);
        g_heard_at[batch[i].from] = now;
        if (batch[i].from == g_host) g_host_heard_at.store(now, std::memory_order_relaxed);
        Packet p;
        p.ip = ip_of_user(batch[i].from);
        p.data = std::move(batch[i].data);
        if (g_in.size() >= 4096) {
          g_in.pop_front();
          --g_in_count;
        }
        g_in.push_back(std::move(p));
        ++g_in_count;
      }
    }
    if (now - pumped >= 15) {
      pumped = now;
      g_steam->pump();
      steam::Event e;
      while (g_steam->next_event(e)) on_event(e, now);
    }
    if (now - ticked >= 1000) {
      ticked = now;
      if (g_auto) auto_switch(now);
      if (g_role == Role::Host)
        host_tick(now);
      else if (g_role == Role::Join)
        join_tick(now);
      else if (g_role == Role::Find)
        find_tick(now);
      for (uint64_t user : g_allowed) {
        bool have;
        {
          std::lock_guard<std::mutex> hold(g_lock);
          have = g_names.count(user) && !g_names[user].empty();
        }
        if (!have) remember_name(user);
      }
    }
    if (now - links >= 5000) {
      links = now;
      say_links();
      if (dropped) {
        LOGD("steam: %llu messages from users outside the lobby were dropped", static_cast<unsigned long long>(dropped));
        dropped = 0;
      }
    }
  }
}

}  // namespace

bool join_by_steam(const Config& cfg) {
  return cfg.net_mode == Config::NetMode::Join && (cfg.net_address == "steam" || cfg.net_address.rfind("steam:", 0) == 0);
}

bool is_ours(uint32_t ip) { return (ip & 0xF8) == 0xF0; }  // the first byte is 240 - 247

bool up() { return g_up.load(); }

bool next_left(uint32_t& ip) {
  std::lock_guard<std::mutex> hold(g_lock);
  if (g_left.empty()) return false;
  ip = g_left.back();
  g_left.pop_back();
  return true;
}

uint64_t host_silent_ms() {
  uint64_t heard = g_host_heard_at.load();
  return heard ? GetTickCount64() - heard : ~0ull;
}

void play_as_host(bool on) { g_play_host.store(on); }
bool knocked() { return g_knocked.load(); }
bool joining() { return g_joining.load(); }

std::vector<Offer> games() {
  std::vector<Offer> out;
  uint64_t now = GetTickCount64();
  std::lock_guard<std::mutex> hold(g_lock);
  for (auto it = g_games.begin(); it != g_games.end();) {
    if (now - it->second.at > 20000) {  // (looked at every 10 s while the menu stands: gone when not seen twice)
      it = g_games.erase(it);
      continue;
    }
    out.push_back(it->second.game);
    ++it;
  }
  return out;
}

bool offer(Offer& out) {
  std::lock_guard<std::mutex> hold(g_lock);
  if (!g_have_offer) return false;
  out = g_offer;
  return true;
}

void answer_offer(uint64_t lobby, bool yes) {
  if (yes) {
    g_joining.store(true);
    g_offer_yes.store(lobby);
  } else {
    g_offer_no.store(lobby);
  }
}

void leave_session() {
  g_leave.store(true);
  g_joining.store(false);
}

std::string my_name() {
  std::lock_guard<std::mutex> hold(g_lock);
  return g_my_name;
}

std::string host_address() {
  std::lock_guard<std::mutex> hold(g_lock);
  return g_host_address;
}

std::string name_at(uint32_t ip) {
  std::lock_guard<std::mutex> hold(g_lock);
  uint64_t user = user_of_ip(ip);
  auto it = g_names.find(user);
  return user && it != g_names.end() ? it->second : "";
}

void set_level(const std::string& level) {
  std::lock_guard<std::mutex> hold(g_lock);
  g_level = level;
}

void install(const Config& cfg) {
  g_cfg = cfg;
  if (cfg.net_mode == Config::NetMode::Off) return;
  bool by_steam = join_by_steam(cfg);
  if (!cfg.net_steam && cfg.lab_steam <= 0) {
    if (by_steam) LOGE("steam: address = steam, but steam = 0 in metronet.ini: there is nobody to join");
    return;
  }
  g_auto = cfg.net_mode == Config::NetMode::Auto;
  g_role = g_auto ? Role::Find : cfg.net_mode == Config::NetMode::Host ? Role::Host : by_steam ? Role::Join : Role::NameOnly;
  if (g_role != Role::NameOnly) {
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    patch::import("WS2_32.dll", "WSASendTo", reinterpret_cast<void*>(&on_send_to), reinterpret_cast<void**>(&g_send_to));
    patch::import("WS2_32.dll", "WSARecvFrom", reinterpret_cast<void*>(&on_recv_from), reinterpret_cast<void**>(&g_recv_from));
    if (!g_send_to || !g_recv_from) {
      LOGE("steam: the game's socket calls were not found, Steam play is off");
      return;
    }
  }
  // The thread starts to run when the loader is done with this module.
  HANDLE thread = CreateThread(nullptr, 0, &run, nullptr, 0, nullptr);
  if (thread) CloseHandle(thread);
}

}  // namespace steamnet
