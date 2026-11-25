#include "mod/session.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "common/log.h"
#include "mod/engine.h"
#include "mod/game.h"
#include "mod/hud.h"
#include "mod/link.h"
#include "mod/menu.h"
#include "mod/net.h"
#include "mod/patch.h"
#include "mod/players.h"
#include "mod/revive.h"
#include "mod/steamnet.h"

namespace session {
namespace {

Config g_cfg;
const char kMenuLevel[] = "2033\\000";

// --- what the engine's log tells (written on the logging thread, read by the tick) ----------------------
std::mutex g_lock;
std::string g_level;                     // the level being loaded / loaded: "2033\l01_hunter"
std::atomic<uint32_t> g_running_seq{0};  // "loader_finish: mapstate_running" seen so many times
std::atomic<uint64_t> g_running_at{0};
std::atomic<bool> g_loading{false};      // a level is being loaded (its "running" line has not come yet)
std::atomic<uint32_t> g_waiting_seq{0};  // "mapstate_loaded_and_waiting"
std::atomic<uint32_t> g_save_bin_seq{0}; // "load save bin": the quick save's objects are in
std::atomic<uint32_t> g_incoming_seq{0}; // "[net] incoming connection"
std::atomic<uint32_t> g_change_seq{0};   // "change_map_cl": the host went on to the next level, this game follows
std::atomic<bool> g_quitting{false};     // the game is closing ("quit"): nothing is started any more

void on_log(const char* line) {
  if (const char* p = strstr(line, "loading content\\maps\\")) {
    p += 21;
    const char* end = strstr(p, "\\level");
    std::lock_guard<std::mutex> hold(g_lock);
    g_level.assign(p, end ? static_cast<size_t>(end - p) : strlen(p));
    g_loading.store(true);
  } else if (strstr(line, "loader_finish: mapstate_running")) {
    g_loading.store(false);
    g_running_at.store(GetTickCount64());
    ++g_running_seq;
  } else if (strstr(line, "mapstate_loaded_and_waiting")) {
    ++g_waiting_seq;
  } else if (strstr(line, "load save bin")) {
    ++g_save_bin_seq;
  } else if (strstr(line, "[net] incoming connection")) {
    ++g_incoming_seq;
  } else if (strstr(line, "change_map_cl")) {
    ++g_change_seq;
  } else if (!strncmp(line, "^quit", 5)) {
    g_quitting.store(true);
  }
}

std::string level() {
  std::lock_guard<std::mutex> hold(g_lock);
  return g_level;
}

bool level_is_running() { return game::ref<void*>(game::kLevel) && game::ref<int>(game::kMapState) != 2; }

// --- the host ---------------------------------------------------------------------------------------------
uint32_t g_hosted_seq = 0;        // the mapstate_running this game hosted last
uint64_t g_waiting_since[32] = {};  // per slot: when a peer without a body was first seen
uint8_t g_slot_state[32] = {};      // per slot: the state seen last (logged when it changes)
uint64_t g_removed_at[32] = {};     // per slot: when its body was taken out (to say whether it is gone)
uint64_t g_dead_since[32] = {};     // per slot: when its player's body was first seen dead
uint64_t g_old_body_at[32] = {};    // per slot: when the body its player died in is to go
const uint64_t kRespawnMs = 4000;   // a joined player who died gets a new body after this long
// A joined player's health arrives from its game. When that player dies it is never seen at 0 here: within
// two ticks it falls to about 0.03 and then grows back by itself, 0.01 a second at first (measured in the lab:
// 1.000 -> 0.493 -> 0.032 -> 0.082 five seconds later). A living player this low is taken for dead too.
const float kDeadHealth = 0.05f;

void host_tick(uint64_t now) {
  uint32_t seq = g_running_seq.load();
  if (seq != g_hosted_seq && !g_loading.load() && now - g_running_at.load() > 2000) {
    g_hosted_seq = seq;
    std::string name = level();
    if (name != kMenuLevel && game::ref<void*>(game::kLevel)) {
      LOGI("session: hosting the level %s", name.c_str());
      steamnet::set_level(name);
      net::pop_loader_pauses();
      memset(g_waiting_since, 0, sizeof(g_waiting_since));
      memset(g_dead_since, 0, sizeof(g_dead_since));
      memset(g_old_body_at, 0, sizeof(g_old_body_at));
      if (!net::host(name.c_str())) LOGE("session: this level could not be hosted");
    }
  }
  // A player who joined gets a body once its game is ready for one: the slot's state is 3 while the joiner
  // loads the level this game sent it, and 4 from the moment it runs. (1 = it follows a level change.)
  if (game::ref<uintptr_t>(game::kNetHost) <= 1 || game::ref<int>(game::kNetLocalPlayer) != 0) return;
  net::Players& p = game::ref<net::Players>(game::kNetPlayers);
  for (unsigned i = 1; i < p.size && i < 32; ++i) {
    const uint8_t* slot = p.array + game::kNetPlayerSize * i;
    bool peer = *reinterpret_cast<void* const*>(slot + 0x30) != nullptr;
    bool body = *reinterpret_cast<const uint16_t*>(slot + 0x64) != 0xFFFF;
    uint8_t state = static_cast<uint8_t>((*reinterpret_cast<const uint32_t*>(slot + 0x68) >> 16) & 7);
    if (state != g_slot_state[i]) {
      static const char* const kWhat[8] = {"free", "changes the level", "is told the level", "loads the level", "is ready, has no body",
                                           "plays", "left", "7"};
      LOGI("session: player %u %s (state %u -> %u)", i, kWhat[state], g_slot_state[i], state);
      g_slot_state[i] = state;
      if (state == 6 && net::remove_body(static_cast<int>(i))) g_removed_at[i] = now;  // the player left
    }
    if (g_removed_at[i] && now - g_removed_at[i] > 3000) {
      g_removed_at[i] = 0;
      int left = net::removed_body_left(static_cast<int>(i));
      if (left == 0)
        LOGI("session: the body of player %u is gone", i);
      else
        LOGW("session: %d entities of the body of player %u are still in the world", left, i);
    }
    // A joined player's body that died (its health comes from that player's game; here it grows back by
    // itself, so the first sight of "dead" is kept): a new body, then the old one goes.
    if (peer && body && state == 5) {
      float health = net::health_of(*reinterpret_cast<const uint16_t*>(slot + 0x64));
      static float seen[32] = {};
      if (health > seen[i] + 0.05f || health < seen[i] - 0.05f) {
        LOGD("session: player %u health here %.3f -> %.3f", i, seen[i], health);
        seen[i] = health;
      }
      if (revive::downed(static_cast<int>(i))) {
        g_dead_since[i] = 0;  // down, not dead: a teammate may raise them (revive.cpp)
      } else if (!g_dead_since[i] && health > -999.f && health <= kDeadHealth) {
        g_dead_since[i] = now;
        LOGI("session: player %u died (health %.3f here), a new body in %llu s", i, health,
             static_cast<unsigned long long>(kRespawnMs / 1000));
      } else if (g_dead_since[i] && now - g_dead_since[i] > kRespawnMs) {
        g_dead_since[i] = 0;
        if (net::respawn_body(static_cast<int>(i))) g_old_body_at[i] = now + 1500;
      }
    } else {
      g_dead_since[i] = 0;
    }
    if (g_old_body_at[i] && now >= g_old_body_at[i]) {
      g_old_body_at[i] = 0;
      net::remove_old_body(static_cast<int>(i));
      g_removed_at[i] = now;
    }
    if (!peer || body || state != 4) {
      g_waiting_since[i] = 0;
    } else if (!g_waiting_since[i]) {
      g_waiting_since[i] = now;
    } else if (now - g_waiting_since[i] > static_cast<uint64_t>(g_cfg.net_spawn_delay) * 1000) {
      g_waiting_since[i] = now + 30000;  // the next try, if this one gives no body, in a while
      net::spawn_body(static_cast<int>(i));
    }
  }
}

// --- the host is gone -------------------------------------------------------------------------------------------
// A joined game whose host went away: the engine's own answer (exe+6952E0, from its network events) closes EVERY
// page of the game's interface and then opens its old multiplayer's page "mp_game_disconnect", which the game does
// not have. With every page closed, the main menu the game goes back to stays a blurred background with nothing to
// press (the friend's frozen screen, 2026-11-25). The mod takes that over: the engine's answer is not run, and the
// session goes back to the main menu itself.
constexpr uintptr_t kHostGone = 0x6952E0;
using HostGoneFn = void (*)(void*);
HostGoneFn g_host_gone = nullptr;
std::atomic<uint32_t> g_lost_seq{0};     // the engine has given the host up

void on_host_gone(void* self) {
  LOGI("session: the engine says the host is gone (its own page for it is left out)");
  ++g_lost_seq;
  (void)self;
}

// --- the joiner ---------------------------------------------------------------------------------------------
enum class Join { Menu, Connecting, Loading, Finishing, Unpausing, InSession, Following, Failed };
Join g_join = Join::Menu;
uint64_t g_join_at = 0;          // when the current step began
uint32_t g_join_waiting = 0, g_join_save_bin = 0, g_join_menu_seq = 0;
uint32_t g_join_change = 0;
uint64_t g_join_waits_since = 0;  // Loading: since when the loaded level has been waiting for its "go on"
uint64_t g_join_knock_at = 0;     // Connecting: when "connect" was said last
int g_join_knocks = 0;            // ... and how many times
const uint64_t kKnockEveryMs = 10000;
bool g_by_steam = false;          // the host is a Steam friend: its address is the one steamnet made up for it
std::string g_join_address;       // what "connect" is said with

void join_tick(uint64_t now) {
  switch (g_join) {
    case Join::Menu:
      // The menu is a level too; a connect before it runs leaves the game paused by its loader.
      if (g_running_seq.load() != g_join_menu_seq && level() == kMenuLevel && now - g_running_at.load() > 1500 &&
          level_is_running()) {
        if (g_by_steam) {
          // Steam has to find the friend's game first (the log of steamnet says how that goes).
          g_join_address = steamnet::host_address();
          if (g_join_address.empty()) break;
        }
        g_join_menu_seq = g_running_seq.load();
        g_join_waiting = g_waiting_seq.load();
        g_join_save_bin = g_save_bin_seq.load();
        LOGI("session: joining %s", g_join_address.c_str());
        engine::command("connect " + g_join_address, true);
        g_join = Join::Connecting;
        g_join_at = now;
        g_join_knock_at = now;
        g_join_knocks = 1;
      }
      break;
    case Join::Connecting:
      if (g_waiting_seq.load() != g_join_waiting) {  // the host's level is loaded here
        net::set_network_game(true);
        g_join = Join::Loading;
        g_join_at = now;
        g_join_waits_since = 0;
      } else if (!g_loading.load() && now - g_join_knock_at > kKnockEveryMs) {
        // Nobody answered: the host is not there yet - its game takes connections only once its level runs, and
        // a big level loads for half a minute. The game's own "connect" knocks once and then stays silent for
        // good, so it is said again (the command drops the connection that failed by itself) until the host
        // answers and its level starts loading here.
        ++g_join_knocks;
        if (g_join_knocks == 2 || g_join_knocks % 6 == 0)
          LOGI("session: no answer from %s yet (is the host in a level?), knocking again - try %d", g_join_address.c_str(),
               g_join_knocks);
        engine::command("connect " + g_join_address, true);
        g_join_knock_at = now;
      }
      break;
    case Join::Loading:
      // The quick save's objects come after the map; the game then waits for a key again. It must be left
      // waiting for a moment: in the first frames of that wait the game tells the host "the level is loaded
      // here" (the slot goes from 3 to 4). Once the "go on" came 3 ms after the wait began - the host never
      // heard it, gave no body, and the joined game hung in an empty level for good.
      if (g_save_bin_seq.load() != g_join_save_bin && game::ref<int>(game::kMapState) == 2) {
        if (!g_join_waits_since) g_join_waits_since = now;
        if (now - g_join_waits_since >= 400 && now - g_join_at > 500) {
          net::finish_load();
          g_join = Join::Finishing;
          g_join_at = now;
          g_join_waits_since = 0;
        }
      } else if (now - g_join_at > 120000) {
        LOGE("session: the host's level did not load in 120 s");
        g_join = Join::Failed;
      }
      break;
    case Join::Finishing:
      if (now - g_join_at > 500) {
        net::pop_loader_pauses();
        net::log_state("joined");
        g_join_change = g_change_seq.load();
        g_join = Join::InSession;
        g_join_at = now;
      }
      break;
    case Join::InSession:
      // The host went on to the next level: the engine tells this game to load it too ("change_map_cl"), as a
      // level of its own, with the "network game" flag off.
      if (g_change_seq.load() != g_join_change) {
        g_join_change = g_change_seq.load();
        LOGI("session: following the host to the next level");
        g_join = Join::Following;
        g_join_at = now;
      }
      break;
    case Join::Following:
      // "change_map_cl" may come twice in a row; the level is loaded when the last of them is through. Then the
      // flag comes back, and with it the quick save the host has sent meanwhile is loaded, as in a join.
      if (g_change_seq.load() != g_join_change) {
        g_join_change = g_change_seq.load();
        g_join_at = now;
      } else if (!game::ref<void*>(game::kLevel) || level() == kMenuLevel) {
        if (now - g_join_at > 180000) {
          LOGE("session: the next level did not load in 180 s");
          g_join = Join::Failed;
        }
      } else if (now - g_join_at > 1000 && now - g_running_at.load() > 1000) {
        if (game::ref<int>(game::kMapState) == 2) {
          net::finish_load();  // "press any key": a joined game goes on by itself
          g_join_at = now;
        } else {
          g_join_waiting = g_waiting_seq.load();
          g_join_save_bin = g_save_bin_seq.load();
          net::set_network_game(true);
          g_join = Join::Loading;
          g_join_at = now;
          g_join_waits_since = 0;
        }
      }
      break;
    case Join::Unpausing:
    case Join::Failed:
      break;
  }
}

// --- mode = auto: nothing is set by hand ---------------------------------------------------------------------
// Idle     the game as it is. In a story level: Steam friends see this game; when the first of them knocks, the
//          level becomes a network game (Hosting). In the main menu: when a friend plays, the game's own window
//          asks whether to join (Joining).
// Hosting  as mode = host, until the player is back in the main menu.
// Joining  as mode = join with a Steam friend, until the player is back in the main menu.
enum class Auto { Idle, Hosting, Joining } g_auto = Auto::Idle;
uint64_t g_menu_since = 0;   // since when the main menu has been running
uint64_t g_asked_lobby = 0;  // the friend's game the window on the screen asks about
uint64_t g_yes_lobby = 0;    // ... and the one the player said yes to
bool g_ask_open = false;
uint32_t g_lost_seen = 0;    // g_lost_seq when the session began
bool g_going_home = false;   // the host is gone, this game was sent to its main menu

bool in_menu() { return !g_loading.load() && level() == kMenuLevel && level_is_running(); }
bool in_story() { return !g_loading.load() && game::ref<void*>(game::kLevel) && level() != kMenuLevel && level_is_running(); }

void on_yes() { g_yes_lobby = g_asked_lobby; }

// --- back in the main menu after a session -------------------------------------------------------------------------
// When a session ends - the player left it, or the host's game went away - the game loads its main menu, and the
// menu waits for "press any key" like at the game's start. A friend whose host had quit was stuck right there
// (2026-11-25, the first real game: the line "Хост завершил игру", a still picture, no key did anything). So after
// a session the menu is let in by the mod itself, and a pause of the loader left on the engine's stack (the same
// one that stops a joined level's clock) is taken off once it runs.
bool g_back_to_menu = false;
uint64_t g_menu_waits_since = 0;
std::string g_last_level;

void back_to_menu_tick(uint64_t now, bool in_session) {
  std::string lvl = level();
  if (in_session && lvl == kMenuLevel && g_last_level != kMenuLevel && !g_last_level.empty() && !g_back_to_menu) {
    LOGI("session: on the way back to the main menu (from %s)", g_last_level.c_str());
    g_back_to_menu = true;
    g_menu_waits_since = 0;
  }
  g_last_level = lvl;
  if (!g_back_to_menu) return;
  // (the engine's "loading" lasts until the map runs: a map that waits for its key is still "loading" here)
  if (lvl != kMenuLevel || !game::ref<void*>(game::kLevel)) {
    g_menu_waits_since = 0;
    return;
  }
  if (game::ref<int>(game::kMapState) == 2) {  // loaded, waiting for a key
    if (!g_menu_waits_since) g_menu_waits_since = now;
    if (now - g_menu_waits_since >= 600) {
      net::log_pauses("the main menu is loaded and waits for a key: the mod lets it in");
      net::finish_load();
      g_menu_waits_since = now + 4000;  // once more in a while, if this one did not take
    }
  } else if (!g_loading.load() && now - g_running_at.load() > 800) {
    net::log_pauses("the main menu runs");
    net::pop_loader_pauses();
    g_back_to_menu = false;
    g_menu_waits_since = 0;
  }
}

void auto_tick(uint64_t now) {
  if (g_quitting.load()) return;
  back_to_menu_tick(now, g_auto != Auto::Idle);
  switch (g_auto) {
    case Auto::Idle: {
      bool story = in_story() && now - g_running_at.load() > 2000;
      steamnet::play_as_host(story);
      if (story) {
        steamnet::set_level(level());
        g_menu_since = 0;
        g_asked_lobby = g_yes_lobby = 0;
        g_ask_open = false;
        if (steamnet::knocked()) {
          LOGI("session: a friend's game is knocking: this level becomes a network game");
          net::backup_save();
          g_auto = Auto::Hosting;
          g_hosted_seq = g_running_seq.load() - 1;  // host_tick hosts the running level at once
        }
      } else if (in_menu()) {
        if (!g_menu_since) g_menu_since = now;
        if (g_yes_lobby) {
          uint64_t lobby = g_yes_lobby;
          g_yes_lobby = g_asked_lobby = 0;
          g_ask_open = false;
          steamnet::answer_offer(lobby, true);
          g_by_steam = true;
          g_join = Join::Menu;
          g_join_menu_seq = 0;
          g_auto = Auto::Joining;
          g_lost_seen = g_lost_seq.load();
          g_going_home = false;
          LOGI("session: joining the friend's game");
        } else if (g_ask_open) {
          if (!menu::page_open()) {  // closed without a yes
            g_ask_open = false;
            LOGI("session: the player does not want to join that game");
            steamnet::answer_offer(g_asked_lobby, false);
          }
        } else if (steamnet::joining()) {  // chosen in Steam itself (an invitation, "Join game")
          g_by_steam = true;
          g_join = Join::Menu;
          g_join_menu_seq = 0;
          g_auto = Auto::Joining;
          g_lost_seen = g_lost_seq.load();
          g_going_home = false;
          LOGI("session: joining the friend's game chosen in Steam");
        } else {
          steamnet::Offer o;
          if (steamnet::offer(o) && o.lobby != g_asked_lobby && now - g_menu_since > 5000 && !menu::page_open()) {
            std::string who = o.name.empty() ? "Ваш друг" : o.name;
            std::string who_en = o.name.empty() ? "A friend of yours" : o.name;
            if (menu::ask(who + " сейчас играет в Metro 2033 Redux. Присоединиться к этой игре?",
                          who_en + " is playing Metro 2033 Redux now. Join this game?", &on_yes)) {
              g_asked_lobby = o.lobby;
              g_ask_open = true;
            }
          }
        }
      } else {
        g_menu_since = 0;
      }
      break;
    }
    case Auto::Hosting:
      steamnet::play_as_host(true);
      host_tick(now);
      if (in_menu() && now - g_running_at.load() > 1500) {
        LOGI("session: back in the main menu, the session is over");
        steamnet::play_as_host(false);
        g_auto = Auto::Idle;
        g_menu_since = 0;
      }
      break;
    case Auto::Joining: {
      join_tick(now);
      bool over = g_join == Join::Failed;
      // The host's game is gone (closed, back in its menu, the connection lost): the engine notices after its
      // silence limit and wants a page of its old multiplayer that the game does not have; Steam tells sooner.
      // The level stands still from then on, so this game goes to its main menu by itself.
      // (Not while the game is closing: a level change begun then falls inside the game's own shutdown,
      //  metro.exe+8CDF25 - seen in the lab, where both copies are closed in the same second.)
      if (g_join == Join::InSession && !g_going_home && !g_quitting.load() &&
          (g_lost_seq.load() != g_lost_seen || (!steamnet::joining() && steamnet::host_silent_ms() > 8000))) {
        g_going_home = true;
        LOGI("session: the host's game is gone, back to the main menu");
        hud::notice("Хост завершил игру", "The host has ended the game", 10);
        engine::on_main([] { engine::to_main_menu(); });
      }
      if ((g_join == Join::Menu || g_join == Join::Connecting) && !steamnet::joining()) over = true;  // that game is gone
      // back in the main menu: the player left the session, or the host went to its menu and this game followed
      if ((g_join == Join::InSession || g_join == Join::Following) && in_menu() && g_running_seq.load() != g_join_menu_seq &&
          now - g_running_at.load() > 2000)
        over = true;
      if (over) {
        LOGI("session: out of the friend's game");
        net::set_network_game(false);
        steamnet::leave_session();
        g_by_steam = false;
        g_join = Join::Menu;
        g_auto = Auto::Idle;
        g_menu_since = 0;
        g_asked_lobby = g_yes_lobby = 0;
        g_ask_open = false;
      }
      break;
    }
  }
}

void tick() {
  static uint64_t last = 0;
  uint64_t now = GetTickCount64();
  if (now - last < 100) return;
  last = now;
  if (g_cfg.net_mode == Config::NetMode::Auto) {
    net::tick();
    auto_tick(now);
  } else if (g_cfg.net_mode == Config::NetMode::Host) {
    net::tick();
    host_tick(now);
  }
  else if (g_cfg.net_mode == Config::NetMode::Join) {
    net::tick();
    join_tick(now);
    if (g_quitting.load()) return;
    // The host's game went away (the engine gives the host up and wants a page of its old multiplayer the game
    // does not have): back to the main menu, from where this game knocks at the host again.
    if ((g_join == Join::InSession || g_join == Join::Following) && g_lost_seq.load() != g_lost_seen && !g_going_home) {
      g_going_home = true;
      g_lost_seen = g_lost_seq.load();
      LOGI("session: the host's game is gone, back to the main menu");
      hud::notice("Хост завершил игру", "The host has ended the game", 10);
      engine::on_main([] { engine::to_main_menu(); });
    }
    if (g_going_home && level() == kMenuLevel && level_is_running() && now - g_running_at.load() > 2000) {
      g_going_home = false;
      net::set_network_game(false);
      g_join = Join::Menu;
      g_join_menu_seq = 0;
    }
    back_to_menu_tick(now, g_join != Join::Menu || g_going_home);
  }
}

}  // namespace

void install(const Config& cfg) {
  g_cfg = cfg;
  if (cfg.net_mode == Config::NetMode::Off) return;
  LOGI("session: mode %s%s%s", cfg.net_mode == Config::NetMode::Auto ? "auto" : cfg.net_mode == Config::NetMode::Host ? "host" : "join",
       cfg.net_mode == Config::NetMode::Join ? ", address " : "", cfg.net_mode == Config::NetMode::Join ? cfg.net_address.c_str() : "");
  g_by_steam = steamnet::join_by_steam(cfg);
  g_join_address = cfg.net_address;
  patch::hook("the host is gone", game::at(kHostGone), reinterpret_cast<void*>(&on_host_gone), reinterpret_cast<void**>(&g_host_gone));
  link::install();
  steamnet::install(cfg);
  players::install(cfg);
  revive::install(cfg);
  engine::on_log(&on_log);
  engine::on_tick(&tick);
  engine::on_tick(&net::frame);
}

}  // namespace session
