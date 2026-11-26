#include "mod/net.h"

#include <windows.h>

#include <shlobj.h>

#include <intrin.h>

#include <atomic>
#include <bitset>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <algorithm>
#include <vector>

#include "common/log.h"
#include "mod/game.h"
#include "mod/engine.h"
#include "mod/drive.h"
#include "mod/hud.h"
#include "mod/players.h"
#include "mod/link.h"
#include "mod/loot.h"
#include "mod/motions.h"
#include "mod/patch.h"
#include "mod/steamnet.h"

namespace net {

uint8_t* entity_by_id(uint16_t id);
uint8_t* parent_of(uint8_t* e);

namespace {

Config g_cfg;

Players& players() { return game::ref<Players>(game::kNetPlayers); }

uint8_t* controlled() {
  uint8_t* level = game::ref<uint8_t*>(game::kLevel);
  return level ? *reinterpret_cast<uint8_t**>(level + game::kLevelControlled) : nullptr;
}

uint16_t entity_id(const uint8_t* entity) {
  return *reinterpret_cast<const uint16_t*>(entity + game::kEntityHandle + game::kHandleId);
}

// --- a slot for a new peer ------------------------------------------------------------------------------
// When a peer connects and no slot is free, the server takes index == size without growing the players
// vector and writes into the entry past its end. (MetroDeveloper's multiplayer_fix: grow it first.)
using SendPropsPackFn = uint64_t (*)(unsigned, void*);
SendPropsPackFn g_send_props_pack = nullptr;

uint64_t on_send_props_pack(unsigned player, void* props) {
  if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == game::at(game::kSendPropsPackNewPeer)) {
    Players& p = players();
    if (player == p.size) {
      using Reserve = int (*)(Players*, unsigned);
      using Ctor = void (*)(uint8_t*);
      if (p.capacity >= p.size + 1u || reinterpret_cast<Reserve>(game::at(game::kNetPlayersReserve))(&p, p.size + 1u)) {
        reinterpret_cast<Ctor>(game::at(game::kNetPlayerCtor))(p.array + game::kNetPlayerSize * p.size);
        ++p.size;
        LOGI("net: a slot for the new peer: players %u", p.size);
      } else {
        LOGE("net: no memory for a new player slot");
      }
    }
  }
  return g_send_props_pack(player, props);
}

// --- a slot without a body ---------------------------------------------------------------------------------
// "Which player is this entity" may answer with a slot that has no actor (a client that has just joined has no
// body yet); the aim code then reads through the empty handle (crash at exe+3B091F). While this PC is in a real
// network game such a slot is answered as "no player": the callers handle that. In plain single player nothing
// changes (the game relies on slot 0 coming back there).
using PlayerOfEntityFn = uint8_t* (*)(void*, void***);
PlayerOfEntityFn g_player_of_entity = nullptr;

uint8_t* on_player_of_entity(void* core, void*** handle) {
  uint8_t* slot = g_player_of_entity(core, handle);
  if (!slot || game::ref<uintptr_t>(game::kNetHost) <= 1) return slot;
  using ActorOf = void*** (*)(uint8_t*, int);
  void*** h = reinterpret_cast<ActorOf>(game::at(game::kActorOf))(slot, 1);
  if (h && *h && **h) return slot;
  static int logged = 0;
  if (logged++ < 5)
    LOGW("net: slot %d (actor id %X) has no body yet: answered as no player",
         static_cast<int>((slot - players().array) / game::kNetPlayerSize), *reinterpret_cast<uint16_t*>(slot + 0x64));
  return nullptr;
}

// --- no player on this PC yet --------------------------------------------------------------------------------
// Single-player code takes "the player" (level + 0x38) for granted. A joined game has none from the moment the
// host's level is loaded until its body arrives (and the host has none for a moment after net start). Each
// such piece of code found by a crash is skipped while there is no player; the list grows level by level.
using EntityFn = void (*)(uint8_t*);
EntityFn g_watcher_update = nullptr;

void on_watcher_update(uint8_t* entity) {
  if (controlled()) g_watcher_update(entity);
}

using PlayerScaledFn = int64_t (*)(void*, unsigned, void*, int, char);
PlayerScaledFn g_player_scaled = nullptr;

int64_t on_player_scaled(void* object, unsigned id, void* x, int by_player, char flag) {
  if (by_player) {
    uint8_t* player = controlled();
    using Weapon = void* (*)(uint8_t*);
    if (!player || !(*reinterpret_cast<Weapon**>(player))[game::kEntityWeapon / 8](player)) by_player = 0;
  }
  return g_player_scaled(object, id, x, by_player, flag);
}

// The "picked up: ..." line on the screen (exe+542EE0: notifications*, item*, count) asks the player of this PC for
// its difficulty table. A joined game that is still loading the host's level has characters taking things (the
// level's state is being put together) and no player yet: l22_black fell here, exe+542F28.
using HudPickupFn = void (*)(void*, void*, int);
HudPickupFn g_hud_pickup = nullptr;

void on_hud_pickup(void* notifications, void* item, int count) {
  if (controlled()) g_hud_pickup(notifications, item, count);
}

// A model's motion by its number (exe+A1E80: x, request*, y): request + 0x96 is the number, the model's motion set
// (*(y + 0x18) + 0x9B8) holds 16-byte entries at + 0x28, their count beside it - and the number is not checked
// against the count. In single player a model is only ever asked for its own motions. In a session a number can
// come that the model on this PC does not have (l36_ostankino: a joined game fell at exe+9A1C6 on an entry read
// from beyond the table). A number past the set whose entry is not a readable motion is answered "no such
// motion", which the caller takes: nothing is played.
using MotionLookupFn = uint8_t* (*)(void*, uint8_t*, uint8_t*);
MotionLookupFn g_motion_lookup = nullptr;

uint8_t* on_motion_lookup(void* x, uint8_t* request, uint8_t* y) {
  uint16_t number = request ? *reinterpret_cast<uint16_t*>(request + 0x96) : 0xFFFF;
  if (number != 0xFFFF && y && game::ref<uintptr_t>(game::kNetHost) > 1) {
    uint8_t* model = *reinterpret_cast<uint8_t**>(y + 0x18);
    uint8_t* set = model ? *reinterpret_cast<uint8_t**>(model + 0x9B8) : nullptr;
    if (set) {
      uint16_t count = *reinterpret_cast<uint16_t*>(set + 0x32);
      if (number >= count) {
        uint8_t** entries = *reinterpret_cast<uint8_t***>(set + 0x28);
        uint8_t* entry = entries && !IsBadReadPtr(entries + number * 2, 8) ? entries[number * 2] : nullptr;
        bool bad = !entries || IsBadReadPtr(entries + number * 2, 8) || (entry && IsBadReadPtr(entry, 0x80));
        static int told = 0;
        if (told++ < 12)
          LOGW("net: a model is asked for motion %u of its %u (capacity %u, entry %p): %s", number, count,
               *reinterpret_cast<uint16_t*>(set + 0x30), entry, bad ? "no such motion here, nothing is played" : "the entry reads as a motion");
        if (bad) return nullptr;
      }
    }
  }
  return g_motion_lookup(x, request, y);
}

// --- the player's own saves ---------------------------------------------------------------------------------
// A save written in a network game is not a single-player save: the game's checkpoint would replace the
// player's auto_save with one that does not load. So a joined game writes no saves at all (its world is a copy
// of the host's), and the host's checkpoints go to a save of their own, coop_auto_save. A save the host asks
// for by name keeps its name.
using SaveRequestFn = void (*)(void*, const char*);
SaveRequestFn g_save_request = nullptr;

void on_save_request(void* manager, const char* name) {
  if (game::ref<uintptr_t>(game::kNetHost) > 1) {
    if (game::ref<int>(game::kNetLocalPlayer) != 0) {
      LOGI("net: a joined game writes no save ('%s' dropped)", name && *name ? name : "auto_save");
      return;
    }
    if (!name || !*name || !strcmp(name, "auto_save")) {
      LOGI("net: a checkpoint while hosting goes to coop_auto_save");
      name = "coop_auto_save";
    }
  }
  g_save_request(manager, name);
}

// With mode = host the game has an autosave of its own, coop_auto_save: every "auto_save" the game writes or
// reads is that one - the checkpoints, the level end's "save_player <next level>" (it writes the carried-over
// inventory and, past the save request above, an auto_save that starts the next level), the menu's Continue,
// "load the last checkpoint" after the host's death. The player's solo autosave is neither written nor read.
// The first time the game runs as a host, coop_auto_save is made as a copy of the solo autosave, so a solo
// game can be carried on together.
using SaveNameFn = void* (*)(void*, void*, const char*);
SaveNameFn g_save_name = nullptr;

void start_coop_save() {
  static bool done = false;
  if (done) return;
  done = true;
  wchar_t folder[MAX_PATH] = {};
  uint8_t* prefix = game::ref<uint8_t*>(game::kSavePrefix);
  if (!prefix) return;
  // A test run has a user folder of its own (the lab redirects the game's own call, not this one): the
  // player's real folder must never be touched from there.
  if (!g_cfg.lab_userdir.empty())
    lstrcpynW(folder, g_cfg.lab_userdir.c_str(), MAX_PATH);
  else if (g_cfg.lab || !SHGetSpecialFolderPathW(nullptr, folder, CSIDL_LOCAL_APPDATA, FALSE))
    return;
  wchar_t name[64] = {};
  MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<const char*>(prefix) + 0x14, -1, name, 32);
  std::wstring base = std::wstring(folder) + L"\\4A Games\\Metro 2033\\" + name;
  std::wstring solo = base + L"_auto_save", coop = base + L"_coop_auto_save";
  if (GetFileAttributesW(coop.c_str()) != INVALID_FILE_ATTRIBUTES) {
    LOGI("net: host mode: the game's autosave is coop_auto_save (it is there)");
  } else if (GetFileAttributesW(solo.c_str()) == INVALID_FILE_ATTRIBUTES) {
    LOGI("net: host mode: the game's autosave is coop_auto_save (none yet, and no solo autosave to start from)");
  } else if (CopyFileW(solo.c_str(), coop.c_str(), TRUE)) {
    LOGI("net: host mode: the game's autosave is coop_auto_save, started as a copy of the solo autosave");
  } else {
    LOGW("net: host mode: could not copy the solo autosave to coop_auto_save (error %lu)", GetLastError());
  }
}

}  // namespace

void backup_save() {
  static bool done = false;
  if (done) return;
  done = true;
  wchar_t folder[MAX_PATH] = {};
  uint8_t* prefix = game::ref<uint8_t*>(game::kSavePrefix);
  if (!prefix) return;
  if (!g_cfg.lab_userdir.empty())
    lstrcpynW(folder, g_cfg.lab_userdir.c_str(), MAX_PATH);
  else if (g_cfg.lab || !SHGetSpecialFolderPathW(nullptr, folder, CSIDL_LOCAL_APPDATA, FALSE))
    return;
  wchar_t name[64] = {};
  MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<const char*>(prefix) + 0x14, -1, name, 32);
  std::wstring save = std::wstring(folder) + L"\\4A Games\\Metro 2033\\" + name + L"_auto_save";
  if (GetFileAttributesW(save.c_str()) == INVALID_FILE_ATTRIBUTES) return;
  if (CopyFileW(save.c_str(), (save + L"_before_coop").c_str(), FALSE))
    LOGI("net: the autosave as it was before this session is kept as %ls_auto_save_before_coop", name);
  else
    LOGW("net: the autosave could not be copied before the session (error %lu)", GetLastError());
}

namespace {

void* on_save_name(void* x, void* out, const char* name) {
  if (g_cfg.net_mode == Config::NetMode::Host && (!name || !*name || !strcmp(name, "auto_save"))) {
    start_coop_save();
    name = "coop_auto_save";
  }
  return g_save_name(x, out, name);
}

// A joined game runs the level's scripts too. Its own level end must neither change the level (the host's
// does, and tells this game to follow with "change_map_cl") nor write the carried-over inventory and saves.
using ConsoleLineFn = void (*)(char*);
ConsoleLineFn g_console_line = nullptr;

// Loading a save in a session ("gameload": the player's own, the death screen's, a script's quick_load).
// - A joined game never loads one: its world is the host's.
// - The host may. But the load happens inside the running level, and with a joined player's body in the world
//   it crashes while the old world is taken down (exe+6FE4), and with the "network game" flag on the save is
//   read out of step. So the bodies are taken out first (the engine tells the joined games), the load runs a
//   moment later with the flag off, and when the level runs again the joined games are told to load it too.
std::string g_pending_load;      // the host's "gameload ..." line, held back
uint64_t g_pending_load_at = 0;
bool g_host_reloaded = false;    // the host's level came from a save load, not from "map"

bool is_command(const char* p, const char* name) {
  size_t n = strlen(name);
  return !strncmp(p, name, n) && (p[n] == ' ' || p[n] == 0);
}

// The game falls when it is closed while a video plays (the main menu's own, after a while of nobody touching
// anything: the video's thread uses what closing takes away - +E60FB, seen in the lab, the same without the
// mod's session). A "quit" while a video plays stops the video first and comes again a moment later.
std::atomic<bool> g_video{false};
std::atomic<unsigned long long> g_frame_seen{0};  // when the mod's frame ran last (net::frame)
std::atomic<uint64_t> g_quit_at{0};

void on_video_line(const char* line) {
  if (strstr(line, "PUSH PAUSE - REASON [play_video]")) g_video.store(true);
  else if (strstr(line, "POP PAUSE - REASON [play_video]")) g_video.store(false);
}

// The pause menu in a session. In a network game Esc asks for the pause menu "default_mp" - a page of the old
// multiplayer of the game's makers, left without working items (0.4.1: "Esc gives a menu where nothing can be
// chosen"). The single-player one ("default") is opened instead; it would stop the game's clock, which on the host
// stops the world of everybody - so in a session its pause is taken off again (frame(): the menu stays, the world
// goes on, as in any network game).
bool g_menu_unpause = false;

void on_console_line(char* line) {
  if (line && game::ref<uintptr_t>(game::kNetHost) > 1 && game::ref<uint8_t>(game::kNetGame)) {
    const char* q = line;
    while (*q == ' ' || *q == '^') ++q;
    if (is_command(q, "mainmenu") && strstr(q, "default_mp")) {
      LOGI("net: the pause menu of a session: the single-player one instead of 'default_mp'");
      char menu[] = "mainmenu default";
      g_menu_unpause = true;
      g_console_line(menu);
      return;
    }
  }
  if (line) {
    const char* q = line;
    while (*q == ' ' || *q == '^') ++q;
    // (only while the mod's frame runs: it gives the quit back)
    if (is_command(q, "quit") && g_video.load() && !g_quit_at.load() && GetTickCount64() - g_frame_seen.load() < 1000) {
      LOGI("net: quit while a video plays: the video is stopped first, the quit comes in a moment");
      char stop[] = "stop_video";
      g_console_line(stop);
      g_quit_at.store(GetTickCount64() + 700);
      return;
    }
  }
  if (line && game::ref<uintptr_t>(game::kNetHost) > 1) {
    const char* p = line;
    while (*p == ' ' || *p == '\t') ++p;
    if (game::ref<int>(game::kNetLocalPlayer) != 0) {
      // (The way to the main menu is the player's own: "Exit to the main menu" of the pause menu.)
      bool to_menu = is_command(p, "change_map") && strstr(p, "\\000");
      if (to_menu) {
        LOGI("net: a joined game leaves the session for the main menu ('%s')", p);
      } else if (is_command(p, "gameload") && game::ref<void*>(game::kLevel)) {
        // "Load the last checkpoint" of a joined player's pause menu: the host loads it, for everybody (a joined
        // game cannot load on its own: its world is the host's). 0.4.5 and before: nothing happened.
        uint8_t slot = static_cast<uint8_t>(game::ref<int>(game::kNetLocalPlayer));
        bool sent = link::send(link::kAskLoad, &slot, 1, true);
        LOGI("net: a joined player chose '%s': the host is asked to load the last checkpoint%s", p, sent ? "" : " (nobody to ask)");
        hud::notice("Хост загружает последнюю контрольную точку — для всех", "The host loads the last checkpoint, for everybody", 6);
        return;
      } else if (is_command(p, "change_map") || is_command(p, "save_player") || is_command(p, "gameload")) {
        LOGI("net: a joined game leaves '%s' to the host", p);
        return;
      }
    } else if (is_command(p, "gameload") && game::ref<void*>(game::kLevel)) {
      unsigned bodies = 0;
      for (int slot = 1; slot < 32; ++slot) bodies += remove_body(slot);
      g_pending_load = p;
      g_pending_load_at = GetTickCount64() + (bodies ? 700 : 0);
      LOGI("net: the host loads a save ('%s'): %u joined players' bodies taken out first", p, bodies);
      return;
    }
  }
  g_console_line(line);
}

// The host: a joined player chose "load the last checkpoint" in the pause menu - loaded for everybody, as the host's
// own choice would be (at most once in 15 s).
uint64_t g_asked_load_at = 0;

void on_ask_load(int from, const uint8_t*, size_t) {
  if (game::ref<int>(game::kNetLocalPlayer) != 0 || from < 1 || !game::ref<void*>(game::kLevel)) return;
  uint64_t now = GetTickCount64();
  if (now - g_asked_load_at < 15000) return;
  g_asked_load_at = now;
  std::string who = players::name(from);
  if (who.empty()) who = "Игрок " + std::to_string(from + 1);
  LOGI("net: player %d asks for the last checkpoint: loaded for everybody", from);
  hud::notice(who + " загружает последнюю контрольную точку", who + " loads the last checkpoint", 6);
  engine::command("gameload auto_save", true);
}

// The bodies the host made for the joined players (ids of the body and of everything it carries) are no part of
// the host's story: loaded in single player, such a save would hold a second player standing where the guest
// was. So the save loop does not see them while the host's own save is written (the same loop writes the quick
// save a joining player gets - that one keeps everything).
std::bitset<65536> g_body_ids;
std::vector<uint16_t> g_slot_body[32];  // per slot: the ids of its body's entities, the body itself last
std::vector<uint16_t> g_slot_removed[32];  // per slot: the same for the body taken out last
std::vector<uint16_t> g_slot_old[32];      // per slot: the body its player died in, until the new one is in use
std::atomic<bool> g_writing_save{false};
std::atomic<unsigned> g_left_out{0};

int on_save_owner(uint8_t* entity) {
  using Owner = int (*)(uint8_t*);
  int owner = (*reinterpret_cast<Owner**>(entity))[game::kEntitySaveOwner / 8](entity);
  if (owner == -1 && g_writing_save.load(std::memory_order_relaxed) &&
      g_body_ids.test(*reinterpret_cast<uint16_t*>(entity + game::kEntityId))) {
    g_left_out.fetch_add(1, std::memory_order_relaxed);
    return 0;  // "a part of something else": the loop goes on to the next entity
  }
  return owner;
}

// The host's save is written with the "network game" flag off: with it on the stream is another format, one
// a single-player load crashes on. The flag is back before the frame goes on.
using SaveQueuedFn = int (*)();
SaveQueuedFn g_save_queued = nullptr;

int on_save_queued() {
  uint8_t& flag = game::ref<uint8_t>(game::kNetGame);
  uint8_t was = flag;
  bool hosting = was && game::ref<uintptr_t>(game::kNetHost) > 1 && game::ref<int>(game::kNetLocalPlayer) == 0;
  if (hosting) {
    flag = 0;
    g_left_out = 0;
    g_writing_save = true;
  }
  int written = g_save_queued();
  if (hosting) {
    g_writing_save = false;
    flag = was;
    if (written) LOGI("net: the host's save was written (%u entities of joined players' bodies left out)", g_left_out.load());
  }
  return written;
}

// --- the UDP port --------------------------------------------------------------------------------------
// The engine binds UDP 1000 on the server and on a client alike, and connects to 1000. Two copies on one PC
// (the lab) cannot both have it: only the bind port is changed here, the port a client connects to stays.
struct EnetAddress {
  uint32_t host;
  uint16_t port;
};
using EnetHostCreateFn = void* (*)(EnetAddress*, size_t, size_t, uint32_t, uint32_t);
EnetHostCreateFn g_enet_host_create = nullptr;
bool g_port_taken = false;  // the hosting socket could not have its port: the game is hosted, but unreachable
bool g_hosting_now = false; // inside the engine's net start of a hosting game

void* on_enet_host_create(EnetAddress* address, size_t peers, size_t channels, uint32_t in_bw, uint32_t out_bw) {
  if (address && g_cfg.lab_bind_port >= 0) {
    LOGI("net: socket port %u -> %d", address->port, g_cfg.lab_bind_port);
    address->port = static_cast<uint16_t>(g_cfg.lab_bind_port);
  } else if (address && !g_hosting_now && (steamnet::join_by_steam(g_cfg) || steamnet::joining())) {
    // A game that joins through Steam sends nothing through its own socket: it is kept on this PC only (any free
    // port of 127.0.0.1), so Windows' firewall has nothing to ask about.
    address->host = 0x0100007F;
    address->port = 0;
  }
  void* host = g_enet_host_create(address, peers, channels, in_bw, out_bw);
  LOGI("net: ENet host %p (port %u, %zu peers)", host, address ? address->port : 0, peers);
  if (!host && address) {
    // The port is taken (another program, another copy of the game). The engine goes on with the null host and
    // falls at exe+8EBA2F. A joining game needs no particular port of its own: any free one does. A hosting game
    // cannot be reached on another port, but it must not fall: it gets a free one too and the log says so.
    uint16_t wanted = address->port;
    address->port = 0;
    host = g_enet_host_create(address, peers, channels, in_bw, out_bw);
    if (g_hosting_now)
      LOGE("net: UDP port %u is taken by another program: nobody can join this game (socket %p on a free port instead)", wanted, host);
    else
      LOGW("net: UDP port %u is taken on this PC: a free port is used for the own socket (%p)", wanted, host);
    g_port_taken = g_hosting_now;
  }
  return host;
}

// --- a copied body and its weapons ---------------------------------------------------------------------------
// A player's body tells its inventory which child is the weapon of slot 16 and which the one of slot 2 by two
// links (actor + 0xDF8 / + 0xE08: {handle*, u16 id, bool valid}); a child whose id is in a link gets that slot
// when the inventory takes it, the others take a free slot as they come. A saved game writes the two ids; a
// body that is copied live (a joined player's body is a copy of the host's) has both links empty. The copies
// of the weapons are then taken last-to-first into "any" slot: the first one lands in slot 2, the next in
// slot 1, the third one pushes the first out. The pushed-out weapon is thrown into the world: it hangs before
// the joined player's eyes, both players get a "take it" prompt, and the game removes it ten seconds later.
// So right before the copy's children are taken (the game calls the body's "links to the new ids" then, with
// the clone list still at hand) the two links are filled in from the slots the originals sit in.
struct CloneList {
  uint16_t* data;  // {entity that is copied, its copy's id, the copy's parent id} x size
  uint16_t capacity;
  uint16_t size;
};
const CloneList* g_cloning = nullptr;  // the list of the clone being made right now (main thread)
bool g_lab_old_ammo = false;  // the lab: ammunition as 0.1.2 handled it

using CloneCoreFn = void* (*)(void*, const CloneList*, void*, void*);
CloneCoreFn g_clone_core = nullptr;

uint16_t g_clone_root = 0xFFFF;  // the id of the last copy made: the body itself is the last entry of its list

void* on_clone_core(void* server, const CloneList* list, void* sources, void* copies) {
  const CloneList* before = g_cloning;
  g_cloning = list;
  if (list && list->data && list->capacity) g_clone_root = list->data[(list->capacity - 1) * 3 + 1];
  void* r = g_clone_core(server, list, sources, copies);
  g_cloning = before;
  return r;
}

uint8_t* as_item(uint8_t* e) {
  using AsItem = uint8_t* (*)(uint8_t*);
  return e ? (*reinterpret_cast<AsItem**>(e))[game::kEntityAsItem / 8](e) : nullptr;
}

int g_spawning_slot = -1;  // the player slot whose body is being made right now, -1: none / not a player's body

using SpawnRunFn = void (*)(void*, uint8_t*);
SpawnRunFn g_spawn_run = nullptr;

// The body of the player on this PC (a joined game): its id and when it was made, until tick() has seen the game
// hand it over (see own_body_ready).
uint16_t g_own_body = 0xFFFF;
ULONGLONG g_own_body_at = 0;

void on_spawn_run(void* x, uint8_t* event) {
  int before = g_spawning_slot;
  g_spawning_slot = event && (event[0] & 0x1F) == 6 ? (*reinterpret_cast<uint16_t*>(event + 8) >> 8) & 0x1F : -1;
  int slot = g_spawning_slot;
  g_clone_root = 0xFFFF;
  g_spawn_run(x, event);
  g_spawning_slot = before;
  if (slot > 0 && slot == game::ref<int>(game::kNetLocalPlayer) && g_clone_root != 0xFFFF) {
    g_own_body = g_clone_root;
    g_own_body_at = GetTickCount64();
  }
}

int item_slot(uint8_t* e) {
  uint8_t* item = as_item(e);
  return item ? item[game::kItemSlot] : -1;
}

using PlayerRemapFn = void* (*)(uint8_t*, void*);
PlayerRemapFn g_player_remap = nullptr;

void* on_player_remap(uint8_t* player, void* map) {
  void* r = g_player_remap(player, map);
  // (only a player's body made by the network spawn event: nothing changes for the game's own clones)
  if (!g_cloning || !player || g_spawning_slot <= 0) return r;
  uint16_t body = *reinterpret_cast<uint16_t*>(player + game::kEntityId);
  // (the game counts the list's size down while it makes the copies: the entries are all there, up to capacity)
  const uint16_t count = g_cloning->capacity;
  if (g_cfg.lab) {
    std::string all;
    for (uint16_t i = 0; i < count && i < 48; ++i) {
      char one[40];
      snprintf(one, sizeof(one), " %X>%X^%X", g_cloning->data[i * 3], g_cloning->data[i * 3 + 1], g_cloning->data[i * 3 + 2]);
      all += one;
    }
    LOGD("net: [trace] links of body %X, clone list of %u (cap %u):%s", body, g_cloning->size, g_cloning->capacity, all.c_str());
  }
  for (uint16_t i = 0; i < count; ++i) {
    if (g_cloning->data[i * 3 + 2] != body) continue;  // not a child of this body
    int slot = item_slot(entity_by_id(g_cloning->data[i * 3]));
    size_t link = slot == 16 ? game::kActorWeaponMain : slot == 2 ? game::kActorWeaponSecond : 0;
    if (!link) continue;
    *reinterpret_cast<uint16_t*>(player + link + 8) = g_cloning->data[i * 3 + 1];
    *(player + link + 10) = 1;
    LOGI("net: a copied body %X: the weapon of slot %d is the copy %X (of %X)", body, slot, g_cloning->data[i * 3 + 1],
         g_cloning->data[i * 3]);
  }
  return r;
}

// The copies do not carry their originals' inventory slots either (a copied weapon comes with the slot of its
// kind, ammunition with none at all - 255 - and is thrown out or removed at once): while a clone is being made,
// a child gets the slot its original sits in before the body's inventory takes it.
using ActorTakeFn = void* (*)(uint8_t*, uint8_t*, uint8_t*);
ActorTakeFn g_actor_take = nullptr;

void* on_actor_take(uint8_t* actor, uint8_t* child, uint8_t* flags) {
  loot::lab_on_take(actor, child, flags);
  bool copy = actor && child && (*reinterpret_cast<uint32_t*>(child + game::kEntityFlags) & 0x10000);
  if (g_cloning && g_spawning_slot > 0 && actor && child) {  // (a player's body only, as above)
    uint16_t id = *reinterpret_cast<uint16_t*>(child + game::kEntityId);
    for (uint16_t i = 0; i < g_cloning->capacity; ++i) {
      if (g_cloning->data[i * 3 + 1] != id) continue;
      int slot = item_slot(entity_by_id(g_cloning->data[i * 3]));
      uint8_t* item = as_item(child);
      if (item && slot >= 0 && slot != 255 && item[game::kItemSlot] != slot) {
        LOGD("net: a copied body: %X gets slot %d of its original %X (it came with %d)", id, slot, g_cloning->data[i * 3],
             item[game::kItemSlot]);
        item[game::kItemSlot] = static_cast<uint8_t>(slot);
      }
      break;
    }
  }
  // Ammunition. The game takes a copied child as if it were picked up (flag 4): ammunition is then added to the
  // stack the inventory already has and the picked-up item is removed - but a copied body's inventory is empty,
  // so every copy of the host's ammunition is simply destroyed (on the host, which then tells everybody), and
  // the joined player has nothing to reload with; the on-screen ammunition list reads rubbish for the missing
  // stacks and the game falls on the first reload. While a player's body is being made:
  //  - the inventory of the player this PC controls takes ammunition as a new stack (no flags);
  //  - any other inventory (the simple one of a body somebody else controls) does not take it at all: the
  //    item stays a child of the body, alive on every PC, and is registered when the inventory becomes a
  //    player's one.
  uint8_t own_flags = flags ? *flags : 0;
  if (g_cloning && !g_lab_old_ammo && child && g_spawning_slot > 0 &&
      *reinterpret_cast<uintptr_t*>(child) == game::at(game::kAmmoClass)) {
    uint8_t* inventory = *reinterpret_cast<uint8_t**>(actor + game::kActorInventory);
    bool players = inventory && *reinterpret_cast<uintptr_t*>(inventory) == game::at(game::kPlayerInventory);
    own_flags = players ? 0 : static_cast<uint8_t>(own_flags | 0x10);
    flags = &own_flags;
  }
  void* r = g_actor_take(actor, child, flags);
  if (g_cfg.lab && copy) {
    const uint8_t* name = *reinterpret_cast<uint8_t**>(child + game::kEntityName);
    LOGD("net: [trace] actor %X took %X '%s' (flags byte %X, cloning %d): slot %d, hangs on %X",
         *reinterpret_cast<uint16_t*>(actor + game::kEntityId), *reinterpret_cast<uint16_t*>(child + game::kEntityId),
         name ? reinterpret_cast<const char*>(name) + 0x14 : "", flags ? *flags : 0, g_cloning != nullptr, item_slot(child),
         parent_of(child) ? *reinterpret_cast<uint16_t*>(parent_of(child) + game::kEntityId) : 0xFFFF);
  }
  return r;
}

// --- an entity that goes while its own "remove me" timer is armed ----------------------------------------
// A thing that lies in the world for a limited time (the weapon a dying player lets go of: ten seconds) has a
// timer in the game's queue with a bare pointer to the entity. In single player only that timer ever removes
// such a thing. In a session it can go earlier - the host takes a dead player's body with all it carried out of
// the world, and every PC does the same on the host's word - and then the timer fires on freed memory (a fall
// in the game of the host, a hang in a joined one, about ten seconds after the death). Whatever removes an
// entity, its timer is taken off first: life time 0, then the game's own "arm / disarm".
using DestroyFn = void (*)(void*, void**);
DestroyFn g_destroy_now = nullptr;

// lab: what was taken out lately, and the thread the game's frame runs on (both for the crash log)
struct GoneNote {
  uintptr_t at;
  uint16_t id;
  uint32_t flags;
  ULONGLONG when;
  char name[40];
  uint32_t words_n;         // the entity's own memory as it was (pointers to its parts), for lab_held_by
  uintptr_t words[768];
};
GoneNote g_gone_ring[256] = {};
std::atomic<unsigned> g_gone_count{0};
std::atomic<unsigned long> g_main_thread{0};
std::atomic<ULONGLONG> g_last_gone_event{0};
std::atomic<ULONGLONG> g_last_frame{0};
std::atomic<int> g_flush_lines{0};

void on_destroy_now(void* server, void** handle) {
  uint8_t** h = handle ? reinterpret_cast<uint8_t**>(*handle) : nullptr;
  uint8_t* e = h && *h ? *h - game::kEntityHandle : nullptr;
  // (only what the mod's bodies consist of - copies, flag 0x10000 - and only a timer that is really armed: the
  // two fields mean something else in entities of other kinds)
  if (e && game::ref<uintptr_t>(game::kNetHost) > 1 && (*reinterpret_cast<uint32_t*>(e + game::kEntityFlags) & 0x10000) &&
      *reinterpret_cast<int*>(e + game::kEntityLife) > 0 &&
      *reinterpret_cast<int*>(e + game::kEntityLife + 4) > 0) {
    *reinterpret_cast<int*>(e + game::kEntityLife) = 0;
    using LifeTimer = void (*)(uint8_t*);
    reinterpret_cast<LifeTimer>(game::at(game::kLifeTimer))(e);
    LOGD("net: entity %X goes with its removal timer armed: the timer is taken off",
         *reinterpret_cast<uint16_t*>(e + game::kEntityId));
  }
  if (g_cfg.lab && e) {  // who takes entities out of the world (the caller chain goes into the log)
    const uint8_t* name = *reinterpret_cast<uint8_t**>(e + game::kEntityName);
    const char* text = name ? reinterpret_cast<const char*>(name) + 0x14 : "";
    GoneNote& note = g_gone_ring[g_gone_count.fetch_add(1) % 256];
    note.at = reinterpret_cast<uintptr_t>(e);
    note.id = *reinterpret_cast<uint16_t*>(e + game::kEntityId);
    note.flags = *reinterpret_cast<uint32_t*>(e + game::kEntityFlags);
    note.when = GetTickCount64();
    strncpy(note.name, text, sizeof(note.name) - 1);
    note.name[sizeof(note.name) - 1] = 0;
    note.words_n = 0;
    for (unsigned part = 0; part < 768 / 64; ++part) {  // (the object's size is not known: as far as it reads)
      if (IsBadReadPtr(e + part * 512, 512)) break;
      memcpy(note.words + part * 64, e + part * 512, 512);
      note.words_n = (part + 1) * 64;
    }
    if ((*reinterpret_cast<uint32_t*>(e + game::kEntityFlags) & 0x10000) || !strncmp(text, "wpn_", 4)) {
      void* frames[10] = {};
      USHORT n = RtlCaptureStackBackTrace(1, 10, frames, nullptr);
      std::string chain;
      for (USHORT i = 0; i < n; ++i) {
        uintptr_t a = reinterpret_cast<uintptr_t>(frames[i]);
        char one[24];
        if (a >= game::at(0) && a < game::at(0) + game::kSizeOfImage)
          snprintf(one, sizeof(one), " +%llX", static_cast<unsigned long long>(a - game::at(0)));
        else
          snprintf(one, sizeof(one), " mod");
        chain += one;
      }
      LOGD("net: [trace] entity %X '%s' flags %X is taken out by:%s", *reinterpret_cast<uint16_t*>(e + game::kEntityId),
           text, *reinterpret_cast<uint32_t*>(e + game::kEntityFlags), chain.c_str());
    }
  }
  g_destroy_now(server, handle);
}

// --- a HUD model that is not there yet ----------------------------------------------------------------------
// The game sets a first-person model up in two steps: "start the model" (0x25FA40, reads holder + 8 at once) and
// only then "make the model if there is none". In single player the model of the hands always exists by then.
// In a session it may not: a joined player killed again within a second or two of getting a new body has the
// next body made while the dead one's weapon is still being put away, and the hands' model is asked for before
// it exists - a fall at exe+25FA51. With no model the first step is left out; the second one makes the model
// and runs the first step again itself.
using HudModelStartFn = void* (*)(uint8_t*);
HudModelStartFn g_hud_model_start = nullptr;

void* on_hud_model_start(uint8_t* holder) {
  if (holder && !*reinterpret_cast<void**>(holder + 8)) {
    static int told = 0;
    if (told++ < 5) LOGW("net: a HUD model is started before it is made: left to the game's own next step");
    return nullptr;
  }
  return g_hud_model_start(holder);
}

// --- players do not hurt each other ------------------------------------------------------------------------
// Every hit goes through one filter on the PC whose bullet made it (and again on the host when a joined game
// sends it): "may this hit event pass". The game's own rule for two players is the console variable
// net_friendly_fire, 1 by default - but the two directions are far from equal (the host's bullets find a joined
// player's body every time, seven hits kill; a joined player's bullets find the host's body about once in
// fifty, and a host brought to zero health that way does not die at all). In a story played together a stray
// burst should not kill the partner: a hit of one player's body by another is dropped, unless
// [net] friendly_fire = 1. Hits on and by everything else pass as before.
using HitFilterFn = int64_t (*)(uint16_t*, void*);
HitFilterFn g_hit_filter = nullptr;

bool is_player(uint16_t id) {
  uint8_t* e = entity_by_id(id);
  return e && *reinterpret_cast<uintptr_t*>(e) == game::at(game::kPlayerClass);
}

// A body that has just been made is not hurt for three seconds: a player who comes back next to the host in the
// middle of a fight would die again at once (and the game does badly when a body dies while it is still being
// handed over). The host is the one that lets hits pass.
ULONGLONG g_body_made_at[32] = {};
uint16_t g_body_made[32] = {};
constexpr ULONGLONG kNewBodySafeMs = 3000;

bool new_body(uint16_t id) {
  ULONGLONG now = GetTickCount64();
  for (int slot = 1; slot < 32; ++slot)
    if (g_body_made[slot] == id && g_body_made_at[slot] && now - g_body_made_at[slot] < kNewBodySafeMs) return true;
  return false;
}

int64_t on_hit_filter(uint16_t* event, void* queue) {
  if (event && game::ref<uintptr_t>(game::kNetHost) > 1 && new_body(event[2])) return 0;
  if (event && !g_cfg.net_friendly_fire && game::ref<uintptr_t>(game::kNetHost) > 1 && event[1] != event[2] &&
      is_player(event[1]) && is_player(event[2])) {
    static int told = 0;
    if (told++ < 3) LOGI("net: a hit of player %X by player %X is dropped (friendly fire is off)", event[2], event[1]);
    return 0;
  }
  int64_t r = g_hit_filter(event, queue);
  static int shown = 0;
  if (g_cfg.lab && event && shown < 400) {
    ++shown;
    LOGD("net: [trace] hit event kind %u: %X hits %X, words %04X %04X %04X %04X -> %lld", event[0] & 0x1F, event[1], event[2],
         event[3], event[4], event[5], event[6], static_cast<long long>(r));
  }
  return r;
}

using QueueDestroyFn = void (*)(void*, void**, void*, int);
QueueDestroyFn g_queue_destroy_traced = nullptr;

void on_queue_destroy_traced(void* a1, void** handle, void* a3, int a4) {
  uint8_t** h = handle ? reinterpret_cast<uint8_t**>(*handle) : nullptr;
  uint8_t* e = h && *h ? *h - game::kEntityHandle : nullptr;
  if (e && (*reinterpret_cast<uint32_t*>(e + game::kEntityFlags) & 0x10000)) {
    const uint8_t* name = *reinterpret_cast<uint8_t**>(e + game::kEntityName);
    void* frames[12] = {};
    USHORT n = RtlCaptureStackBackTrace(1, 12, frames, nullptr);
    std::string chain;
    for (USHORT i = 0; i < n; ++i) {
      uintptr_t a = reinterpret_cast<uintptr_t>(frames[i]);
      char one[24];
      if (a >= game::at(0) && a < game::at(0) + game::kSizeOfImage)
        snprintf(one, sizeof(one), " +%llX", static_cast<unsigned long long>(a - game::at(0)));
      else
        snprintf(one, sizeof(one), " mod");
      chain += one;
    }
    LOGD("net: [trace] removal of entity %X '%s' flags %X is queued (%d) by:%s", *reinterpret_cast<uint16_t*>(e + game::kEntityId),
         name ? reinterpret_cast<const char*>(name) + 0x14 : "", *reinterpret_cast<uint32_t*>(e + game::kEntityFlags), a4,
         chain.c_str());
  }
  g_queue_destroy_traced(a1, handle, a3, a4);
}

// --- "the player" of a level script ----------------------------------------------------------------------------
// Every script block that acts on the player asks one helper for it (49 places: actions/player/*, the player's
// checks). In single player the helper answers "the body this PC controls". In a network game it answers "the
// entity this script was started by, if that is a player" - the multiplayer the engine was written for has no
// level script that speaks about "the" player. A story level's script is started by the level, a timer, a
// trigger zone: the answer is "nobody", and the blocks either do nothing (movement locks, the gas mask, hidden
// hands, scene cameras never reach the player) or fall (l00_intro, exe+426719: both games, 40 s into the level).
// So in a session "nobody" becomes the single-player answer: the body this PC controls. Every PC runs the
// level's script itself, and each one's blocks reach its own player.
using ScriptPlayerFn = uint8_t* (*)(void*, int);
ScriptPlayerFn g_script_player = nullptr;

uint8_t* on_script_player(void* context, int net_game) {
  uint8_t* player = g_script_player(context, net_game);
  if (!player && net_game) {
    player = g_script_player(context, 0);
    static int told = 0;
    if (told++ < 3) LOGI("net: a level script asked for the player: given the body of this PC (%p)", player);
  }
  return player;
}

// --- "the player has walked in" ---------------------------------------------------------------------------------
// A story level is driven by zones that wait for the player ("triggers/zone/player"): walk in - the ambush
// starts, the door opens, the talk begins; walk out - the other half. Such a zone watches one entity, and with
// its reference left empty (as the levels have it) that is the body this PC controls. In a session each PC's
// zones then wait for that PC's own player only: the host walks in - the scene starts on the host's PC and not
// on the others'; a joined player runs ahead - the scene starts on its PC alone.
// Here every player's body counts, on every PC, with one zone state for all of them: the zone is entered when
// the first player walks in and left when the last one walks out. The zone itself is told that it was this PC's
// own body - what follows in the script (player actions, the scene's camera) then reaches each PC's own player.
constexpr uintptr_t kPlayerZoneClass = 0xB70D20;  // vtable of the "triggers/zone/player" block
constexpr size_t kZoneCallbacks = 104;            // block + : the zone's callback interface (enter, leave)
using ZoneEventFn = int64_t (*)(uint8_t*, void**);
ZoneEventFn g_zone_enter = nullptr, g_zone_leave = nullptr;

struct ZonePlayers {
  uint8_t* zone;
  std::vector<uint16_t> inside;
};
std::vector<ZonePlayers> g_zone_players;
void* g_zone_level = nullptr;

// How many players are inside after this event (bodies that are gone are forgotten); -1: not the mod's case;
// -2: a player in a player zone, but this PC has no body to speak for.
int zone_players(uint8_t* callbacks, void** entity, bool enters, bool* was_empty) {
  if (game::ref<uintptr_t>(game::kNetHost) <= 1 || !entity || !*entity) return -1;
  uint8_t* zone = callbacks - kZoneCallbacks;
  if (*reinterpret_cast<uintptr_t*>(zone) != game::at(kPlayerZoneClass)) return -1;
  uint8_t* target = *reinterpret_cast<uint8_t**>(*entity);
  if (!target) return -1;
  uint8_t* e = target - game::kEntityHandle;
  if (*reinterpret_cast<uintptr_t*>(e) != game::at(game::kPlayerClass)) return -1;
  if (!controlled()) return -2;  // no body of this PC's yet: its script cannot be told, and nothing is remembered
  void* level = game::ref<void*>(game::kLevel);
  if (level != g_zone_level) {  // another level: its zones are other objects
    g_zone_level = level;
    g_zone_players.clear();
  }
  ZonePlayers* z = nullptr;
  for (ZonePlayers& one : g_zone_players)
    if (one.zone == zone) z = &one;
  if (!z) {
    g_zone_players.push_back({zone, {}});
    z = &g_zone_players.back();
  }
  size_t keep = 0;
  for (uint16_t id : z->inside)
    if (is_player(id)) z->inside[keep++] = id;
  z->inside.resize(keep);
  uint16_t id = *reinterpret_cast<uint16_t*>(e + game::kEntityId);
  bool known = std::find(z->inside.begin(), z->inside.end(), id) != z->inside.end();
  *was_empty = z->inside.empty();
  if (enters && !known) z->inside.push_back(id);
  if (!enters && known) z->inside.erase(std::find(z->inside.begin(), z->inside.end(), id));
  static int told = 0;
  if (told++ < 30)
    LOGI("net: player body %X %s a player zone (%p): %zu inside now", id, enters ? "walks into" : "walks out of", zone, z->inside.size());
  return static_cast<int>(z->inside.size());
}

// What a zone does "to the player" is of two kinds. The story (a scene, a door, a character's line, the handcar's
// start) goes by the shared state above: once for all, when the first one is in / the last one is out. What is the
// player's own business where they stand - the weapon lowered at a station ("civil mode", "friend mode"), the gas
// mask and its filters, the suit, fire, the light's adaptation, the sprint - belongs to each player's own steps:
// a player outside the station keeps the weapon up while another walks in. (0.4.1: "zones fire for everybody".)
// The zone's signal (exe+706270: its list of {object, function} links, called one by one) is split: the shared
// event leaves out the links to such blocks, and every step of this PC's own body in or out fires those links
// alone. Blocks reached further down a chain (a zone -> a delay -> civil mode) still go with the shared event.
constexpr uintptr_t kPersonalBlocks[] = {
    0xB5C020,  // actions/player/civil mode
    0xB5C8B0,  // actions/player/friend mode (friend_mode, friend_mode_target)
    0xB5BE80,  // actions/player/in flames
    0xB56970,  // actions/player/wear gasmask
    0xB56DD0,  // actions/player/gasmask effect
    0xB84E80,  // actions/player/gasmask hit
    0xB56BB0,  // actions/player/gasmask spend speed
    0xB6EDA0,  // actions/player/gasmask filter limits
    0xB59680,  // actions/player/filters_spend
    0xB667E0,  // actions/player/wear suit
    0xB56FF0,  // actions/player/wear nightvision
    0xB6E560,  // actions/player/disable sprint track
    0xB6A0B0,  // actions/player/luminocity correction
    0xB5CAC0,  // actions/player/restrictor obstacle
    0xB53E60,  // actions/player/platform_attach (a lift's floor carries the player who stands on it)
    0xB54070,  // actions/player/platform_detach
};
// What a thing's "use" does to the player who uses it - the scene of a wheel, a lever, a hatch: the camera taken to
// the thing, the hands' and body's animation, the player hung on the thing, the HUD hidden. When another player's
// use is played on this PC (the door must open here too), none of that may reach this PC's player (0.4.2: "both the
// host and the friend were pulled to the wheel, the door broke"): only the one who used it goes through the scene,
// the others see the thing move. Blocks of these classes are left out of a replayed use, at any depth of its chain.
constexpr uintptr_t kSceneBlocks[] = {
    0xB5BC80,  // actions/player/hide subj
    0xB5BAE0,  // actions/player/show subj
    0xB5B940,  // actions/player/hide hands
    0xB5B7A0,  // actions/player/show hands
    0xB5B170,  // actions/player/attach camera
    0xB5AF60,  // actions/player/lock camera
    0xB85B50,  // actions/player/play camera-effect
    0xB6B2B0,  // actions/player/play dof
    0xB6E350,  // actions/player/play hud anim
    0xB50CA0,  // actions/player/anim movement
    0xB59AC0,  // actions/player/movement
    0xB5D510,  // actions/player/random controls
    0xB5C630,  // actions/player/visible hud
    0xB5B5A0,  // actions/player/effect switcher
    0xBE87E0,  // actions/player/play vibration
    0xB84620,  // actions/player/savepos
    0xB6D750,  // actions/player/disable all use
};
bool g_replaying_use = false;  // another player's use is being played on this PC now
int g_replay_slot = -1;        // ... whose
bool g_own_chain = false;      // this PC's own player's use is being fired now
int g_scene_told = 0;
int g_remote_depth = 0;        // inside the chain of another player's use (any of its later steps too)
int g_remote_slot = -1;        // ... whose

// The chain of a player's use goes on later by itself (a delay, the end of a step, a held key): the blocks it went
// through are remembered for a while with whose use it was, and a signal out of one of them is still that player's
// chain - another player's here (its scene stays theirs), or this PC's own (drive.cpp tells the others what its
// "motion control" blocks are told).
struct Tainted {
  uint8_t* block;
  uint64_t until;
  int slot;
};
constexpr int kZoneTagBase = 100;  // slots from here up: a zone's chain (see ZoneTag below), not a player's use
std::vector<Tainted> g_tainted;

void taint(uint8_t* block, uint64_t now, int slot) {
  for (Tainted& t : g_tainted)
    if (t.block == block) {
      t.until = now + (slot >= kZoneTagBase ? 30000 : 25000);
      t.slot = slot;
      return;
    }
  if (g_tainted.size() < 1024) g_tainted.push_back({block, now + (slot >= kZoneTagBase ? 30000 : 25000), slot});
}

// The chain an output (or a block) is in: the slot of the player whose use it is, else -1.
int tainted(const uint8_t* output, uint64_t now) {
  if (g_tainted.empty()) return -1;
  int slot = -1;
  uint64_t newest = 0;
  size_t keep = 0;
  for (size_t i = 0; i < g_tainted.size(); ++i) {
    if (g_tainted[i].until <= now) continue;
    if (output >= g_tainted[i].block && output < g_tainted[i].block + 0x100 && g_tainted[i].until >= newest) {
      slot = g_tainted[i].slot;
      newest = g_tainted[i].until;
    }
    g_tainted[keep++] = g_tainted[i];
  }
  g_tainted.resize(keep);
  return slot;
}

// A zone's shared event that another player's step started goes on later through its chain (a delay, a counter):
// what that chain does to "the player" further down (civil mode at a station, the gas mask) belongs to the player
// who walked in, not to this PC's (0.4.1 - 0.4.5: it went to everybody). The blocks of such a chain are marked with
// a zone tag (slot numbers from 100 up); the player blocks it reaches are not fired here, but kept, and fired when
// this PC's own player walks into the zone (or out of it, for a "leave" chain).
struct ZoneTag {
  uint8_t* zone;
  bool enters;
  std::vector<std::pair<uint8_t*, void*>> kept;  // {block, its function}
};
std::vector<ZoneTag> g_zone_tags;
int g_zone_chain = -1;  // the tag of the shared event firing now (another player's step), else -1

int zone_tag(uint8_t* zone, bool enters) {
  for (size_t i = 0; i < g_zone_tags.size(); ++i)
    if (g_zone_tags[i].zone == zone && g_zone_tags[i].enters == enters) return kZoneTagBase + static_cast<int>(i);
  if (g_zone_tags.size() >= 256) return -1;
  g_zone_tags.push_back({zone, enters, {}});
  return kZoneTagBase + static_cast<int>(g_zone_tags.size() - 1);
}

enum ZoneScope { kScopeNone, kScopeShared, kScopeOwn };
ZoneScope g_zone_scope = kScopeNone;
uint8_t* g_zone_firing = nullptr;  // the zone block whose event runs now
using SignalFn = void (*)(uint8_t*);
SignalFn g_signal = nullptr;
int g_zone_split_told = 0;

uintptr_t block_class(const uint8_t* obj) {
  for (int back : {0, 8}) {
    const uint8_t* p = obj - back;
    if (IsBadReadPtr(p, 8)) continue;
    uintptr_t vt = *reinterpret_cast<const uintptr_t*>(p);
    for (uintptr_t c : kPersonalBlocks)
      if (vt == game::at(c)) return c;
  }
  return 0;
}

uintptr_t scene_class(const uint8_t* obj) {
  for (int back : {0, 8}) {
    const uint8_t* p = obj - back;
    if (IsBadReadPtr(p, 8)) continue;
    uintptr_t vt = *reinterpret_cast<const uintptr_t*>(p);
    for (uintptr_t c : kSceneBlocks)
      if (vt == game::at(c)) return c;
  }
  return block_class(obj);
}

// A block of the scene in another player's use: not played here (it would take this PC's player: the camera, the
// hands, the body), only finished - its "done" (vtbl + 0x180, what each action calls when it is over: at once, or
// at the end of its animation) - so that what the story does after it (the door, the character going on) happens.
void finish_block(uint8_t* obj, uintptr_t cls) {
  uint8_t* base = obj;
  if (*reinterpret_cast<uintptr_t*>(obj) != game::at(cls) && !IsBadReadPtr(obj - 8, 8) && *reinterpret_cast<uintptr_t*>(obj - 8) == game::at(cls))
    base = obj - 8;
  if (g_scene_told++ < 60)
    LOGI("net: another player's use: its scene block (+%llX) is finished here without being played", static_cast<unsigned long long>(cls));
  reinterpret_cast<void (*)(uint8_t*)>((*reinterpret_cast<void***>(base))[0x180 / 8])(base);
}

// The links of an output fired as the chain of player `slot`'s use: every block it reaches is that chain's.
// Another player's: the scene blocks are only finished while that player is still at the thing (its action: the
// hands on the wheel, the camera at it); what the chain does later - a scene the story goes on with - plays here
// too, as any scene of the level (the user, 2026-11-26: a shared screen in a cutscene, not in an action).
void fire_chain(uint8_t* output, int slot) {
  uint64_t now = GetTickCount64();
  int me = game::ref<int>(game::kNetLocalPlayer);
  uint16_t n = *reinterpret_cast<uint16_t*>(output + 26);
  uint8_t** links = *reinterpret_cast<uint8_t***>(output + 16);
  if (slot == me) {  // this PC's own: only remembered
    for (unsigned i = 0; i < n && links; ++i) {
      uint8_t* link = links[i];
      if (!link) continue;
      uint8_t* obj = *reinterpret_cast<uint8_t**>(link);
      if (!obj) continue;
      taint(obj, now, slot);
      if (!IsBadReadPtr(obj - 8, 8)) taint(obj - 8, now, slot);
      if (g_cfg.lab && g_scene_told++ < 200) {
        uintptr_t v0 = *reinterpret_cast<uintptr_t*>(obj), v8 = IsBadReadPtr(obj - 8, 8) ? 0 : *reinterpret_cast<uintptr_t*>(obj - 8);
        LOGI("lab: this player's use goes through a block: obj %p vtbl +%llX / (-8) +%llX, fn +%llX", static_cast<void*>(obj),
             static_cast<unsigned long long>(v0 - game::at(0)), static_cast<unsigned long long>(v8 - game::at(0)),
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(*reinterpret_cast<void**>(link + 8)) - game::at(0)));
      }
      reinterpret_cast<void (*)(uint8_t*)>(*reinterpret_cast<void**>(link + 8))(obj);
    }
    return;
  }
  int was_slot = g_remote_slot;
  g_remote_slot = slot;
  bool action = drive::acting(slot);
  ++g_remote_depth;
  for (unsigned i = 0; i < n && links; ++i) {
    uint8_t* link = links[i];
    if (!link) continue;
    uint8_t* obj = *reinterpret_cast<uint8_t**>(link);
    if (!obj) continue;
    uintptr_t cls = scene_class(obj);
    if (cls && action) {
      finish_block(obj, cls);
      continue;
    }
    if (cls && g_scene_told++ < 60)
      LOGI("net: player %d's use is over: the scene its chain goes on with (+%llX) plays here too", slot, static_cast<unsigned long long>(cls));
    taint(obj, now, slot);
    if (!IsBadReadPtr(obj - 8, 8)) taint(obj - 8, now, slot);
    if (g_cfg.lab && g_scene_told++ < 200) {  // the lab: what the chain goes through (the classes of its blocks)
      uintptr_t v0 = *reinterpret_cast<uintptr_t*>(obj), v8 = IsBadReadPtr(obj - 8, 8) ? 0 : *reinterpret_cast<uintptr_t*>(obj - 8);
      LOGI("lab: player %d's use goes through a block: obj %p vtbl +%llX / (-8) +%llX, fn +%llX", slot, static_cast<void*>(obj),
           static_cast<unsigned long long>(v0 - game::at(0)), static_cast<unsigned long long>(v8 - game::at(0)),
           static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(*reinterpret_cast<void**>(link + 8)) - game::at(0)));
    }
    reinterpret_cast<void (*)(uint8_t*)>(*reinterpret_cast<void**>(link + 8))(obj);
  }
  --g_remote_depth;
  g_remote_slot = was_slot;
}

void on_signal(uint8_t* output) {
  uint64_t now = GetTickCount64();
  int me = game::ref<int>(game::kNetLocalPlayer);
  int slot = g_replaying_use ? g_replay_slot : g_own_chain ? me : tainted(output, now);
  uint8_t* zone = g_zone_firing;
  bool in_zone = g_zone_scope != kScopeNone && zone && output >= zone && output < zone + 0x100;
  if (slot >= kZoneTagBase && !in_zone) {  // a zone chain another player's step started: the player blocks are kept
    size_t t = static_cast<size_t>(slot - kZoneTagBase);
    uint16_t n = *reinterpret_cast<uint16_t*>(output + 26);
    uint8_t** links = *reinterpret_cast<uint8_t***>(output + 16);
    for (unsigned i = 0; i < n && links; ++i) {
      uint8_t* link = links[i];
      if (!link) continue;
      uint8_t* obj = *reinterpret_cast<uint8_t**>(link);
      if (!obj) continue;
      void* fn = *reinterpret_cast<void**>(link + 8);
      uintptr_t cls = block_class(obj);
      if (cls && t < g_zone_tags.size()) {
        auto& kept = g_zone_tags[t].kept;
        if (std::find(kept.begin(), kept.end(), std::make_pair(obj, fn)) == kept.end() && kept.size() < 64) kept.push_back({obj, fn});
        if (g_zone_split_told++ < 40)
          LOGI("net: zone %p: a player block (+%llX) further down its chain is the other player's: kept for this PC's own step",
               g_zone_tags[t].zone, static_cast<unsigned long long>(cls));
        continue;
      }
      taint(obj, now, slot);
      if (!IsBadReadPtr(obj - 8, 8)) taint(obj - 8, now, slot);
      reinterpret_cast<void (*)(uint8_t*)>(fn)(obj);
    }
    return;
  }
  if (slot >= 0 && slot < kZoneTagBase && !in_zone && (slot != me || g_cfg.net_drive)) return fire_chain(output, slot);
  if (!in_zone) return g_signal(output);
  uint16_t n = *reinterpret_cast<uint16_t*>(output + 26);
  uint8_t** links = *reinterpret_cast<uint8_t***>(output + 16);
  for (unsigned i = 0; i < n && links; ++i) {
    uint8_t* link = links[i];
    if (!link) continue;
    uint8_t* obj = *reinterpret_cast<uint8_t**>(link);
    uintptr_t cls = obj ? block_class(obj) : 0;
    bool personal = cls != 0;
    if (personal != (g_zone_scope == kScopeOwn)) {
      if (personal && g_zone_split_told++ < 20)
        LOGI("net: zone %p: its player block (+%llX) is left to each player's own steps", zone, static_cast<unsigned long long>(cls));
      continue;
    }
    if (personal && g_zone_split_told++ < 20)
      LOGI("net: zone %p: its player block (+%llX) fires for this PC's own step", zone, static_cast<unsigned long long>(cls));
    if (!personal && g_zone_chain >= 0 && obj) {  // (the story's chain of another player's step: marked)
      taint(obj, now, g_zone_chain);
      if (!IsBadReadPtr(obj - 8, 8)) taint(obj - 8, now, g_zone_chain);
    }
    reinterpret_cast<void (*)(uint8_t*)>(*reinterpret_cast<void**>(link + 8))(obj);
  }
}

// --- "the player died" in a session ------------------------------------------------------------------------------
// A level's script ends the game on "player die" (triggers/player/player die: its callback exe+425F2C) with a death
// camera and "show death menu" (exe+6C9820). In a network game that trigger answers to any player's death: when the
// friend died, the host's game played its death camera and opened its death menu too, and loading the checkpoint
// from it reloaded everybody (0.4.1, the first real game). In a session the trigger and the menu go on only on the
// host, and only when the host's own player is dead; a joined player's dead body is replaced by the host anyway.
using DieFn = void (*)(uint8_t*, uint8_t*);
DieFn g_player_die = nullptr;
using ShowDeathFn = int64_t (*)(uint8_t*);
ShowDeathFn g_show_death = nullptr;

bool death_is_this_pcs() {
  if (game::ref<uintptr_t>(game::kNetHost) <= 1 || !game::ref<uint8_t>(game::kNetGame)) return true;  // not a session
  if (game::ref<int>(game::kNetLocalPlayer) != 0) return false;
  uint8_t* own = controlled();
  return !own || !(*reinterpret_cast<float*>(own + game::kEntityHealth) > 0.f);
}

void on_player_die(uint8_t* callbacks, uint8_t* info) {
  if (!death_is_this_pcs()) {
    LOGI("net: the level's \"player died\" for another player's death: left out on this PC");
    return;
  }
  g_player_die(callbacks, info);
}

int64_t on_show_death(uint8_t* block) {
  if (!death_is_this_pcs()) {
    LOGI("net: the level's death menu for another player's death: not shown on this PC");
    return 0;
  }
  return g_show_death(block);
}

using AttachToFn = int64_t (*)(void**, void**, void**, const float*);
AttachToFn g_attach_to = nullptr;

int64_t on_attach_to(void** child, void** parent, void** bone, const float* offset) {
  if ((g_replaying_use || g_remote_depth > 0) && drive::acting(g_replaying_use ? g_replay_slot : g_remote_slot) && child && *child &&
      *reinterpret_cast<uint8_t**>(*child)) {
    uint8_t* e = *reinterpret_cast<uint8_t**>(*child) - game::kEntityHandle;
    if (is_player(*reinterpret_cast<uint16_t*>(e + game::kEntityId))) {
      LOGI("net: another player's use would hang a player's body on a thing: left out");
      return 0;
    }
  }
  return g_attach_to(child, parent, bone, offset);
}

int64_t on_zone_event(ZoneEventFn original, uint8_t* callbacks, void** entity, bool enters) {
  bool was_empty = false;
  int inside = zone_players(callbacks, entity, enters, &was_empty);
  if (inside == -2) return 0;
  if (inside < 0) return original(callbacks, entity);
  uint8_t* own = controlled();
  void* own_handle = own ? *reinterpret_cast<void**>(own + game::kEntityHandle) : nullptr;
  if (!own_handle) return 0;
  bool own_step = *reinterpret_cast<uint8_t**>(*entity) == own + game::kEntityHandle;
  // the first one in / the last one out counts for the story; the others change nothing there
  bool shared = enters ? (was_empty && inside == 1) : (!was_empty && inside == 0);
  int64_t result = 0;
  uint8_t* zone = callbacks - kZoneCallbacks;
  bool split = g_signal != nullptr;
  if (shared) {
    g_zone_firing = zone, g_zone_scope = split ? kScopeShared : kScopeNone;
    g_zone_chain = split && !own_step && g_cfg.net_zone_own ? zone_tag(zone, enters) : -1;
    result = original(callbacks, &own_handle);
    g_zone_chain = -1;
    g_zone_firing = nullptr, g_zone_scope = kScopeNone;
  }
  if (own_step && split) {
    g_zone_firing = zone, g_zone_scope = kScopeOwn;
    original(callbacks, &own_handle);
    g_zone_firing = nullptr, g_zone_scope = kScopeNone;
    // the player blocks another player's chain of this zone came to: now this PC's player is in (or out) too
    for (ZoneTag& t : g_zone_tags) {
      if (t.zone != zone || t.enters != enters) continue;
      for (auto& [obj, fn] : t.kept) {
        if (!block_class(obj)) continue;  // (gone with its level)
        if (g_zone_split_told++ < 40)
          LOGI("net: zone %p: a player block of its chain (+%llX) fires for this PC's own step", zone,
               static_cast<unsigned long long>(block_class(obj)));
        reinterpret_cast<void (*)(uint8_t*)>(fn)(obj);
      }
    }
  }
  return result;
}

int64_t on_zone_enter(uint8_t* callbacks, void** entity) { return on_zone_event(g_zone_enter, callbacks, entity, true); }
int64_t on_zone_leave(uint8_t* callbacks, void** entity) { return on_zone_event(g_zone_leave, callbacks, entity, false); }

// --- the host's player in a joined game's copy of the level -----------------------------------------------------
// A joined game gets the level as the host's save. The host's player is in it, written by the inventory class of
// a body its PC controls ("player": slots, then "_fast_slot" and "_fsm_update_mode", a byte each). In the joined
// game that body is led by another PC and has the plain inventory class, whose reader skips the slots and
// "_fast_slot" - and stops one byte short. Everything after it in the player's state (what it carries in its
// hands, its activities, its three activity records) is then read one byte off. While the host stands still that
// part is nearly all zeroes and the shift goes unnoticed; once the host has walked or a level starts with the
// player doing something, a count comes out as garbage and the joined game falls while loading the level (a
// jump through a table by a byte that is no type: l19 after three seconds of walking, l16 at once).
// The byte is skipped: the plain reader, for the level's own player body, in a game that has joined.
constexpr uintptr_t kHolderLoad = 0x273F10;     // (actor + 2896, reader**): the inventory, "_item_to_spawn", "_ammo_in_box_to_spawn"
constexpr uintptr_t kInventoryLoad = 0x271060;  // the plain inventory's reader (class +AD7920)
constexpr size_t kActorHolder = 2896;
constexpr size_t kReaderPlace = 64;             // reader + : int, how far it has read
using HolderLoadFn = int64_t (*)(uint8_t*, void**);
HolderLoadFn g_holder_load = nullptr;
using InventoryLoadFn = void (*)(uint8_t*, void**);
InventoryLoadFn g_inventory_load = nullptr;
uint8_t* g_loading_actor = nullptr;

int64_t on_holder_load(uint8_t* holder, void** reader) {
  uint8_t* before = g_loading_actor;
  g_loading_actor = holder - kActorHolder;
  int64_t r = g_holder_load(holder, reader);
  g_loading_actor = before;
  return r;
}

void on_inventory_load(uint8_t* inventory, void** reader) {
  g_inventory_load(inventory, reader);
  uint8_t* actor = g_loading_actor;
  if (!actor || !reader || !*reader || *reinterpret_cast<uintptr_t*>(actor) != game::at(game::kPlayerClass)) return;
  if (game::ref<uintptr_t>(game::kNetHost) <= 1 || game::ref<int>(game::kNetLocalPlayer) == 0) return;
  // Only a body that its own PC was controlling when the save was written has the byte: the level's own player
  // entity. A joined player's body is a spawned copy (flag 0x10000) and was a led body on the host: the plain
  // class wrote it, the plain class reads it. (An old save may hold one: re/lab-saves/2033_hunter_start does.)
  uint32_t flags = *reinterpret_cast<uint32_t*>(actor + game::kEntityFlags);
  if (flags & 0x10000) {
    LOGI("net: a player's body %X in this copy of the level is a spawned copy (flags %X): its inventory's state is read as it is",
         *reinterpret_cast<uint16_t*>(actor + game::kEntityId), flags);
    return;
  }
  *reinterpret_cast<int*>(*reinterpret_cast<uint8_t**>(reader) + kReaderPlace) += 1;
  LOGI("net: the host's player %X (flags %X) in this copy of the level: one more byte of its inventory's state is passed over",
       *reinterpret_cast<uint16_t*>(actor + game::kEntityId), flags);
}

// --- the lab: a player's saved state as it is written and as it is read (found: a joined game fell reading it) --
std::string hex_of(const uint8_t* p, size_t n) {
  std::string out;
  char one[4];
  for (size_t i = 0; i < n; ++i) {
    snprintf(one, sizeof(one), "%02X", p[i]);
    out += one;
    if ((i & 3) == 3) out += ' ';
  }
  return out;
}

// (the three "activity" records inside it: where each begins, counted from the start of the player's state)
bool g_trace_player = false;
int g_trace_read_from = 0;
uint8_t* g_trace_written_from = nullptr;

using RecordLoadFn = int64_t (*)(uint8_t*, void**, void*, uint16_t);
RecordLoadFn g_record_load = nullptr;

int64_t on_record_load_traced(uint8_t* record, void** stream, void* owner, uint16_t version) {
  uint8_t* st = stream ? *reinterpret_cast<uint8_t**>(stream) : nullptr;
  if (g_trace_player && st) {
    const uint8_t* data = *reinterpret_cast<uint8_t**>(st + 56);
    int pos = *reinterpret_cast<int*>(st + 64);
    LOGD("net: [trace]   an activity record is read from +%d: %s", pos - g_trace_read_from,
         data && !IsBadReadPtr(data + pos, 64) ? hex_of(data + pos, 64).c_str() : "?");
  }
  return g_record_load(record, stream, owner, version);
}

using RecordSaveFn = int64_t* (*)(uint8_t*, void**, void*);
RecordSaveFn g_record_save = nullptr;

int64_t* on_record_save_traced(uint8_t* record, void** stream, void* owner) {
  uint8_t* st = stream ? *reinterpret_cast<uint8_t**>(stream) : nullptr;
  uint8_t* buffer = st ? *reinterpret_cast<uint8_t**>(st + 144) : nullptr;
  uint8_t* before = buffer && reinterpret_cast<intptr_t>(buffer) != -24 ? *reinterpret_cast<uint8_t**>(buffer + 8) : nullptr;
  int64_t* r = g_record_save(record, stream, owner);
  if (g_trace_player && before) {
    uint8_t* after = *reinterpret_cast<uint8_t**>(buffer + 8);
    size_t n = static_cast<size_t>(after - before);
    LOGD("net: [trace]   an activity record is written at +%lld, %zu bytes (id %u, parts %u): %s",
         static_cast<long long>(before - g_trace_written_from), n, record[0], *reinterpret_cast<uint16_t*>(record + 26),
         n < 4096 && !IsBadReadPtr(before, n) ? hex_of(before, n < 96 ? n : 96).c_str() : "?");
  }
  return r;
}

using ActorLoadFn = char (*)(uint8_t*, void**, uint16_t);
ActorLoadFn g_actor_load = nullptr;

char on_actor_load_traced(uint8_t* actor, void** stream, uint16_t version) {
  bool player = *reinterpret_cast<uintptr_t*>(actor) == game::at(game::kPlayerClass);
  uint8_t* st = stream ? *reinterpret_cast<uint8_t**>(stream) : nullptr;
  if (player && st) {
    const uint8_t* data = *reinterpret_cast<uint8_t**>(st + 56);
    int pos = *reinterpret_cast<int*>(st + 64);
    LOGD("net: [trace] reading the state of player %X (version %u, reader %p, place %d):", *reinterpret_cast<uint16_t*>(actor + game::kEntityId),
         version, st, pos);
    if (data && !IsBadReadPtr(data + pos, 256))
      for (int row = 0; row < 256; row += 64) LOGD("net: [trace]   %s", hex_of(data + pos + row, 64).c_str());
  }
  g_trace_player = player && st;
  g_trace_read_from = st ? *reinterpret_cast<int*>(st + 64) : 0;
  char r = g_actor_load(actor, stream, version);
  g_trace_player = false;
  if (player && st) LOGD("net: [trace]   read up to place %d", *reinterpret_cast<int*>(*reinterpret_cast<uint8_t**>(stream) + 64));
  return r;
}

using ActorSaveFn = int64_t (*)(uint8_t*, void**);
ActorSaveFn g_actor_save = nullptr;

int64_t on_actor_save_traced(uint8_t* actor, void** stream) {
  bool player = *reinterpret_cast<uintptr_t*>(actor) == game::at(game::kPlayerClass);
  uint8_t* st = stream ? *reinterpret_cast<uint8_t**>(stream) : nullptr;
  uint8_t* buffer = st ? *reinterpret_cast<uint8_t**>(st + 144) : nullptr;  // {begin**, cursor, capacity}
  uint8_t* before = buffer && reinterpret_cast<intptr_t>(buffer) != -24 ? *reinterpret_cast<uint8_t**>(buffer + 8) : nullptr;
  g_trace_player = player && before;
  g_trace_written_from = before;
  int64_t r = g_actor_save(actor, stream);
  g_trace_player = false;
  if (player && before) {
    uint8_t* after = *reinterpret_cast<uint8_t**>(buffer + 8);
    size_t n = static_cast<size_t>(after - before);
    LOGD("net: [trace] the state of player %X written: %zu bytes:", *reinterpret_cast<uint16_t*>(actor + game::kEntityId), n);
    if (n < 0x10000 && !IsBadReadPtr(before, n))
      for (size_t row = 0; row < n && row < 256; row += 64) LOGD("net: [trace]   %s", hex_of(before + row, n - row < 64 ? n - row : 64).c_str());
  }
  return r;
}

// --- "use" ------------------------------------------------------------------------------------------------------
// A door, a lever, a talk that waits for the player's "use" is a script block "triggers/entity/use" on the
// thing: when the thing is used the block fires (exe+423508), and the script goes on - on the PC of the player
// who pressed the key. The other PCs' scripts never hear of it: the door a joined player opened stays shut for
// the host. Every PC makes the level's blocks in the same order (the level's data, or the host's save of it),
// so each keeps the list of use blocks as they are made (exe+46E9BC, their constructor), and a use that happens
// here is told to the others as "use block number n", which they fire on their own copy.
// (Switching a block on - exe+423A84 - is no way to list them: a level comes with its blocks already on.)
constexpr uintptr_t kUseBlockMade = 0x46E9BC;  // block* (block*): the constructor of "triggers/entity/use"
constexpr uintptr_t kUseFired = 0x423508;      // void (block*, handle** user): the thing was used by that entity
                                               // (the block's own check, vtbl + 0x198, reads the user: exe+4237BC)
constexpr uintptr_t kUseBlockClass = 0xB75A70;
constexpr uintptr_t kTargetCount = 0x705F30;   // unsigned (list*, 0)
constexpr uintptr_t kTargetAt = 0x706030;      // (handle** out, list*, unsigned i, 0, 0): a counted reference
constexpr uintptr_t kHandleDrop = 0x7F00;      // (handle**): gives the reference back
constexpr size_t kBlockTargets = 144, kBlockOn = 96;
using UseBlockMadeFn = uint8_t* (*)(uint8_t*);
using UseFiredFn = void (*)(uint8_t*, void**);
UseBlockMadeFn g_use_block_made = nullptr;
UseFiredFn g_use_fired = nullptr;

std::vector<uint8_t*> g_use_blocks;  // in the order they were made, for the level that is loaded
uint64_t g_use_block_at = 0;
bool g_use_replayed = false;

#pragma pack(push, 1)
struct UseNote {
  uint16_t nth, count;  // which block, and how many this PC has (a check that the lists agree)
  uint8_t from;         // the player slot whose player used it (that PC does not fire it again)
  uint16_t user;        // the entity that used it (a player's body: every PC has it under this id)
};
#pragma pack(pop)

uint8_t* on_use_block_made(uint8_t* block) {
  uint8_t* made = g_use_block_made(block);
  // a level's blocks are made in one go while it loads: the first one after a pause begins another level's list
  uint64_t now = GetTickCount64();
  if (now - g_use_block_at > 3000) g_use_blocks.clear();
  g_use_block_at = now;
  if (made && g_use_blocks.size() < 0xFFFF) {
    // a block made where an old one was (its memory given again): the old entry goes
    for (size_t i = 0; i < g_use_blocks.size(); ++i)
      if (g_use_blocks[i] == made) {
        g_use_blocks.erase(g_use_blocks.begin() + static_cast<std::ptrdiff_t>(i));
        break;
      }
    g_use_blocks.push_back(made);
  }
  return made;
}

#pragma pack(push, 1)
struct UseKeyNote {
  uint8_t from;          // the player slot whose player used it
  uint16_t user;         // the entity that used it
  uint8_t n;             // how many target ids follow (0: the block waits on no entity - nth / count decide)
  uint16_t target[4];    // the entities the block waits on, smallest id first
  uint16_t nth, count;   // the block's place in the sender's list (only when n == 0, and only if the lists agree)
  uint8_t from_end;      // which of the blocks that wait on the same entities, counted from the one made last
                         // (a cabinet has two: one opens it, one takes its things; a joined game may hold the
                         // blocks of the level and, made later, those of the host's save - the last ones live)
};

bool use_block_alive(const uint8_t* block);
int read_targets(uint8_t* block, uint16_t* ids, int max);

// The alive blocks that wait on exactly these entities, in the order they were made.
std::vector<uint8_t*> blocks_waiting_on(const uint16_t* ids, int n) {
  std::vector<uint8_t*> out;
  for (uint8_t* block : g_use_blocks) {
    if (!use_block_alive(block)) continue;
    uint16_t got[4];
    if (read_targets(block, got, 4) == n && !memcmp(got, ids, n * sizeof(uint16_t))) out.push_back(block);
  }
  return out;
}
#pragma pack(pop)

// The ids of the entities a use block waits on, smallest first; -1 when the block cannot be read (its memory
// went back to the game: a block of a level that was taken down).
int read_targets(uint8_t* block, uint16_t* ids, int max) {
  __try {
    unsigned n = reinterpret_cast<unsigned (*)(uint8_t*, unsigned)>(game::at(kTargetCount))(block + kBlockTargets, 0);
    int got = 0;
    for (unsigned i = 0; i < n && i < 16; ++i) {
      void* handle = nullptr;
      reinterpret_cast<void* (*)(void**, uint8_t*, unsigned, unsigned, unsigned)>(game::at(kTargetAt))(&handle, block + kBlockTargets, i, 0, 0);
      uint8_t* target = handle ? *reinterpret_cast<uint8_t**>(handle) : nullptr;
      if (target && got < max) ids[got++] = *reinterpret_cast<uint16_t*>(target - game::kEntityHandle + game::kEntityId);
      if (handle) reinterpret_cast<void (*)(void**)>(game::at(kHandleDrop))(&handle);
    }
    for (int i = 1; i < got; ++i)  // smallest first
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

bool use_block_alive(const uint8_t* block) {
  return !IsBadReadPtr(block, 0x170) && *reinterpret_cast<const uintptr_t*>(block) == game::at(kUseBlockClass);
}

void on_use_fired(uint8_t* block, void** user) {
  if (g_use_replayed || !block || game::ref<uintptr_t>(game::kNetHost) <= 1 || game::ref<int>(game::kNetLocalPlayer) < 0 || link::peers() < 1)
    return g_use_fired(block, user);
  uint8_t* target = user && *user ? *reinterpret_cast<uint8_t**>(*user) : nullptr;
  uint16_t user_id = target ? *reinterpret_cast<uint16_t*>(target - game::kEntityHandle + game::kEntityId) : static_cast<uint16_t>(0xFFFF);
  // only what this PC's own player did is told: a use by a body another PC leads is that PC's to tell
  uint8_t* own = controlled();
  if (!own || user_id != *reinterpret_cast<uint16_t*>(own + game::kEntityId)) return g_use_fired(block, user);
  UseKeyNote note = {};
  note.from = static_cast<uint8_t>(game::ref<int>(game::kNetLocalPlayer));
  note.user = user_id;
  int n = read_targets(block, note.target, 4);
  note.n = static_cast<uint8_t>(n > 0 ? n : 0);
  int by = -1;
  if (!drive::may_use(note.target, note.n, &by)) return;  // another player is at it now: only one turns a wheel
  drive::used(note.target, note.n);
  g_own_chain = g_cfg.net_drive;
  g_use_fired(block, user);
  g_own_chain = false;
  for (int i = 0; i < n; ++i) motions::own_use(note.target[i]);  // (what this PC's player turns is not turned back by another PC's word)
  note.nth = 0xFFFF;
  note.count = static_cast<uint16_t>(g_use_blocks.size());
  for (size_t i = 0; i < g_use_blocks.size(); ++i)
    if (g_use_blocks[i] == block) note.nth = static_cast<uint16_t>(i);
  note.from_end = 0xFF;
  if (note.n) {
    std::vector<uint8_t*> same = blocks_waiting_on(note.target, note.n);
    for (size_t i = 0; i < same.size(); ++i)
      if (same[i] == block) note.from_end = static_cast<uint8_t>(same.size() - 1 - i);
  }
  bool sent = link::send(link::kUseKey, &note, sizeof(note), true);
  static int told = 0;
  if (told++ < 40)
    LOGI("net: a thing was used here (use block %d of %zu, waits on %d: %X %X %X %X): %s", note.nth == 0xFFFF ? -1 : note.nth,
         g_use_blocks.size(), note.n, note.target[0], note.target[1], note.target[2], note.target[3],
         sent ? "the others are told" : "nobody to tell");
}

// A use told by another PC, named by what its block waits on: the block here that waits on the same entities
// fires (one that is on before one that is off; of equals, the one made last).
void on_use_key(int, const uint8_t* data, size_t size) {
  UseKeyNote note;
  if (size < sizeof(note)) return;
  memcpy(&note, data, sizeof(note));
  if (g_cfg.net_use_on_joined && game::ref<int>(game::kNetLocalPlayer) == 0 && link::peers() > 1)
    link::send(link::kUseKey, data, size, true);  // to the other joined games
  if (note.from == static_cast<uint8_t>(game::ref<int>(game::kNetLocalPlayer))) return;  // this PC's own use, come back round
  if (note.n <= 4) drive::remote_used(note.from, note.target, note.n);  // (that player's for a while: nobody else uses it)
  // (0.4.2+: a joined game does not play anybody's use. The host's use of a wheel ran its scene on the joined PC too
  // and opened the door there at once, while on the host the player still turned the wheel. What the use moves
  // - the wheel, the door, a cabinet - comes to the joined games from the host's motions, when it moves there.)
  if (!g_cfg.net_use_on_joined && game::ref<int>(game::kNetLocalPlayer) != 0) {
    LOGI("net: player %d used a thing: left to the host's motions here", static_cast<int>(note.from));
    return;
  }
  if (!g_cfg.net_use_on_host && game::ref<int>(game::kNetLocalPlayer) == 0) {  // (what it moves comes by its motions)
    LOGI("net: player %d used a thing: what it moves comes by its motions", static_cast<int>(note.from));
    return;
  }
  uint8_t* chosen = nullptr;
  bool chosen_on = false;
  int candidates = 0;
  if (note.n > 0 && note.n <= 4) {
    std::vector<uint8_t*> same = blocks_waiting_on(note.target, note.n);
    candidates = static_cast<int>(same.size());
    if (note.from_end < same.size()) {
      chosen = same[same.size() - 1 - note.from_end];
      chosen_on = chosen[kBlockOn] == 1;
    } else {
      for (uint8_t* block : same) {  // the last one that is on, else the last one
        bool on = block[kBlockOn] == 1;
        if (!chosen || on || !chosen_on) {
          chosen = block;
          chosen_on = on;
        }
      }
    }
  } else if (note.nth < g_use_blocks.size() && note.count == g_use_blocks.size() && use_block_alive(g_use_blocks[note.nth])) {
    chosen = g_use_blocks[note.nth];
    candidates = 1;
  }
  if (!chosen) {
    LOGW("net: player %d used a thing (waits on %u: %X %X %X %X), but this PC has no such use block (%zu blocks here): not fired here",
         static_cast<int>(note.from), note.n, note.target[0], note.target[1], note.target[2], note.target[3], g_use_blocks.size());
    return;
  }
  uint8_t* user = entity_by_id(note.user);
  if (!user || !is_player(note.user)) user = controlled();
  void* handle = user ? *reinterpret_cast<void**>(user + game::kEntityHandle) : nullptr;
  if (!handle) {
    LOGW("net: player %d used a thing, but there is no body here to use it with", static_cast<int>(note.from));
    return;
  }
  g_use_replayed = true;
  g_replaying_use = g_cfg.net_use_scene_own;
  g_replay_slot = note.from;
  if (g_replaying_use) taint(chosen, GetTickCount64(), note.from);
  g_use_fired(chosen, &handle);
  g_replaying_use = false;
  g_use_replayed = false;
  LOGI("net: player %d used a thing (waits on %u: %X %X %X %X; %d such block%s here, the one fired is %s): fired here too",
       static_cast<int>(note.from), note.n, note.target[0], note.target[1], note.target[2], note.target[3], candidates,
       candidates == 1 ? "" : "s", chosen_on ? "on" : "off");
}

void on_use(int, const uint8_t* data, size_t size) {
  UseNote note;
  if (size < sizeof(note)) return;
  memcpy(&note, data, sizeof(note));
  if (g_cfg.net_use_on_joined && game::ref<int>(game::kNetLocalPlayer) == 0 && link::peers() > 1)
    link::send(link::kUse, data, size, true);  // to the other joined games
  if (note.from == static_cast<uint8_t>(game::ref<int>(game::kNetLocalPlayer))) return;  // this PC's own use, come back round
  // (0.4.2+: a joined game does not play anybody's use. The host's use of a wheel ran its scene on the joined PC too
  // and opened the door there at once, while on the host the player still turned the wheel. What the use moves
  // - the wheel, the door, a cabinet - comes to the joined games from the host's motions, when it moves there.)
  if (!g_cfg.net_use_on_joined && game::ref<int>(game::kNetLocalPlayer) != 0) {
    LOGI("net: player %d used a thing: left to the host's motions here", static_cast<int>(note.from));
    return;
  }
  if (!g_cfg.net_use_on_host && game::ref<int>(game::kNetLocalPlayer) == 0) {  // (what it moves comes by its motions)
    LOGI("net: player %d used a thing: what it moves comes by its motions", static_cast<int>(note.from));
    return;
  }
  if (note.count != g_use_blocks.size() || note.nth >= g_use_blocks.size() || !use_block_alive(g_use_blocks[note.nth])) {
    LOGW("net: player %d used a thing (use block %u of %u), but this PC's list has %zu: not fired here", static_cast<int>(note.from), note.nth,
         note.count, g_use_blocks.size());
    return;
  }
  // the user here: this PC's copy of the body that used it (the block's check looks at who uses)
  uint8_t* user = entity_by_id(note.user);
  if (!user || !is_player(note.user)) user = controlled();
  void* handle = user ? *reinterpret_cast<void**>(user + game::kEntityHandle) : nullptr;
  if (!handle) {
    LOGW("net: player %d used a thing (use block %u), but there is no body here to use it with", static_cast<int>(note.from), note.nth);
    return;
  }
  g_use_replayed = true;
  g_replaying_use = g_cfg.net_use_scene_own;
  g_replay_slot = note.from;
  drive::remote_used(note.from, nullptr, 0);
  if (g_replaying_use) taint(g_use_blocks[note.nth], GetTickCount64(), note.from);
  g_use_fired(g_use_blocks[note.nth], &handle);
  g_replaying_use = false;
  g_use_replayed = false;
  LOGI("net: player %d used a thing (use block %u of %u, as body %X): fired here too", static_cast<int>(note.from), note.nth, note.count,
       *reinterpret_cast<uint16_t*>(user + game::kEntityId));
}

// The things a use block waits on (their ids), for the lab's list.
std::string use_block_things(uint8_t* block) {
  std::string out;
  unsigned n = reinterpret_cast<unsigned (*)(uint8_t*, unsigned)>(game::at(kTargetCount))(block + kBlockTargets, 0);
  for (unsigned i = 0; i < n && i < 8; ++i) {
    void* handle = nullptr;
    reinterpret_cast<void* (*)(void**, uint8_t*, unsigned, unsigned, unsigned)>(game::at(kTargetAt))(&handle, block + kBlockTargets, i, 0, 0);
    uint8_t* target = handle ? *reinterpret_cast<uint8_t**>(handle) : nullptr;
    char one[96];
    if (target) {
      uint8_t* e = target - game::kEntityHandle;
      const uint8_t* name = *reinterpret_cast<uint8_t**>(e + game::kEntityName);
      snprintf(one, sizeof(one), " %X '%s'", *reinterpret_cast<uint16_t*>(e + game::kEntityId), name ? reinterpret_cast<const char*>(name) + 0x14 : "");
    } else {
      snprintf(one, sizeof(one), " -");
    }
    out += one;
    if (handle) reinterpret_cast<void (*)(void**)>(game::at(kHandleDrop))(&handle);
  }
  return out;
}

}  // namespace

// drive.cpp: whose use's chain a block is in (-1: none), a block given to that chain, an output fired as it.
int chain_owner(const uint8_t* block) {
  int slot = tainted(block, GetTickCount64());
  return slot >= kZoneTagBase ? -1 : slot;
}
void tag_chain(uint8_t* block, int slot) { taint(block, GetTickCount64(), slot); }
void fire_as(uint8_t* output, int slot) { fire_chain(output, slot); }

// The lab: "!uses" lists the use blocks of the level; "!use <n>" fires one as if this PC's player had used its thing.
void lab_uses() {
  unsigned on = 0;
  for (size_t i = 0; i < g_use_blocks.size(); ++i) {
    uint8_t* block = g_use_blocks[i];
    if (!use_block_alive(block)) {
      LOGI("net:   use block %zu: gone", i);
      continue;
    }
    on += block[kBlockOn] == 1;
    LOGI("net:   use block %zu (%s):%s", i, block[kBlockOn] == 1 ? "on" : "off", use_block_things(block).c_str());
  }
  LOGI("net: %zu use blocks, %u of them on", g_use_blocks.size(), on);
}

void lab_use_on(unsigned target) {
  uint16_t id = static_cast<uint16_t>(target);
  std::vector<uint8_t*> same = blocks_waiting_on(&id, 1);
  for (uint8_t* b : same) b[kBlockOn] = 1;
  LOGI("net: the lab turns on the %zu use blocks waiting on %X", same.size(), id);
}

void lab_use_id(unsigned target, unsigned k) {
  uint16_t id = static_cast<uint16_t>(target);
  std::vector<uint8_t*> same = blocks_waiting_on(&id, 1);
  if (k >= same.size()) {
    LOGI("net: %X has %zu use blocks, no %u-th from the end", id, same.size(), k);
    return;
  }
  uint8_t* block = same[same.size() - 1 - k];
  uint8_t* own = controlled();
  void* handle = own ? *reinterpret_cast<void**>(own + game::kEntityHandle) : nullptr;
  if (!handle) return;
  LOGI("net: the lab uses %X: block %u from the end of %zu (%s)", id, k, same.size(), block[kBlockOn] == 1 ? "on" : "off");
  on_use_fired(block, &handle);
}

void lab_use_named(const std::string& name, unsigned k) {
  uint8_t* server = game::ref<uint8_t*>(game::kServer);
  if (!server) return;
  uint8_t* table = *reinterpret_cast<uint8_t**>(server + game::kServerEntities);
  unsigned count = *reinterpret_cast<uint16_t*>(server + game::kServerEntityCount);
  uint16_t id = 0xFFFF;
  for (unsigned i = 0; i < count && id == 0xFFFF; ++i) {
    uint8_t* e = *reinterpret_cast<uint8_t**>(table + i * 16 + 8);
    const uint8_t* str = e ? *reinterpret_cast<uint8_t**>(e + game::kEntityName) : nullptr;
    if (str && name == reinterpret_cast<const char*>(str) + 0x14) id = *reinterpret_cast<uint16_t*>(e + game::kEntityId);
  }
  if (id == 0xFFFF) {
    LOGI("net: no entity named '%s'", name.c_str());
    return;
  }
  std::vector<uint8_t*> same = blocks_waiting_on(&id, 1);
  if (k >= same.size()) {
    LOGI("net: '%s' (%X) has %zu use blocks, no %u-th from the end", name.c_str(), id, same.size(), k);
    return;
  }
  uint8_t* block = same[same.size() - 1 - k];
  uint8_t* own = controlled();
  void* handle = own ? *reinterpret_cast<void**>(own + game::kEntityHandle) : nullptr;
  if (!handle) {
    LOGI("net: no body to use '%s' with", name.c_str());
    return;
  }
  LOGI("net: the lab uses '%s' (%X): block %u from the end of %zu (%s)", name.c_str(), id, k, same.size(), block[kBlockOn] == 1 ? "on" : "off");
  on_use_fired(block, &handle);
}

void lab_use(unsigned n) {
  if (n >= g_use_blocks.size() || !use_block_alive(g_use_blocks[n])) {
    LOGI("net: no use block %u", n);
    return;
  }
  uint8_t* own = controlled();
  void* handle = own ? *reinterpret_cast<void**>(own + game::kEntityHandle) : nullptr;
  if (!handle) {
    LOGI("net: no body to use block %u with", n);
    return;
  }
  LOGI("net: the lab fires use block %u (%s):%s", n, g_use_blocks[n][kBlockOn] == 1 ? "on" : "off", use_block_things(g_use_blocks[n]).c_str());
  on_use_fired(g_use_blocks[n], &handle);
}

namespace {
// --- a character's action that names an entity which is not here any more -------------------------------------
// In a network game a joined PC does not think for the level's characters: their brains run "as a client"
// (the game's own name for it: base_brain_unit::client_update_types) on short records from the host - what the
// character does now. A record is two dwords: the kind of action in the first (a letter in bits 13..20), and in
// the second a word at +6 that is, by the kind, the number of a motion / a sound / a named transition - or, when
// bit 0x200 of the second dword is set, the id of the entity the action is about (the monster a ranger fights).
// Every brain class starts its handler the same way:
//     if (exe+3CBCC0 (brain, record)) return;     // "it is about an entity": found by its id, taken, done
//     ... read the record by its kind ...          // (exe+3CB7C0 and the class's own cases)
// exe+3CBCC0 answers 0 both for "not about an entity" and for "about an entity that is not in this world" - and
// in the second case the handler goes on and reads the id as a number of a motion or of a line in a table.
// The host's game never comes here, and in a joined game it takes the host removing an entity while one of the
// level's characters is busy with it: l36_ostankino, where the script takes a wave of monsters out from under
// the rangers and Miller. The joined game fell there about one run in four, in the first update after the
// monsters were destroyed (metro.exe+3CBA5F - a line of a table by the id; +BD46E, +7AE10D, +C4896, +30D7DB,
// +86B70 - the animation playing "motion 9854 of 3606", 9854 being the id of a monster gone a moment before).
// Here: a record about an entity that is gone is answered "done" - there is nobody to do it with.
constexpr uintptr_t kClientAboutEntity = 0x3CBCC0;
using ClientAboutEntityFn = int64_t (*)(uint8_t*, uint8_t*);
ClientAboutEntityFn g_client_about_entity = nullptr;

int64_t on_client_about_entity(uint8_t* brain, uint8_t* record) {
  int64_t taken = g_client_about_entity(brain, record);
  if (taken || !record || !(*reinterpret_cast<uint32_t*>(record + 4) & 0x200)) return taken;
  static std::atomic<int> said{0};
  if (said.fetch_add(1) < 5)
    LOGI("net: a character's action names entity %X, which is not in this world any more: the action is skipped",
         *reinterpret_cast<uint16_t*>(record + 6));
  return 1;
}

// (lab) The host's "these entities are gone" event (exe+68C820: x, event* - a count at +4, ids from +5). It only
// puts the entities into the removal queue (exe+689050, the queue is at server + 40, how many wait - a word at
// server + 50); exe+68A280 (server*) destroys what waits there - once a turn of the main loop (exe+647D30), right
// before the network step, so an entity the host names in one turn is destroyed at the start of the next, and
// the first update of the characters after that follows at once. (exe+684A30, which every world-event handler
// calls first, is another thing: the waiting changes of who is whose child.)
constexpr uintptr_t kGoneEvent = 0x68C820;
using GoneEventFn = int64_t (*)(void*, uint8_t*);
GoneEventFn g_gone_event = nullptr;

int64_t on_gone_event(void* x, uint8_t* event) {
  if (event && game::ref<uintptr_t>(game::kNetHost) > 1 && game::ref<int>(game::kNetLocalPlayer) > 0) {
    g_last_gone_event = GetTickCount64();
    g_flush_lines = 0;
    LOGD("net: [trace] the host's word: %u entities are gone (thread %lu, the frame's thread %lu)", event[4] & 0x7F,
         GetCurrentThreadId(), g_main_thread.load());
  }
  loot::filter_gone(event);
  return g_gone_event(x, event);
}

constexpr uintptr_t kDestroyQueued = 0x68A280;
using DestroyQueuedFn = uint64_t (*)(uint8_t*);
DestroyQueuedFn g_destroy_queued = nullptr;

uint64_t on_destroy_queued(uint8_t* server) {
  unsigned waiting = server ? *reinterpret_cast<uint16_t*>(server + 50) : 0;
  if (waiting && game::ref<uintptr_t>(game::kNetHost) > 1 && game::ref<int>(game::kNetLocalPlayer) > 0 &&
      GetTickCount64() - g_last_gone_event.load() < 3000 && g_flush_lines.fetch_add(1) < 12)
    LOGD("net: [trace] %u entities of the removal queue are destroyed (from +%llX, thread %lu, the frame's thread %lu)", waiting,
         static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(_ReturnAddress()) - game::at(0)), GetCurrentThreadId(),
         g_main_thread.load());
  return g_destroy_queued(server);
}

// (lab) The update of one entity (exe+6B1720: entity*, time step; the level's update gives the entities to the
// worker threads eight at a time, exe+6B1800): which entity this thread is in - for the crash log.
thread_local uint8_t* t_updating = nullptr;
using UpdateOneFn = int64_t (*)(uint8_t*, unsigned);
UpdateOneFn g_update_one = nullptr;

int64_t on_update_one(uint8_t* e, unsigned step) {
  uint8_t* outer = t_updating;
  t_updating = e;
  int64_t r = g_update_one(e, step);
  t_updating = outer;
  return r;
}

}  // namespace

void on_rails(int from, const uint8_t* data, size_t size);
void install_rails();
void on_seat(int from, const uint8_t* data, size_t size);
void on_hands(int from, const uint8_t* data, size_t size);
void on_actors(int from, const uint8_t* data, size_t size);
void on_way(int from, const uint8_t* data, size_t size);
void settle_actors();

// --- the game's collision tree: taking it for writing without stopping the readers ----------------------------
// The level's tree of volumes ("uSpatialDB::access") is guarded by a readers / writer lock (tree + 8: readers
// exe+145E20, writer exe+145ED0, release exe+145F70; the count of readers or -1 for a writer at lock + 0xE4, the
// writers that wait at + 0xE0). A writer that waits holds every NEW reader back - and that is a trap, because the
// game has readers that depend on each other:
//   - a character's ground query takes ITS OWN shape for writing (a spin lock at shape + 176) and then reads the
//     tree (exe+75B4A0 -> exe+75AAC0 -> exe+7285B0);
//   - a ray query reads the tree and, for every shape it meets, takes that shape for reading (exe+8F4920 ->
//     exe+14A11C -> ... -> exe+75AD10).
// Both read the tree at once and nothing is wrong - until a third thread asks for the tree for writing between
// them (a new entity put into the tree, exe+8F1DE0, or the tree's queue of 32 moves being carried out,
// exe+1461A0 / exe+1464F0). Then the first cannot start reading (a writer waits), the second cannot finish (it
// spins on the shape the first holds), the writer waits for the second - and with them every thread that needs
// the tree: the game stands still for good. The two carry-out functions make it worse by waiting for the tree
// while they hold the spin lock of the queue (tree + 0x210), so that everybody who moves stands too.
// Seen in the long fight of l36_ostankino, where every hit makes particles (new entities): a lab copy stood
// still in 5 long runs of 14 on four cores and in 1 of 14 on eight - with one copy and the network off just the
// same. In a real game the gap is microseconds wide, which is why it ships like that.
// Here the writer does not wait inside the lock: it takes the tree only when it is free, and until then stands
// aside - new readers pass, and the queue's spin lock is given back while it waits.
constexpr uintptr_t kTreeWriteLock = 0x145ED0;
using TreeWriteLockFn = int64_t (*)(uint8_t*);
TreeWriteLockFn g_tree_write_lock = nullptr;
std::atomic<unsigned> g_tree_waits{0};
std::atomic<unsigned> g_tree_longest_ms{0};

int64_t on_tree_write_lock(uint8_t* lock) {
  // mode = auto: a game nobody has joined is the game as it is, its own lock included.
  if (g_cfg.net_mode == Config::NetMode::Auto && !game::ref<uint8_t>(game::kNetGame)) return g_tree_write_lock(lock);
  uintptr_t from = reinterpret_cast<uintptr_t>(_ReturnAddress()) - game::at(0);
  bool queue_held = from == 0x1461F9 || from == 0x14654C;  // the two carry-out functions: tree + 0x210 is theirs
  volatile LONG* queue = reinterpret_cast<volatile LONG*>(lock + 0x208);
  int32_t* state = reinterpret_cast<int32_t*>(lock + 0xE4);
  CRITICAL_SECTION* guard = reinterpret_cast<CRITICAL_SECTION*>(lock);
  ULONGLONG since = 0;
  for (unsigned turn = 0;; ++turn) {
    EnterCriticalSection(guard);
    bool mine = *state == 0;
    if (mine) *state = -1;
    LeaveCriticalSection(guard);
    if (mine) break;
    if (!since) since = GetTickCount64();
    if (queue_held) InterlockedExchange(queue, 0);
    if (turn < 4000) {
      for (int i = 0; i < 32; ++i) YieldProcessor();
      if (!SwitchToThread()) Sleep(0);
    } else {
      Sleep(turn < 6000 ? 0 : 1);
    }
    if (queue_held)
      while (InterlockedCompareExchange(queue, -1, 0)) YieldProcessor();
  }
  if (since) {
    unsigned took = static_cast<unsigned>(GetTickCount64() - since);
    g_tree_waits.fetch_add(1);
    unsigned longest = g_tree_longest_ms.load();
    while (took > longest && !g_tree_longest_ms.compare_exchange_weak(longest, took)) {
    }
    if (took >= 500) LOGW("net: the collision tree was not free for writing for %u ms", took);
  }
  DWORD slot = *reinterpret_cast<DWORD*>(lock + 0x30);  // (the lock counts each thread's own holds there)
  TlsSetValue(slot, static_cast<char*>(TlsGetValue(slot)) + 1);
  return 0;
}

// How long a PC may be silent. The game's network library (ENet, an old one: its limits are numbers in the code
// of exe+8EDBA0, the check of unanswered packets) takes the other side for gone when a packet stays unanswered
// for 20 s, or for 5 s once it has been sent again five times - with the usual pauses between the tries that is
// 6 - 9 s. A game does not answer while it is inside the first phase of a level load ("maploader_resources":
// the main loop stands still, nobody turns the network): 0.6 s on most levels on a fast PC, but 7.5 - 9.3 s for
// l33_d6 on a lab copy - and the host dropped the joiner in the middle of its load, three joins of three, leaving
// it alone in the level without a body. A slow disk does the same to a real game. Both limits are moved out.
void widen_silence_limits(int seconds) {
  if (seconds <= 0) return;
  uint32_t least = static_cast<uint32_t>(seconds) * 1000, most = least + 15000;
  const uint8_t was_most[6] = {0x81, 0xF9, 0x20, 0x4E, 0x00, 0x00};   // cmp ecx, 20000
  const uint8_t was_least[6] = {0x81, 0xFA, 0x88, 0x13, 0x00, 0x00};  // cmp edx, 5000
  uint8_t now_most[6] = {0x81, 0xF9}, now_least[6] = {0x81, 0xFA};
  memcpy(now_most + 2, &most, 4);
  memcpy(now_least + 2, &least, 4);
  patch::replace("network: the longest silence", game::at(0x8EDC47), was_most, now_most, 6);
  patch::replace("network: silence after the retries", game::at(0x8EDC6C), was_least, now_least, 6);
}

void set_hands_mode(int mode);

void install(const Config& cfg) {
  g_cfg = cfg;
  set_hands_mode(cfg.net_hide_hands);
  engine::on_log(&on_video_line);
  if (cfg.net_mode != Config::NetMode::Off) widen_silence_limits(cfg.net_timeout);
  if (cfg.net_mode != Config::NetMode::Off && cfg.net_tree_lock)
    patch::hook("collision tree: write lock", game::at(kTreeWriteLock), reinterpret_cast<void*>(&on_tree_write_lock),
                reinterpret_cast<void**>(&g_tree_write_lock));
  patch::hook("script player", game::at(game::kScriptPlayer), reinterpret_cast<void*>(&on_script_player),
              reinterpret_cast<void**>(&g_script_player));
  patch::hook("holder load", game::at(kHolderLoad), reinterpret_cast<void*>(&on_holder_load), reinterpret_cast<void**>(&g_holder_load));
  patch::hook("inventory load", game::at(kInventoryLoad), reinterpret_cast<void*>(&on_inventory_load),
              reinterpret_cast<void**>(&g_inventory_load));
  if (cfg.net_zones) {
    patch::hook("zone: enter", game::at(0x4AEFB4), reinterpret_cast<void*>(&on_zone_enter), reinterpret_cast<void**>(&g_zone_enter));
    patch::hook("zone: leave", game::at(0x4AF530), reinterpret_cast<void*>(&on_zone_leave), reinterpret_cast<void**>(&g_zone_leave));
  }
  if ((cfg.net_zones && cfg.net_zone_own) || (cfg.net_use && cfg.net_use_scene_own))
    patch::hook("script signals: the player's own blocks", game::at(0x706270), reinterpret_cast<void*>(&on_signal), reinterpret_cast<void**>(&g_signal));
  if (cfg.net_mode != Config::NetMode::Off) {
    patch::hook("the level's player died", game::at(0x425F2C), reinterpret_cast<void*>(&on_player_die), reinterpret_cast<void**>(&g_player_die));
    patch::hook("the level's death menu", game::at(0x6C9820), reinterpret_cast<void*>(&on_show_death), reinterpret_cast<void**>(&g_show_death));
  }
  if (cfg.net_use && cfg.net_use_scene_own)
    patch::hook("script attach", game::at(game::kAttachTo), reinterpret_cast<void*>(&on_attach_to), reinterpret_cast<void**>(&g_attach_to));
  if (cfg.net_use) {
    patch::hook("use block made", game::at(kUseBlockMade), reinterpret_cast<void*>(&on_use_block_made), reinterpret_cast<void**>(&g_use_block_made));
    patch::hook("use: fired", game::at(kUseFired), reinterpret_cast<void*>(&on_use_fired), reinterpret_cast<void**>(&g_use_fired));
    link::on(link::kUse, &on_use);        // from a PC with an older mod
    link::on(link::kUseKey, &on_use_key);
  }
  if (cfg.net_missing_target)
    patch::hook("client action about an entity", game::at(kClientAboutEntity), reinterpret_cast<void*>(&on_client_about_entity),
                reinterpret_cast<void**>(&g_client_about_entity));
  if (cfg.lab) {
    patch::hook("entities gone (lab trace)", game::at(kGoneEvent), reinterpret_cast<void*>(&on_gone_event),
                reinterpret_cast<void**>(&g_gone_event));
    patch::hook("removal queue (lab trace)", game::at(kDestroyQueued), reinterpret_cast<void*>(&on_destroy_queued),
                reinterpret_cast<void**>(&g_destroy_queued));
    patch::hook("update one entity (lab)", game::at(0x6B1720), reinterpret_cast<void*>(&on_update_one),
                reinterpret_cast<void**>(&g_update_one));
  }
  link::on(link::kAskLoad, &on_ask_load);
  link::on(link::kRails, &on_rails);
  install_rails();
  link::on(link::kSeat, &on_seat);
  link::on(link::kHands, &on_hands);
  link::on(link::kActors, &on_actors);
  link::on(link::kWay, &on_way);
  patch::hook("hit event filter", game::at(game::kHitEventFilter), reinterpret_cast<void*>(&on_hit_filter),
              reinterpret_cast<void**>(&g_hit_filter));
  patch::hook("HUD model start", game::at(game::kHudModelStart), reinterpret_cast<void*>(&on_hud_model_start),
              reinterpret_cast<void**>(&g_hud_model_start));
  if (cfg.lab) {
    patch::hook("queue destroy (lab trace)", game::at(0x6975D0), reinterpret_cast<void*>(&on_queue_destroy_traced),
                reinterpret_cast<void**>(&g_queue_destroy_traced));
    patch::hook("character state load (lab trace)", game::at(0x276140), reinterpret_cast<void*>(&on_actor_load_traced),
                reinterpret_cast<void**>(&g_actor_load));
    patch::hook("character state save (lab trace)", game::at(0x276AC0), reinterpret_cast<void*>(&on_actor_save_traced),
                reinterpret_cast<void**>(&g_actor_save));
    patch::hook("activity record load (lab trace)", game::at(0x4B8980), reinterpret_cast<void*>(&on_record_load_traced),
                reinterpret_cast<void**>(&g_record_load));
    patch::hook("activity record save (lab trace)", game::at(0x4B8D70), reinterpret_cast<void*>(&on_record_save_traced),
                reinterpret_cast<void**>(&g_record_save));
  }
  patch::hook("destroy entity", game::at(game::kDestroyNow), reinterpret_cast<void*>(&on_destroy_now),
              reinterpret_cast<void**>(&g_destroy_now));
  patch::hook("send_props_pack", game::at(game::kSendPropsPack), reinterpret_cast<void*>(&on_send_props_pack),
              reinterpret_cast<void**>(&g_send_props_pack));
  patch::hook("player of entity", game::at(game::kNetPlayerOfEntity), reinterpret_cast<void*>(&on_player_of_entity),
              reinterpret_cast<void**>(&g_player_of_entity));
  patch::hook("watcher update", game::at(game::kWatcherUpdate), reinterpret_cast<void*>(&on_watcher_update),
              reinterpret_cast<void**>(&g_watcher_update));
  patch::hook("motion by number", game::at(0xA1E80), reinterpret_cast<void*>(&on_motion_lookup), reinterpret_cast<void**>(&g_motion_lookup));
  patch::hook("picked-up line", game::at(game::kHudPickup), reinterpret_cast<void*>(&on_hud_pickup), reinterpret_cast<void**>(&g_hud_pickup));
  patch::hook("value scaled by the player", game::at(game::kPlayerScaled), reinterpret_cast<void*>(&on_player_scaled),
              reinterpret_cast<void**>(&g_player_scaled));
  patch::hook("save request", game::at(game::kSaveRequest), reinterpret_cast<void*>(&on_save_request),
              reinterpret_cast<void**>(&g_save_request));
  patch::hook("save name", game::at(game::kSaveName), reinterpret_cast<void*>(&on_save_name),
              reinterpret_cast<void**>(&g_save_name));
  patch::hook("console line", game::at(game::kConsoleLine), reinterpret_cast<void*>(&on_console_line),
              reinterpret_cast<void**>(&g_console_line));
  patch::hook("save queued", game::at(game::kSaveQueued), reinterpret_cast<void*>(&on_save_queued),
              reinterpret_cast<void**>(&g_save_queued));
  static const uint8_t null1_was[6] = {0x75, 0x04, 0x33, 0xC0, 0xEB, 0x06};
  static const uint8_t null1_now[6] = {0x0F, 0x84, 0x88, 0x03, 0x00, 0x00};
  static const uint8_t null2_was[8] = {0x75, 0x06, 0x33, 0xDB, 0x8B, 0xC3, 0xEB, 0x08};
  static const uint8_t null2_now[8] = {0x0F, 0x84, 0x54, 0x03, 0x00, 0x00, 0x90, 0x90};
  patch::replace("move: released controller (1)", game::at(game::kMoveOwnerNull1), null1_was, null1_now, sizeof(null1_now));
  patch::replace("move: released controller (2)", game::at(game::kMoveOwnerNull2), null2_was, null2_now, sizeof(null2_now));
  static const uint8_t owner_call[6] = {0xFF, 0x90, 0x70, 0x0B, 0x00, 0x00};
  patch::call_site("save loop", game::at(game::kSaveLoopOwnerCall), owner_call, sizeof(owner_call),
                   reinterpret_cast<void*>(&on_save_owner));
  patch::hook("enet_host_create", game::at(game::kEnetHostCreate), reinterpret_cast<void*>(&on_enet_host_create),
              reinterpret_cast<void**>(&g_enet_host_create));
  // The game's old network mode knew two weapon slots; Redux added a third (slot 16). In a network game "take
  // the weapon of slot 16 into the hands" still answers "no" at once, and the key of the third weapon does
  // something else (it looks the held weapon over): whoever puts the third weapon away cannot get it back until
  // the session ends. Both checks are taken out (je -> jmp): the third slot works as in single player.
  static const uint8_t slot16_was[2] = {0x74, 0x0D};
  static const uint8_t slot16_now[2] = {0xEB, 0x0D};
  patch::replace("weapon slot 16 in a network game", game::at(game::kSelectSlot16NetCheck), slot16_was, slot16_now,
                 sizeof(slot16_now));
  static const uint8_t key16_was[2] = {0x74, 0x39};
  static const uint8_t key16_now[2] = {0xEB, 0x39};
  patch::replace("the key of weapon slot 16 in a network game", game::at(game::kKeySlot16NetCheck), key16_was, key16_now,
                 sizeof(key16_now));
  // The keys of slots 1 and 2 have a network way of their own too: the weapon is taken while the key is held and
  // what was in the hands may come back when it is let go (a tap on "2" after "1" left the first weapon in the
  // hands). They go the single-player way as well: je rel32 -> nop, jmp rel32 (the same target).
  static const uint8_t key1_was[6] = {0x0F, 0x84, 0xCF, 0x03, 0x00, 0x00};
  static const uint8_t key1_now[6] = {0x90, 0xE9, 0xCF, 0x03, 0x00, 0x00};
  patch::replace("the key of weapon slot 1 in a network game", game::at(game::kKeySlot1NetCheck), key1_was, key1_now,
                 sizeof(key1_now));
  static const uint8_t key2_was[6] = {0x0F, 0x84, 0x8E, 0x03, 0x00, 0x00};
  static const uint8_t key2_now[6] = {0x90, 0xE9, 0x8E, 0x03, 0x00, 0x00};
  patch::replace("the key of weapon slot 2 in a network game", game::at(game::kKeySlot2NetCheck), key2_was, key2_now,
                 sizeof(key2_now));
  patch::hook("clone", game::at(game::kCloneCore), reinterpret_cast<void*>(&on_clone_core),
              reinterpret_cast<void**>(&g_clone_core));
  patch::hook("spawn event", game::at(game::kSpawnRun), reinterpret_cast<void*>(&on_spawn_run),
              reinterpret_cast<void**>(&g_spawn_run));
  patch::hook("actor takes a child", game::at(game::kActorTake), reinterpret_cast<void*>(&on_actor_take),
              reinterpret_cast<void**>(&g_actor_take));
  patch::hook("player's links after a clone", game::at(game::kPlayerRemap), reinterpret_cast<void*>(&on_player_remap),
              reinterpret_cast<void**>(&g_player_remap));
}

uint8_t* entity_by_id(uint16_t id);

float health_of(unsigned id) {
  uint8_t* e = id == 0xFFFF ? controlled() : entity_by_id(static_cast<uint16_t>(id));
  return e ? *reinterpret_cast<float*>(e + game::kEntityHealth) : -1000.f;
}

void set_health(unsigned id, float health) {
  uint8_t* e = id == 0xFFFF ? controlled() : entity_by_id(static_cast<uint16_t>(id));
  if (!e) {
    LOGW("net: no entity %X to set the health of", id);
    return;
  }
  float& h = *reinterpret_cast<float*>(e + game::kEntityHealth);
  LOGI("net: health of entity %X: %.2f -> %.2f", *reinterpret_cast<uint16_t*>(e + game::kEntityId), h, health);
  h = health;
}

void log_state(const char* why) {
  Players& p = players();
  uint8_t* ctl = controlled();
  std::string slots;
  for (unsigned i = 0; i < p.size && i < 8; ++i) {
    const uint8_t* s = p.array + game::kNetPlayerSize * i;
    char one[160];
    const float* at = reinterpret_cast<const float*>(s + 0x48);  // the slot's network position
    uint16_t actor = *reinterpret_cast<const uint16_t*>(s + 0x64);
    snprintf(one, sizeof(one), " s%u=%X/%u/%s(%.2f,%.2f,%.2f)h%.2f", i, actor,
             (*reinterpret_cast<const uint32_t*>(s + 0x68) >> 16) & 7, *reinterpret_cast<void* const*>(s + 0x30) ? "peer" : "-",
             at[0], at[1], at[2], actor == 0xFFFF ? -1.f : health_of(actor));
    slots += one;
  }
  const float* pos = ctl ? reinterpret_cast<const float*>(ctl + 0xF0) : nullptr;  // translation of the body's matrix
  LOGI("net [%s]: network game %u, host %s, local slot %d, players %u, level %s, body %X at (%.2f,%.2f,%.2f)%s", why,
       game::ref<uint8_t>(game::kNetGame),
       game::ref<uintptr_t>(game::kNetHost) > 1 ? "ENet" : game::ref<uintptr_t>(game::kNetHost) ? "local" : "none",
       game::ref<int>(game::kNetLocalPlayer), p.size, game::ref<void*>(game::kLevel) ? "loaded" : "none",
       ctl ? entity_id(ctl) : 0xFFFF, pos ? pos[0] : 0.f, pos ? pos[1] : 0.f, pos ? pos[2] : 0.f, slots.c_str());
}

uint8_t* parent_of(uint8_t* e);

void log_entity(unsigned id) {
  uint8_t* server = game::ref<uint8_t*>(game::kServer);
  if (!server) {
    LOGI("net: no world");
    return;
  }
  uint8_t* table = *reinterpret_cast<uint8_t**>(server + game::kServerEntities);
  unsigned count = *reinterpret_cast<uint16_t*>(server + game::kServerEntityCount);
  unsigned copies = 0, alive = 0;
  for (unsigned i = 0; i < count; ++i) {
    uint8_t* e = *reinterpret_cast<uint8_t**>(table + i * 16 + 8);
    if (!e) continue;
    ++alive;
    uint32_t flags = *reinterpret_cast<uint32_t*>(e + game::kEntityFlags);
    if (flags & 0x10000) ++copies;
    if (*reinterpret_cast<uint16_t*>(e + game::kEntityId) != id) continue;
    const uint8_t* name = *reinterpret_cast<uint8_t**>(e + game::kEntityName);
    const float* at = reinterpret_cast<const float*>(e + game::kEntityPosition);
    uint8_t* parent = parent_of(e);
    using AsItem = uint8_t* (*)(uint8_t*);
    uint8_t* item = (*reinterpret_cast<AsItem**>(e))[game::kEntityAsItem / 8](e);
    LOGI("net: entity %X '%s' flags %X, hangs on %X, slot %d, life %d ms (goes at %d), at (%.2f,%.2f,%.2f)", id,
         name ? reinterpret_cast<const char*>(name) + 0x14 : "", flags,
         parent ? *reinterpret_cast<uint16_t*>(parent + game::kEntityId) : 0xFFFF, item ? item[game::kItemSlot] : -1,
         *reinterpret_cast<int*>(e + game::kEntityLife), *reinterpret_cast<int*>(e + game::kEntityLife + 4), at[0], at[1],
         at[2]);
    return;
  }
  if (id == 0xFFFF)
    LOGI("net: the world holds %u entities, %u of them spawned copies", alive, copies);
  else
    LOGI("net: no entity %X (the world holds %u, %u of them spawned copies)", id, alive, copies);
}

void log_controllers(bool all) {
  uint8_t* physics = game::ref<uint8_t*>(game::kPhysics);
  void** manager = physics ? *reinterpret_cast<void***>(physics + 0xE0) : nullptr;
  if (!manager) {
    LOGI("net: no physics");
    return;
  }
  void** vt = *reinterpret_cast<void***>(manager);
  unsigned n = reinterpret_cast<unsigned (*)(void*)>(vt[0])(manager);
  void** list = reinterpret_cast<void** (*)(void*)>(vt[1])(manager);
  unsigned orphans = 0;
  for (unsigned i = 0; i < n && list; ++i) {
    void* ctl = list[i];
    void** cvt = *reinterpret_cast<void***>(ctl);
    void* actor = reinterpret_cast<void* (*)(void*)>(cvt[4])(ctl);
    uint8_t* shape = reinterpret_cast<uint8_t* (*)(void*)>(game::at(game::kFirstShape))(actor);
    void* owner = shape ? *reinterpret_cast<void**>(shape + 0x10) : nullptr;
    const float* at = reinterpret_cast<const float* (*)(void*)>(cvt[3])(ctl);
    if (!owner) ++orphans;
    if (all || !owner)
      LOGI("net: controller %u of %u: %p owner %p flags %X at (%.2f,%.2f,%.2f)", i, n, ctl, owner,
           owner ? *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(owner) + 0x98) : 0, at[0], at[1], at[2]);
  }
  LOGI("net: %u character controllers, %u without an owner (manager: count +%llX, list +%llX, create +%llX, release +%llX)",
       n, orphans, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(vt[0]) - game::at(0)),
       static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(vt[1]) - game::at(0)),
       static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(vt[2]) - game::at(0)),
       static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(vt[3]) - game::at(0)));
}

void set_network_game(bool on) {
  game::ref<uint8_t>(game::kNetGame) = on ? 1 : 0;
  LOGI("net: network game flag = %d", on ? 1 : 0);
}

// --- where a new body stands ------------------------------------------------------------------------------
// A body is made as a copy of the host's and stands exactly where the host stands: the two players are inside
// each other until one walks away (each sees the other's weapon across its own view). A joined game moves its
// own body itself, so it is the one that steps aside - to a place where the host stood a moment ago: the host's
// way is kept (a point every 0.4 m), and the body goes to the latest point that is 1.2 m or more from the host.
// Such a point is free by construction (the host's own body has just been there). Right after a join or a level
// change there is no way yet, and the body stays where it was made.
struct Point {
  float x, y, z;
};
std::vector<Point> g_host_way;
uint16_t g_host_way_of = 0xFFFF;  // the host's body the way belongs to (a new level gives a new one)

uint8_t* host_body() {
  Players& p = players();
  if (p.size < 1 || !p.array) return nullptr;
  return entity_by_id(*reinterpret_cast<uint16_t*>(p.array + 0x64));
}

void keep_host_way() {
  uint8_t* host = host_body();
  if (!host) return;
  uint16_t id = *reinterpret_cast<uint16_t*>(host + game::kEntityId);
  if (id != g_host_way_of) {
    g_host_way.clear();
    g_host_way_of = id;
  }
  const float* at = reinterpret_cast<const float*>(host + game::kEntityPosition);
  if (!g_host_way.empty()) {
    const Point& last = g_host_way.back();
    float dx = at[0] - last.x, dy = at[1] - last.y, dz = at[2] - last.z;
    if (dx * dx + dy * dy + dz * dz < 0.4f * 0.4f) return;
  }
  if (g_host_way.size() >= 120) g_host_way.erase(g_host_way.begin());
  g_host_way.push_back({at[0], at[1], at[2]});
}

// A joined game knows the host's way only from the moment it sees the host: a player who has just come in would
// stand inside the host. The host keeps its own way too and sends its last stretch along with the new body.
void send_way(int slot) {
  constexpr size_t kMost = 60;
  size_t n = g_host_way.size() < kMost ? g_host_way.size() : kMost;
  if (!n) return;
  uint8_t out[2 + kMost * sizeof(Point)];
  uint16_t count = static_cast<uint16_t>(n);
  memcpy(out, &count, 2);
  memcpy(out + 2, g_host_way.data() + (g_host_way.size() - n), n * sizeof(Point));
  link::send(link::kWay, out, 2 + n * sizeof(Point), true, slot);
}

void on_way(int, const uint8_t* data, size_t size) {
  uint16_t n = 0;
  if (size < 2) return;
  memcpy(&n, data, 2);
  if (n > 200 || size < 2 + n * sizeof(Point)) return;
  g_host_way.resize(n);
  if (n) memcpy(g_host_way.data(), data + 2, n * sizeof(Point));
  uint8_t* host = host_body();
  if (host) g_host_way_of = *reinterpret_cast<uint16_t*>(host + game::kEntityId);
  LOGI("net: the host's way came with the body: %u points", n);
}

// The game's own ray (exe+728CF0, what its "pick" and the camera's collision use): {from, direction, range,
// 0x60000, 4} -> whether it hit, and how far (the entity `ignore` is let through).
bool ray_hit(float x, float y, float z, float dx, float dy, float dz, float range, void* ignore, float* distance) {
  struct Ray {
    float from[3], dir[3], range;
    uint32_t mask, kind, pad;
  } ray = {{x, y, z}, {dx, dy, dz}, range, 0x60000, 4, 0};
  alignas(16) uint8_t result[0x40] = {};
  result[0x26] = 1;
  void* skip = ignore;
  using Pick = bool (*)(void*, void*, void*, void**);
  bool hit = reinterpret_cast<Pick>(game::at(0x728CF0))(&ray, nullptr, result, &skip);
  float d = 0.f;
  memcpy(&d, result + 8, 4);
  if (distance) *distance = d;
  return hit && d > 0.f && d <= range;
}

void step_aside(uint8_t* body) {
  uint8_t* host = host_body();
  if (!host || host == body) return;
  const float* h = reinterpret_cast<const float*>(host + game::kEntityPosition);
  for (size_t i = g_host_way.size(); i-- > 0;) {
    const Point& p = g_host_way[i];
    float dx = p.x - h[0], dz = p.z - h[2], dy = p.y - h[1];
    float flat = sqrtf(dx * dx + dz * dz);
    if (flat < 1.2f) continue;
    if (flat > 10.f || fabsf(dy) > 2.5f) break;  // the way went far off: better inside the host than lost
    float m[16];
    memcpy(m, body + game::kEntityMatrix, sizeof(m));
    m[12] = p.x, m[13] = p.y, m[14] = p.z;
    using SetMatrix = void (*)(uint8_t*, const float*, int);
    (*reinterpret_cast<SetMatrix**>(body))[game::kEntitySetMatrix / 8](body, m, 0);
    LOGI("net: the own body steps %.1f m aside, to where the host stood: (%.2f,%.2f,%.2f)", flat, p.x, p.y, p.z);
    return;
  }
  // No way of the host to go by (the level has just begun, the host has not walked): a free place around the host,
  // behind it first, found by the game's own ray (the one its "pick" uses): nothing between the host's chest and
  // the place, and a floor under the place at the host's height.
  float yaw = atan2f(-reinterpret_cast<const float*>(host + game::kEntityMatrix)[8], -reinterpret_cast<const float*>(host + game::kEntityMatrix)[10]);
  void* host_handle = *reinterpret_cast<void**>(host + game::kEntityHandle);
  void* own_handle = *reinterpret_cast<void**>(body + game::kEntityHandle);
  for (int k = 0; k < 8; ++k) {
    static const float turns[8] = {0.f, 0.785f, -0.785f, 1.571f, -1.571f, 2.356f, -2.356f, 3.142f};
    float a = yaw + turns[k];
    float dx = sinf(a), dz = cosf(a);
    const float reach = 1.3f;
    float hit = 0.f;
    if (ray_hit(h[0], h[1] + 1.2f, h[2], dx, 0.f, dz, reach + 0.45f, host_handle, &hit)) continue;  // a wall in between
    float cx = h[0] + dx * reach, cz = h[2] + dz * reach;
    float down = 0.f;
    if (!ray_hit(cx, h[1] + 1.2f, cz, 0.f, -1.f, 0.f, 2.4f, own_handle, &down)) continue;  // no floor there
    float floor_y = h[1] + 1.2f - down;
    if (fabsf(floor_y - h[1]) > 0.45f) continue;  // a step, a pit
    float m[16];
    memcpy(m, body + game::kEntityMatrix, sizeof(m));
    m[12] = cx, m[13] = floor_y, m[14] = cz;
    using SetMatrix = void (*)(uint8_t*, const float*, int);
    (*reinterpret_cast<SetMatrix**>(body))[game::kEntitySetMatrix / 8](body, m, 0);
    LOGI("net: the own body steps %.1f m aside from the host (the host has not walked): (%.2f,%.2f,%.2f), %d tries", reach, cx, floor_y, cz,
         k + 1);
    return;
  }
  LOGI("net: the own body stays where it was made (the host has not walked %s; no free place around it)",
       g_host_way.size() > 1 ? "far enough" : "yet");
}

// The picture of the weapon and ammunition on the screen belongs to one player entity (the weapon HUD keeps a
// pointer to it and reads its inventory at every refresh). The game sets it once - single player never changes
// the body - so a joined game goes on showing the host's body from the loaded level, and after a death the old
// body: when the host then takes the old body out, the HUD reads a freed entity and the game falls. The HUD is
// given the body this PC controls each time that body changes.
void bind_hud(uint8_t* body) {
  uint8_t* ui = game::ref<uint8_t*>(game::kUi);
  uint8_t* manager = ui ? *reinterpret_cast<uint8_t**>(ui + game::kUiHudManager) : nullptr;
  uint8_t* hud = manager ? *reinterpret_cast<uint8_t**>(manager + game::kHudManagerWeaponHud) : nullptr;
  if (!hud) {
    LOGW("net: no weapon HUD to give the body to");
    return;
  }
  uint8_t* before = *reinterpret_cast<uint8_t**>(hud + game::kWeaponHudPlayer);
  using Bind = int64_t (*)(uint8_t*, uint8_t*, char, char);
  reinterpret_cast<Bind>(game::at(game::kWeaponHudBind))(hud, body, 0x10, 0);
  LOGI("net: the weapon HUD shows the body %X now (it showed %s)", *reinterpret_cast<uint16_t*>(body + game::kEntityId),
       !before ? "nobody" : before == body ? "it already" : "another body");
}

// --- a joined player's own ammunition and money ------------------------------------------------------------
// Every body the host gives a joined player is a copy of the host's own player, with the host's inventory: at a
// level change, after a death, at every new join the joined player's rounds - the military ones are the money -
// were the host's again (0.4.1: "the money of each player"). A joined game keeps what its own player carries now
// (each pack's number, every second, by the pack's kind: its name without the "_0013" the level gave it), and
// puts those numbers into the packs of its next body. The packs are counted on this PC only (the host never asks):
// shooting and trading go by them. A kind the next body has no pack of is lost; weapons, medkits and filters are
// still the host's copy.
constexpr size_t kAmmoCount = 0x4F4;          // ammunition entity + : u32 rounds (read: vtbl + 0xE78)
constexpr uintptr_t kAmmoSetCount = 0x2B2720; // (entity*, int rounds): the game's own setter (the HUD follows it)
constexpr size_t kItemEntity = 0x430;         // an inventory's item = its entity + this
constexpr size_t kInventoryItems = 0xF0, kInventoryItemCount = 0xFA;

std::vector<std::pair<std::string, uint32_t>> g_gear;  // the kinds and numbers this PC's player carried last
uint16_t g_gear_body = 0xFFFF;                          // the body they were read from
uint64_t g_gear_at = 0;
bool g_gear_loaded = false;

// The numbers are kept in a file too (metronet\gear.txt in %APPDATA%; the lab: its own user folder), so that
// they last over a restart of the game: the next session's first body gets them as well.
std::wstring gear_file() {
  wchar_t folder[MAX_PATH] = {};
  if (!g_cfg.lab_userdir.empty())
    lstrcpynW(folder, g_cfg.lab_userdir.c_str(), MAX_PATH);
  else if (g_cfg.lab || !SHGetSpecialFolderPathW(nullptr, folder, CSIDL_APPDATA, FALSE))
    return L"";
  std::wstring dir = std::wstring(folder) + L"\\metronet";
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir + L"\\gear.txt";
}

void load_gear() {
  if (g_gear_loaded) return;
  g_gear_loaded = true;
  std::wstring path = gear_file();
  FILE* f = path.empty() ? nullptr : _wfopen(path.c_str(), L"r");
  if (!f) return;
  char kind[96];
  unsigned rounds = 0;
  while (fscanf(f, "%95s %u", kind, &rounds) == 2) g_gear.push_back({kind, rounds});
  fclose(f);
  LOGI("net: this player's own rounds from the last game: %zu kinds", g_gear.size());
}

void save_gear() {
  std::wstring path = gear_file();
  FILE* f = path.empty() ? nullptr : _wfopen(path.c_str(), L"w");
  if (!f) return;
  for (const auto& [kind, rounds] : g_gear) fprintf(f, "%s %u\n", kind.c_str(), rounds);
  fclose(f);
}

std::string ammo_kind(const uint8_t* e) {
  const uint8_t* name = *reinterpret_cast<uint8_t* const*>(e + game::kEntityName);
  std::string s = name ? reinterpret_cast<const char*>(name) + 0x14 : "";
  size_t cut = s.find_last_of('_');
  if (cut != std::string::npos && cut + 1 < s.size() && s.find_first_not_of("0123456789", cut + 1) == std::string::npos)
    s.resize(cut);
  return s;
}

template <typename Fn>
void each_ammo(uint8_t* body, Fn fn) {
  uint8_t* inv = *reinterpret_cast<uint8_t**>(body + game::kActorInventory);
  if (!inv || *reinterpret_cast<uintptr_t*>(inv) != game::at(game::kPlayerInventory)) return;
  uint8_t** items = *reinterpret_cast<uint8_t***>(inv + kInventoryItems);
  unsigned n = *reinterpret_cast<uint16_t*>(inv + kInventoryItemCount);
  for (unsigned k = 0; k < n && k < 256 && items; ++k) {
    uint8_t* item = items[k];
    if (!item) continue;
    uint8_t* e = item - kItemEntity;
    if (IsBadReadPtr(e, 0x500) || *reinterpret_cast<uintptr_t*>(e) != game::at(game::kAmmoClass)) continue;
    fn(e);
  }
}

void keep_gear() {
  if (!g_cfg.net_own_gear || game::ref<int>(game::kNetLocalPlayer) <= 0 || g_own_body != 0xFFFF) return;
  uint8_t* own = controlled();
  if (!own || *reinterpret_cast<uint16_t*>(own + game::kEntityId) != g_gear_body) return;
  if (!(*reinterpret_cast<float*>(own + game::kEntityHealth) > 0.f)) return;
  uint64_t now = GetTickCount64();
  if (now - g_gear_at < 1000) return;
  g_gear_at = now;
  std::vector<std::pair<std::string, uint32_t>> now_carried;
  each_ammo(own, [&](uint8_t* e) { now_carried.push_back({ammo_kind(e), *reinterpret_cast<uint32_t*>(e + kAmmoCount)}); });
  if (!now_carried.empty() && now_carried != g_gear) {
    g_gear.swap(now_carried);
    save_gear();
  }
}

// A new body of this PC's player: its packs get the numbers the player carried (none on the first body of the run).
void give_gear(uint8_t* body) {
  uint16_t id = *reinterpret_cast<uint16_t*>(body + game::kEntityId);
  if (g_cfg.net_own_gear) load_gear();
  if (!g_cfg.net_own_gear || g_gear.empty()) {
    g_gear_body = id;
    return;
  }
  int set = 0;
  std::string said;
  each_ammo(body, [&](uint8_t* e) {
    std::string kind = ammo_kind(e);
    for (const auto& [k, rounds] : g_gear) {
      if (k != kind) continue;
      uint32_t was = *reinterpret_cast<uint32_t*>(e + kAmmoCount);
      if (was != rounds) {
        reinterpret_cast<void (*)(uint8_t*, int)>(game::at(kAmmoSetCount))(e, static_cast<int>(rounds));
        ++set;
        if (said.size() < 300) said += " " + kind + " " + std::to_string(was) + "->" + std::to_string(rounds);
      }
      break;
    }
  });
  g_gear_body = id;
  LOGI("net: the own body %X carries this player's own rounds again (%d packs changed:%s)", id, set, said.c_str());
}

// A joined game has just been given its body. The copy comes with the weapons in their slots but with no weapon
// "in the hands" of its own - the picture on the screen is still the host's weapon from the loaded level, and the
// first shot comes from whatever slot the inventory happens to stand on. The main weapon is taken into the
// hands the way the keys do it: slot 16 (the third slot of Redux), else 2, else 1.
void own_body_ready(uint8_t* body) {
  give_gear(body);
  uint8_t* inv = *reinterpret_cast<uint8_t**>(body + game::kActorInventory);
  if (!inv || *reinterpret_cast<uintptr_t*>(inv) != game::at(game::kPlayerInventory)) {
    LOGW("net: the own body %X has no player's inventory", *reinterpret_cast<uint16_t*>(body + game::kEntityId));
    return;
  }
  using Select = int64_t (*)(uint8_t*, char, int64_t, int);
  Select select = (*reinterpret_cast<Select**>(inv))[game::kInventorySelect / 8];
  int taken = 0;
  for (int slot : {16, 2, 1}) {
    if (select(inv, static_cast<char>(slot), 0, 0)) {
      taken = slot;
      break;
    }
  }
  LOGI("net: the own body %X is here: %u items in its inventory, the weapon of slot %d goes into the hands",
       *reinterpret_cast<uint16_t*>(body + game::kEntityId), *reinterpret_cast<uint16_t*>(inv + 0xFA), taken);
  bind_hud(body);
  step_aside(body);
}

// --- hanging on a thing ---------------------------------------------------------------------------------------
// The level script's plain attach and detach (the engine does them a frame later). A character nobody controls
// here is carried by its parent; a body this PC controls is not (see "a seat beside the host").
void hang_on(uint8_t* child, uint8_t* parent) {
  void* ch = *reinterpret_cast<void**>(child + game::kEntityHandle);
  void* ph = *reinterpret_cast<void**>(parent + game::kEntityHandle);
  if (!ch || !ph) return;
  void* bone = nullptr;
  using AttachTo = int64_t (*)(void**, void**, void**, const float*);
  reinterpret_cast<AttachTo>(game::at(game::kAttachTo))(&ch, &ph, &bone, nullptr);
}

void come_off(uint8_t* child) {
  void* ch = *reinterpret_cast<void**>(child + game::kEntityHandle);
  if (ch) reinterpret_cast<int64_t (*)(void**)>(game::at(game::kDetach))(&ch);
}

// --- rail vehicles -----------------------------------------------------------------------------------------
// A handcar goes along its track by a path animation: entity + 0x6A0 -> {+8 time, +0xC length, +0x1C speed};
// its update (0x370FD0) sets the speed from what the level script and the pumping gave it, and the engine's
// animation moves the entity. Nothing of that is in the network game: each PC's script drives its own copy, and
// the copies part within seconds (l03_chase: the joined game's handcar was 155 m ahead after two minutes).
// The host says where its rail vehicles are on their tracks and how fast they go 20 times a second; a joined
// game leads its own copies by that (lead_rail).
constexpr uintptr_t kRailClasses[] = {0xA1BFC0, 0xB2BF10, 0xB2D110, 0xB2E100};  // vtables of the drezina family
constexpr size_t kRailAnim = 0x6A0;
constexpr size_t kAnimTime = 0x8, kAnimLength = 0xC, kAnimSpeed = 0x1C;

bool is_rail(const uint8_t* e) {
  uintptr_t cls = *reinterpret_cast<const uintptr_t*>(e) - game::at(0);
  for (uintptr_t c : kRailClasses)
    if (c == cls) return true;
  return false;
}

// What a player can ride (0.4.1: "transport other than handcars"): a handcar, or whatever else the level's script
// hangs the player's body on - the car of l28, a cage, a lift's floor, a thing of a scene - but not a character
// (a monster that grabs, a man who leads) and not another player.
bool is_carrier(const uint8_t* e) {
  if (!e) return false;
  if (is_rail(e)) return true;
  if (*reinterpret_cast<const uint32_t*>(e + game::kEntityFlags) & 4u) return false;
  if (*reinterpret_cast<const uintptr_t*>(e) == game::at(game::kPlayerClass)) return false;
  return *reinterpret_cast<const uint16_t*>(e + game::kEntityId) != 0xFFFF;
}

#pragma pack(push, 1)
struct RailState {
  uint16_t id;
  float time, speed;
};
#pragma pack(pop)

std::vector<uint16_t> g_rails;  // the host: the rail vehicles of the level
uint64_t g_rails_found_at = 0, g_rails_sent_at = 0;

void send_rails() {
  uint64_t now = GetTickCount64();
  uint8_t* server = game::ref<uint8_t*>(game::kServer);
  if (!server || !link::peers()) return;
  if (now - g_rails_found_at >= 1000) {
    g_rails_found_at = now;
    g_rails.clear();
    uint8_t* table = *reinterpret_cast<uint8_t**>(server + game::kServerEntities);
    unsigned count = *reinterpret_cast<uint16_t*>(server + game::kServerEntityCount);
    for (unsigned i = 0; i < count && g_rails.size() < 32; ++i) {
      uint8_t* e = *reinterpret_cast<uint8_t**>(table + i * 16 + 8);
      if (e && is_rail(e) && *reinterpret_cast<uint8_t**>(e + kRailAnim)) g_rails.push_back(*reinterpret_cast<uint16_t*>(e + game::kEntityId));
    }
  }
  if (g_rails.empty() || now - g_rails_sent_at < 50) return;
  g_rails_sent_at = now;
  uint8_t out[2 + 32 * sizeof(RailState)];
  uint16_t n = 0;
  for (uint16_t id : g_rails) {
    uint8_t* e = entity_by_id(id);
    uint8_t* anim = e && is_rail(e) ? *reinterpret_cast<uint8_t**>(e + kRailAnim) : nullptr;
    if (!anim) continue;
    RailState st = {id, *reinterpret_cast<float*>(anim + kAnimTime), *reinterpret_cast<float*>(anim + kAnimSpeed)};
    memcpy(out + 2 + n * sizeof(st), &st, sizeof(st));
    ++n;
  }
  if (!n) return;
  memcpy(out, &n, 2);
  link::send(link::kRails, out, 2 + n * sizeof(RailState), false);
}

struct RailTarget {
  RailState state;
  uint64_t at;
};
std::vector<RailTarget> g_rail_targets;  // a joined game: what the host said last

void on_rails(int, const uint8_t* data, size_t size) {
  uint16_t n = 0;
  if (size < 2) return;
  memcpy(&n, data, 2);
  if (n > 32 || size < 2 + n * sizeof(RailState)) return;
  uint64_t now = GetTickCount64();
  static bool told = false;
  if (!told && n) {
    told = true;
    LOGI("net: the host leads %u rail vehicle(s) of this level", n);
  }
  for (uint16_t i = 0; i < n; ++i) {
    RailState st;
    memcpy(&st, data + 2 + i * sizeof(st), sizeof(st));
    bool known = false;
    for (RailTarget& t : g_rail_targets)
      if (t.state.id == st.id) t.state = st, t.at = now, known = true;
    if (!known && g_rail_targets.size() < 32) g_rail_targets.push_back({st, now});
  }
}

// The engine moves a rail vehicle by what its animation advanced in the frame: writing the animation's time moves
// nothing (tried: the joined game's handcar kept a 1.4 m lead it had run up while the host's was braking). What
// can be led is the speed. Right after the vehicle's own update has set it, a joined game replaces it by the
// host's speed plus a pull towards the host's place on the track (2 per second of the difference in animation
// time): the engine then moves the vehicle itself, and the level's script on this PC sees the speed the vehicle
// really has (its "the handcar has stopped" comes when the host's handcar stops).
constexpr size_t kRailSpeed = 0x6C4;  // entity + : float, what the update writes into the animation's speed
constexpr float kRailPull = 2.f;

void lead_rail(uint8_t* e) {
  if (game::ref<uintptr_t>(game::kNetHost) <= 1 || game::ref<int>(game::kNetLocalPlayer) <= 0 || g_rail_targets.empty()) return;
  uint16_t id = *reinterpret_cast<uint16_t*>(e + game::kEntityId);
  uint64_t now = GetTickCount64();
  for (const RailTarget& t : g_rail_targets) {
    if (t.state.id != id) continue;
    if (now - t.at > 1500) return;  // the host has gone quiet (it is loading): the own copy goes by itself
    uint8_t* anim = *reinterpret_cast<uint8_t**>(e + kRailAnim);
    if (!anim) return;
    float host_time = t.state.time + t.state.speed * static_cast<float>(now - t.at) / 1000.f;
    float behind = host_time - *reinterpret_cast<float*>(anim + kAnimTime);
    float speed = t.state.speed + behind * kRailPull;
    if (speed < -1.f) speed = -1.f;
    if (speed > t.state.speed + 4.f) speed = t.state.speed + 4.f;
    if (fabsf(speed) < 0.0005f) speed = 0;
    *reinterpret_cast<float*>(anim + kAnimSpeed) = speed;
    *reinterpret_cast<float*>(e + kRailSpeed) = speed;
    return;
  }
}

using RailUpdateFn = int64_t (*)(uint8_t*, int);
RailUpdateFn g_rail_update[3] = {};

int64_t on_rail_update_0(uint8_t* e, int ms) {
  int64_t r = g_rail_update[0](e, ms);
  lead_rail(e);
  return r;
}
int64_t on_rail_update_1(uint8_t* e, int ms) {
  int64_t r = g_rail_update[1](e, ms);
  lead_rail(e);
  return r;
}
int64_t on_rail_update_2(uint8_t* e, int ms) {
  int64_t r = g_rail_update[2](e, ms);
  lead_rail(e);
  return r;
}

void install_rails() {
  // the per-frame update of the drezina family: the base's (also called by the two others) and the two overrides
  patch::hook("rail vehicle update", game::at(0x36D620), reinterpret_cast<void*>(&on_rail_update_0), reinterpret_cast<void**>(&g_rail_update[0]));
  patch::hook("rail vehicle update (2)", game::at(0x370000), reinterpret_cast<void*>(&on_rail_update_1), reinterpret_cast<void**>(&g_rail_update[1]));
  patch::hook("handcar update", game::at(0x370FD0), reinterpret_cast<void*>(&on_rail_update_2), reinterpret_cast<void**>(&g_rail_update[2]));
}

// --- a seat beside the host -----------------------------------------------------------------------------------
// The level's script seats "the player" on the handcar (the body becomes the handcar's child, on a bone). A
// joined player's body came later and stands on the track: the handcar has no floor to stand on, the game's
// plain attach does not carry a body that a player controls, and the script's "platform" attach (lifts) throws
// it out of the world there. So while the host's body rides a rail vehicle, a joined game keeps its own body
// across the vehicle's middle line from the host's seat by putting it there every frame.
struct Seat {
  uint16_t carrier = 0xFFFF;
  float x = 0, y = 0, z = 0;  // in the carrier's own axes
} g_seat;
uint64_t g_seat_off_at = 0;

// The other PCs see a joined player's body by the positions its own PC sends: on a vehicle at 4 m/s it would
// trail behind and shake. So the body's PC says "this body sits on that thing, there" (and "it is off"), and
// every other PC hangs its copy of the body on its copy of the thing - a body nobody controls here is carried
// by the game's plain attach.
#pragma pack(push, 1)
struct SeatNote {
  uint16_t body, carrier;  // carrier FFFF: off
  float x, y, z;
};
#pragma pack(pop)
uint16_t g_seat_told_body = 0xFFFF, g_seat_told_carrier = 0xFFFF;
int g_seat_told_peers = 0;

// Said again when the body, the vehicle or the number of connected PCs changes (a PC that came later must know).
void tell_seat(uint8_t* own, const Seat& seat) {
  uint16_t body = *reinterpret_cast<uint16_t*>(own + game::kEntityId);
  int peers = link::peers();
  if (body == g_seat_told_body && seat.carrier == g_seat_told_carrier && peers == g_seat_told_peers) return;
  if (seat.carrier == 0xFFFF && g_seat_told_carrier == 0xFFFF && body == g_seat_told_body) {
    g_seat_told_peers = peers;  // nothing to say: it never sat
    return;
  }
  SeatNote note = {body, seat.carrier, seat.x, seat.y, seat.z};
  if (link::send(link::kSeat, &note, sizeof(note), true))
    g_seat_told_body = body, g_seat_told_carrier = seat.carrier, g_seat_told_peers = peers;
}

// The host's own body: where the level's script has seated it.
void tell_host_seat() {
  uint8_t* own = controlled();
  if (!own) return;
  uint8_t* carrier = parent_of(own);
  Seat seat;
  if (carrier && is_carrier(carrier)) {
    const float* c = reinterpret_cast<const float*>(carrier + game::kEntityMatrix);
    const float* h = reinterpret_cast<const float*>(own + game::kEntityPosition);
    float d[3] = {h[0] - c[12], h[1] - c[13], h[2] - c[14]};
    seat.carrier = *reinterpret_cast<uint16_t*>(carrier + game::kEntityId);
    seat.x = d[0] * c[0] + d[1] * c[1] + d[2] * c[2];
    seat.y = d[0] * c[4] + d[1] * c[5] + d[2] * c[6];
    seat.z = d[0] * c[8] + d[1] * c[9] + d[2] * c[10];
  }
  tell_seat(own, seat);
}

void on_seat(int from, const uint8_t* data, size_t size) {
  SeatNote note;
  if (size < sizeof(note)) return;
  memcpy(&note, data, sizeof(note));
  uint8_t* body = entity_by_id(note.body);
  if (game::ref<int>(game::kNetLocalPlayer) == 0 && link::peers() > 1) link::send(link::kSeat, data, size, true);  // to the other joined games
  if (!body || body == controlled() || !is_player(note.body)) return;
  uint8_t* carrier = note.carrier == 0xFFFF ? nullptr : entity_by_id(note.carrier);
  if (!carrier) {
    if (parent_of(body)) come_off(body);
    LOGI("net: player %d's body %X is off its vehicle", from, note.body);
    return;
  }
  if (parent_of(body) == carrier) return;  // it sits there already (the level came with it so)
  const float* c = reinterpret_cast<const float*>(carrier + game::kEntityMatrix);
  float m[16];
  memcpy(m, body + game::kEntityMatrix, sizeof(m));
  m[12] = c[12] + c[0] * note.x + c[4] * note.y + c[8] * note.z;
  m[13] = c[13] + c[1] * note.x + c[5] * note.y + c[9] * note.z;
  m[14] = c[14] + c[2] * note.x + c[6] * note.y + c[10] * note.z;
  using SetMatrix = void (*)(uint8_t*, const float*, int);
  (*reinterpret_cast<SetMatrix**>(body))[game::kEntitySetMatrix / 8](body, m, 0);
  hang_on(body, carrier);
  LOGI("net: player %d's body %X sits on %X (%.2f, %.2f, %.2f from its middle): hung on it here", from, note.body, note.carrier,
       note.x, note.y, note.z);
}

void follow_seat() {
  uint8_t* own = controlled();
  uint8_t* host = host_body();
  uint8_t* carrier = own && host && own != host ? parent_of(host) : nullptr;
  if (!carrier || !is_carrier(carrier)) {
    if (g_seat.carrier != 0xFFFF) LOGI("net: the host is off the vehicle %X: the own body is free again", g_seat.carrier);
    g_seat.carrier = 0xFFFF;
    if (own) tell_seat(own, g_seat);
    return;
  }
  uint64_t now = GetTickCount64();
  if (parent_of(own)) {
    // the own level script has seated the own body too - into the host's very seat: taken off, kept beside
    if (now - g_seat_off_at > 1000) {
      g_seat_off_at = now;
      come_off(own);
    }
    return;
  }
  const float* c = reinterpret_cast<const float*>(carrier + game::kEntityMatrix);  // rows: right, up, forward, place
  uint16_t id = *reinterpret_cast<uint16_t*>(carrier + game::kEntityId);
  if (g_seat.carrier != id) {
    const float* h = reinterpret_cast<const float*>(host + game::kEntityPosition);
    float d[3] = {h[0] - c[12], h[1] - c[13], h[2] - c[14]};
    g_seat.carrier = id;
    g_seat.x = d[0] * c[0] + d[1] * c[1] + d[2] * c[2];
    g_seat.y = d[0] * c[4] + d[1] * c[5] + d[2] * c[6];
    g_seat.z = d[0] * c[8] + d[1] * c[9] + d[2] * c[10];
    float host_x = g_seat.x;
    // Where the others sit, measured on l03's handcar: its middle is the lever (a body put there stood inside it
    // and every bullet hit the lever); across the middle from the host a man pumps the lever 0.85 m in front of
    // the eyes. The host's own side is the open one: the first joined player rides on the handcar's edge beside
    // the host, the second takes the seat across the middle, the third hangs on outside the first.
    // (0.4.1, "the second player stands on the edge"): when the host sits at one end of the vehicle, the first
    // joined player stands on the platform on the host's side, 1.4 m nearer the other end (l03: between the host
    // and the passenger, beside the lever) instead of out on the edge.
    int me = game::ref<int>(game::kNetLocalPlayer);
    float side = host_x >= 0 ? 1.f : -1.f;
    float off = fabsf(host_x) >= 0.35f ? fabsf(host_x) : 0.7f;
    if (!is_rail(carrier)) {
      // another vehicle (the car of l28, a lift): the seat across the vehicle's middle from the host, or behind the
      // host when the host sits in the middle; the third and the next further back
      if (fabsf(host_x) >= 0.3f && me <= 1) {
        g_seat.x = -host_x;
      } else {
        g_seat.z -= 1.0f * (me <= 1 ? 1 : me - (fabsf(host_x) >= 0.3f ? 1 : 0));
      }
    } else if (me <= 1 && fabsf(g_seat.z) >= 0.9f) {
      g_seat.x = side * off;
      g_seat.z -= (g_seat.z > 0 ? 1.f : -1.f) * 1.4f;
    } else if (me <= 1) {
      g_seat.x = side * (off + 0.75f);
    } else if (me == 2) {
      g_seat.x = -side * off;
    } else {
      g_seat.x = side * (off + 1.5f);
    }
    const uint8_t* name = *reinterpret_cast<uint8_t**>(carrier + game::kEntityName);
    LOGI("net: the host rides '%s' (%X): the own body is kept beside its seat (the host sits %.2f m off the middle, "
         "this body %.2f; %.2f up, %.2f along)", name ? reinterpret_cast<const char*>(name) + 0x14 : "", id, host_x, g_seat.x,
         g_seat.y, g_seat.z);
  }
  float m[16];
  memcpy(m, own + game::kEntityMatrix, sizeof(m));
  m[12] = c[12] + c[0] * g_seat.x + c[4] * g_seat.y + c[8] * g_seat.z;
  m[13] = c[13] + c[1] * g_seat.x + c[5] * g_seat.y + c[9] * g_seat.z;
  m[14] = c[14] + c[2] * g_seat.x + c[6] * g_seat.y + c[10] * g_seat.z;
  using SetMatrix = void (*)(uint8_t*, const float*, int);
  (*reinterpret_cast<SetMatrix**>(own))[game::kEntitySetMatrix / 8](own, m, 0);
  tell_seat(own, g_seat);
}

// --- the other players' first-person hands ----------------------------------------------------------------------
// Every player's body carries the first-person hands ("players_hands": the bare arms, and the weapon in them) at
// its eyes. On the PC that controls the body that is the picture of one's own hands; every other PC draws them in
// the world where the body's eyes are: a big bare arm in front of the figure for whoever comes close, and across
// the whole view while two players stand in one spot. Each PC puts away the hands of the bodies it does not
// control. The way chosen ([net] hide_hands, default 1) is the game's own "hide hands" of its scenes (exe+408660):
// the hands' link to the eyes' bone is dropped (exe+6A78A0) and they hang on the body itself again (exe+6A87E0).
// That link is at hands + 0x1E0; the game may make it anew (a weapon change), so it is looked at again.
constexpr size_t kHandsEyesLink = 0x1E0;
int g_hands_mode = 1;
uint16_t g_hands_done[8] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
uint64_t g_hands_logged = 0;

uint8_t* hands_of(uint8_t* body) {
  uint8_t* server = game::ref<uint8_t*>(game::kServer);
  if (!server || !body) return nullptr;
  uint8_t* table = *reinterpret_cast<uint8_t**>(server + game::kServerEntities);
  unsigned count = *reinterpret_cast<uint16_t*>(server + game::kServerEntityCount);
  if (!table) return nullptr;
  for (unsigned i = 0; i < count; ++i) {
    uint8_t* e = *reinterpret_cast<uint8_t**>(table + i * 16 + 8);
    if (!e || e == body || parent_of(e) != body) continue;
    const uint8_t* name = *reinterpret_cast<uint8_t**>(e + game::kEntityName);
    if (name && !strncmp(reinterpret_cast<const char*>(name) + 0x14, "players_hands", 13)) return e;
  }
  return nullptr;
}

// Puts one body's hands away; false when it has none (yet).
bool put_hands_away(uint8_t* body, int mode, bool again) {
  uint8_t* hands = hands_of(body);
  if (!hands) return false;
  uint16_t hid = *reinterpret_cast<uint16_t*>(hands + game::kEntityId);
  uint16_t bid = *reinterpret_cast<uint16_t*>(body + game::kEntityId);
  if (mode == 1) {
    if (again && !*reinterpret_cast<void**>(hands + kHandsEyesLink)) return true;  // still away
    reinterpret_cast<void (*)(uint8_t*)>(game::at(0x6A78A0))(hands);
    using Hang = uint32_t (*)(uint16_t, uint8_t*, uint16_t, uint64_t, uint64_t);
    reinterpret_cast<Hang>(game::at(0x6A87E0))(hid, hands, bid, 0, 0);
  } else if (mode == 2) {
    *reinterpret_cast<uint32_t*>(hands + game::kEntityFlags) |= 0x4000;
  } else if (mode == 3) {
    if (again && !parent_of(hands)) return true;
    come_off(hands);
    float m[16];
    memcpy(m, hands + game::kEntityMatrix, sizeof(m));
    m[13] -= 400.f;
    using SetMatrix = void (*)(uint8_t*, const float*, int);
    (*reinterpret_cast<SetMatrix**>(hands))[game::kEntitySetMatrix / 8](hands, m, 0);
  }
  uint64_t now = GetTickCount64();
  if (!again || now - g_hands_logged > 10000) {
    g_hands_logged = now;
    LOGI("net: the hands %X of body %X are put away here (way %d%s)", hid, bid, mode, again ? ", once more" : "");
  }
  return true;
}

void hide_others_hands() {
  if (g_hands_mode <= 0 || game::ref<uintptr_t>(game::kNetHost) < 2) return;
  Players& p = players();
  uint8_t* mine = controlled();
  if (!p.array) return;
  for (unsigned slot = 0; slot < p.size && slot < 8; ++slot) {
    uint16_t id = *reinterpret_cast<uint16_t*>(p.array + game::kNetPlayerSize * slot + 0x64);
    uint8_t* body = id != 0xFFFF ? entity_by_id(id) : nullptr;
    if (!body || body == mine) {
      g_hands_done[slot] = 0xFFFF;
      continue;
    }
    bool again = g_hands_done[slot] == id;
    if (put_hands_away(body, g_hands_mode, again)) g_hands_done[slot] = id;
  }
}

void set_hands_mode(int mode) { g_hands_mode = mode; }

// --- the hands in the host's scenes -----------------------------------------------------------------------------
// A scene of the story puts the player's hands away (the talk on the handcar of l03, a scene with a camera): the
// level script's "hide hands" on the host's PC. A joined game's own script does not always come to that block (the
// scene began by the host's seat, the host's zone), and its player rode on with the weapon in the hands. The host
// tells whether its own hands are away; a joined game keeps its own hands away for as long (as the game's "hide
// hands" does), and takes the weapon back into the hands when the host's are out again.
bool g_host_hands_away = false;   // a joined game: what the host said
bool g_own_hands_put = false;     // a joined game: the mod put this PC's hands away for the host's scene
int g_hands_said = -1, g_hands_said_peers = -1;   // the host: what it said last, to how many PCs
// The host says "away" only after it has seen its hands out in this level (with this body): should the link be
// empty in some state of plain play as well, the joined players would otherwise be left without their weapons.
uint16_t g_hands_seen_out = 0xFFFF;               // the host's body whose hands were seen with their link

bool own_hands_away(uint8_t* own) {
  uint8_t* hands = hands_of(own);
  return hands && !*reinterpret_cast<void**>(hands + kHandsEyesLink);
}

void on_hands(int, const uint8_t* data, size_t size) {
  if (size < 1 || game::ref<int>(game::kNetLocalPlayer) == 0) return;
  bool away = data[0] != 0;
  if (away != g_host_hands_away) LOGI("net: the host's hands are %s (its scene)", away ? "away" : "out again");
  g_host_hands_away = away;
}

void scene_hands() {
  if (game::ref<uintptr_t>(game::kNetHost) < 2 || !g_cfg.net_scene_hands) return;
  uint8_t* own = controlled();
  if (!own) return;
  int me = game::ref<int>(game::kNetLocalPlayer);
  if (me == 0) {
    uint16_t body = *reinterpret_cast<uint16_t*>(own + game::kEntityId);
    uint8_t* hands = hands_of(own);
    if (hands && *reinterpret_cast<void**>(hands + kHandsEyesLink)) g_hands_seen_out = body;
    int away = g_hands_seen_out == body && own_hands_away(own) ? 1 : 0;
    int peers = link::peers();
    if (away == g_hands_said && peers == g_hands_said_peers) return;
    if (away != g_hands_said) LOGI("net: the own hands are %s here", away ? "away (a scene of the level script)" : "out");
    uint8_t note = static_cast<uint8_t>(away);
    if (peers > 0 && !link::send(link::kHands, &note, 1, true)) return;
    g_hands_said = away, g_hands_said_peers = peers;
    return;
  }
  if (me < 0) return;
  if (g_host_hands_away) {
    if (!own_hands_away(own) && put_hands_away(own, 1, false)) {
      if (!g_own_hands_put) LOGI("net: the own hands are put away for the host's scene");
      g_own_hands_put = true;
    }
  } else if (g_own_hands_put) {
    g_own_hands_put = false;
    uint8_t* inv = *reinterpret_cast<uint8_t**>(own + game::kActorInventory);
    int taken = 0;
    if (inv && *reinterpret_cast<uintptr_t*>(inv) == game::at(game::kPlayerInventory)) {
      using Select = int64_t (*)(uint8_t*, char, int64_t, int);
      Select select = (*reinterpret_cast<Select**>(inv))[game::kInventorySelect / 8];
      for (int slot : {16, 2, 1})
        if (select(inv, static_cast<char>(slot), 0, 0)) {
          taken = slot;
          break;
        }
    }
    LOGI("net: the host's scene is over: the own hands are out again (the weapon of slot %d into them)", taken);
  }
}

void lab_hands(int mode) {
  g_hands_mode = mode;
  for (uint16_t& d : g_hands_done) d = 0xFFFF;
  LOGI("lab: hands of the others: way %d", mode);
  hide_others_hands();
}

void tick() {
  hide_others_hands();
  scene_hands();
  keep_gear();
  if (game::ref<uintptr_t>(game::kNetHost) > 1 && game::ref<int>(game::kNetLocalPlayer) > 0) settle_actors();
  if (game::ref<uintptr_t>(game::kNetHost) > 1) keep_host_way();
  if (g_own_body != 0xFFFF) {
    ULONGLONG now = GetTickCount64();
    uint8_t* body = entity_by_id(g_own_body);
    if (!body || now - g_own_body_at > 15000) {
      g_own_body = 0xFFFF;
      g_gear_body = 0xFFFF;  // (never read the rounds of a body that was not handed over: they are the host's)
    } else if (now - g_own_body_at >= 300 && controlled() == body) {
      g_own_body = 0xFFFF;
      own_body_ready(body);
    }
  }
  if (g_pending_load.empty() || GetTickCount64() < g_pending_load_at) return;
  static char line[0x1000];  // the console works in a buffer of this size
  strncpy(line, g_pending_load.c_str(), sizeof(line) - 1);
  line[sizeof(line) - 1] = 0;
  g_pending_load.clear();
  game::ref<uint8_t>(game::kNetGame) = 0;
  g_host_reloaded = true;
  LOGI("net: loading now, the network game flag is off until the level runs again");
  g_console_line(line);
}

// --- the characters the engine does not send ---------------------------------------------------------------------
// The engine sends the characters (people, monsters) that are active near a player: where they are and what
// they do. A character the host's level script has put away - taken off a vehicle, moved elsewhere, put to
// sleep - is not in that list any more, and nothing tells the joined games: their copy stays as it was. (l03:
// the nosalis that kills the passenger rode the joined player's handcar for the rest of the fight, right in
// front of its gun, while on the host it lay asleep 50 m behind.) A joined game's own script does not do it
// either: it waits for the character's own events, and a character that another PC leads has none here.
// So the host says, for every character: what it hangs on, and - when the engine does not send it - where it
// stands. Said when it changes (and everything once to a PC that has just come); a joined game brings its
// copy in line: off its parent / on the same parent, to the same place.
#pragma pack(push, 1)
struct ActorNote {
  uint16_t id, parent;  // parent FFFF: none
  uint8_t bits;         // 1: the engine sends this one itself (its place is not the mod's business)
                        // 2: it waits for its scene (entity flag 0x20: does nothing, takes no damage, is not sent)
                        // 4: it is dead (where a body falls is each PC's own physics: its place is left alone)
  float x, y, z;
};
#pragma pack(pop)

struct ActorSaid {
  uint16_t parent;
  uint8_t bits;
  float x, y, z;
};
std::vector<ActorSaid> g_actors_said;      // the host: by entity id, what was said last (parent 0xFFFE: nothing yet)
uint64_t g_actors_said_at = 0;
int g_actors_peers = 0;

bool is_character(const uint8_t* e) {
  return (*reinterpret_cast<const uint32_t*>(e + game::kEntityFlags) & game::kFlagNetCandidate) &&
         *reinterpret_cast<const uintptr_t*>(e) != game::at(game::kPlayerClass);
}

void send_actors() {
  uint64_t now = GetTickCount64();
  if (now - g_actors_said_at < 250) return;
  g_actors_said_at = now;
  uint8_t* server = game::ref<uint8_t*>(game::kServer);
  int peers = link::peers();
  if (!server || !peers) {
    g_actors_peers = 0;
    return;
  }
  if (peers != g_actors_peers || g_actors_said.size() != 0x10000) {  // somebody came: everything is said anew
    g_actors_peers = peers;
    g_actors_said.assign(0x10000, ActorSaid{0xFFFE, 0, 0, 0, 0});
  }
  uint8_t* table = *reinterpret_cast<uint8_t**>(server + game::kServerEntities);
  unsigned count = *reinterpret_cast<uint16_t*>(server + game::kServerEntityCount);
  constexpr unsigned kMost = 64;
  uint8_t out[2 + kMost * sizeof(ActorNote)];
  uint16_t n = 0;
  for (unsigned i = 0; i < count && n < kMost; ++i) {
    uint8_t* e = *reinterpret_cast<uint8_t**>(table + i * 16 + 8);
    if (!e || !is_character(e)) continue;
    uint16_t id = *reinterpret_cast<uint16_t*>(e + game::kEntityId);
    if (id == 0xFFFF) continue;
    uint8_t* parent = parent_of(e);
    const float* at = reinterpret_cast<const float*>(e + game::kEntityPosition);
    ActorNote note = {id, parent ? *reinterpret_cast<uint16_t*>(parent + game::kEntityId) : static_cast<uint16_t>(0xFFFF),
                      static_cast<uint8_t>(((*reinterpret_cast<uint32_t*>(e + game::kEntityFlags) & game::kFlagNetworked) ? 1 : 0) |
                                           ((*reinterpret_cast<uint32_t*>(e + game::kEntityFlags) & game::kFlagWaiting) ? 2 : 0) |
                                           (*reinterpret_cast<float*>(e + game::kEntityHealth) > 0.f ? 0 : 4)),
                      at[0], at[1], at[2]};
    ActorSaid& said = g_actors_said[id];
    bool moved = false;
    if (!(note.bits & 5) && !parent) {
      float dx = note.x - said.x, dy = note.y - said.y, dz = note.z - said.z;
      moved = dx * dx + dy * dy + dz * dz > 0.5f * 0.5f;
    }
    if (said.parent == note.parent && said.bits == note.bits && !moved) continue;
    said = {note.parent, note.bits, note.x, note.y, note.z};
    memcpy(out + 2 + n * sizeof(note), &note, sizeof(note));
    ++n;
  }
  if (!n) return;
  memcpy(out, &n, 2);
  link::send(link::kActors, out, 2 + n * sizeof(ActorNote), true);
}

struct ActorWanted {
  ActorNote note;
  uint64_t tried_at = 0;
  int tries = 0;
};
std::vector<ActorWanted> g_actors_wanted;  // a joined game: what the host said and this world does not show yet
unsigned g_actors_fixed = 0;

void on_actors(int, const uint8_t* data, size_t size) {
  uint16_t n = 0;
  if (size < 2) return;
  memcpy(&n, data, 2);
  if (size < 2 + n * sizeof(ActorNote)) return;
  for (uint16_t i = 0; i < n; ++i) {
    ActorNote note;
    memcpy(&note, data + 2 + i * sizeof(note), sizeof(note));
    bool known = false;
    for (ActorWanted& w : g_actors_wanted)
      if (w.note.id == note.id) w.note = note, w.tries = 0, known = true;
    if (!known) g_actors_wanted.push_back({note, 0, 0});
  }
}

// About ten times a second: each thing the host said is looked at until this world shows it (an attach or a
// detach is done by the engine a frame later; a character that is not here yet may come).
void settle_actors() {
  if (g_actors_wanted.empty() || !controlled()) return;  // (while the own level loads nothing is touched)
  uint8_t* server = game::ref<uint8_t*>(game::kServer);
  if (!server) return;
  static std::vector<uint8_t*> by_id;
  by_id.assign(0x10000, nullptr);
  uint8_t* table = *reinterpret_cast<uint8_t**>(server + game::kServerEntities);
  unsigned count = *reinterpret_cast<uint16_t*>(server + game::kServerEntityCount);
  for (unsigned i = 0; i < count; ++i) {
    uint8_t* e = *reinterpret_cast<uint8_t**>(table + i * 16 + 8);
    if (e) by_id[*reinterpret_cast<uint16_t*>(e + game::kEntityId)] = e;
  }
  uint64_t now = GetTickCount64();
  size_t keep = 0;
  for (size_t i = 0; i < g_actors_wanted.size(); ++i) {
    ActorWanted w = g_actors_wanted[i];
    uint8_t* e = by_id[w.note.id];
    bool done = false;
    if (!e || !is_character(e)) {
      done = ++w.tries > 50;  // not here (yet): looked for five seconds
    } else {
      uint8_t* parent = parent_of(e);
      uint16_t has = parent ? *reinterpret_cast<uint16_t*>(parent + game::kEntityId) : static_cast<uint16_t>(0xFFFF);
      const uint8_t* name = *reinterpret_cast<uint8_t**>(e + game::kEntityName);
      const char* called = name ? reinterpret_cast<const char*>(name) + 0x14 : "";
      if (has != w.note.parent) {
        if (now - w.tried_at >= 500) {
          w.tried_at = now;
          uint8_t* wanted = w.note.parent == 0xFFFF ? nullptr : by_id[w.note.parent];
          if (w.note.parent != 0xFFFF && !wanted) {
            done = ++w.tries > 10;
          } else if (++w.tries > 6) {
            LOGW("net: '%s' (%X) hangs on %X here and on %X on the host: it did not change in six goes, left", called, w.note.id, has, w.note.parent);
            done = true;
          } else {
            if (wanted)
              hang_on(e, wanted);
            else
              come_off(e);
            if (g_actors_fixed++ < 40)
              LOGI("net: '%s' (%X) hangs on %X here and on %X on the host: %s", called, w.note.id, has, w.note.parent,
                   wanted ? "hung on it" : "taken off");
          }
        }
      } else if (!parent && ((*reinterpret_cast<uint32_t*>(e + game::kEntityFlags) & game::kFlagWaiting) != 0) != ((w.note.bits & 2) != 0)) {
        // the host's script has started this character's scene (or put it back to waiting): the same here, by the
        // call the level scripts make
        if (now - w.tried_at >= 500) {
          w.tried_at = now;
          if (++w.tries > 6) {
            LOGW("net: '%s' (%X) did not take the host's \"%s\" in six goes, left", called, w.note.id, (w.note.bits & 2) ? "wait" : "go");
            done = true;
          } else {
            reinterpret_cast<void* (*)(uint8_t*, char)>(game::at(game::kSetActive))(e, (w.note.bits & 2) ? 0 : 1);
            if (g_actors_fixed++ < 40)
              LOGI("net: '%s' (%X) %s on the host: the same here (flags %X now)", called, w.note.id,
                   (w.note.bits & 2) ? "waits for its scene again" : "has been set going", *reinterpret_cast<uint32_t*>(e + game::kEntityFlags));
          }
        }
      } else {
        if (!(w.note.bits & 5) && !parent && *reinterpret_cast<float*>(e + game::kEntityHealth) > 0.f) {
          const float* at = reinterpret_cast<const float*>(e + game::kEntityPosition);
          float dx = at[0] - w.note.x, dy = at[1] - w.note.y, dz = at[2] - w.note.z;
          float apart = sqrtf(dx * dx + dy * dy + dz * dz);
          if (apart > 0.75f) {
            float m[16];
            memcpy(m, e + game::kEntityMatrix, sizeof(m));
            m[12] = w.note.x, m[13] = w.note.y, m[14] = w.note.z;
            using SetMatrix = void (*)(uint8_t*, const float*, int);
            (*reinterpret_cast<SetMatrix**>(e))[game::kEntitySetMatrix / 8](e, m, 0);
            if (g_actors_fixed++ < 40)
              LOGI("net: '%s' (%X) stood %.1f m from where the host has it: put there (%.1f,%.1f,%.1f)", called, w.note.id, apart,
                   w.note.x, w.note.y, w.note.z);
          }
        }
        done = true;
      }
    }
    if (!done) g_actors_wanted[keep++] = w;
  }
  g_actors_wanted.resize(keep);
}

// Main thread, every turn of the message pump (at least once a frame).
void frame() {
  if (!g_main_thread) g_main_thread = GetCurrentThreadId();
  g_last_frame = GetTickCount64();
  link::poll();
  g_frame_seen.store(GetTickCount64());
  if (uint64_t at = g_quit_at.load(); at && GetTickCount64() >= at) {
    g_video.store(false);
    engine::command("quit", true);
  }
  if (game::ref<uintptr_t>(game::kNetHost) <= 1 || !game::ref<void*>(game::kLevel)) return;
  if (g_menu_unpause) {  // the pause menu opened in a session: its pause comes off, the menu stays
    uint8_t* loader = reinterpret_cast<uint8_t*>(game::at(game::kLoader));
    uint16_t n = *reinterpret_cast<uint16_t*>(loader + game::kPauseStack + 0xA);
    void** stack = *reinterpret_cast<void***>(loader + game::kPauseStack);
    for (int i = n - 1; i >= 0 && stack; --i) {
      const char* name = stack[i] ? static_cast<const char*>(stack[i]) + 0x14 : "";
      if (!strcmp(name, "ui_menu [default]")) {
        void* handle = stack[i];
        g_menu_unpause = false;
        LOGI("net: the pause menu of a session does not stop the world: its pause is taken off");
        reinterpret_cast<void (*)(void*, void**)>(game::at(game::kPausePop))(loader, &handle);
        break;
      }
    }
  }
  if (game::ref<int>(game::kNetLocalPlayer) == 0) {
    send_rails();
    if (g_cfg.net_actors) send_actors();
    if (g_cfg.net_ride) tell_host_seat();
  } else if (game::ref<int>(game::kNetLocalPlayer) > 0) {
    if (g_cfg.net_ride) follow_seat();
  }
}

bool host(const char* level) {
  uint8_t* body = controlled();
  if (!game::ref<void*>(game::kLevel) || !body) {
    LOGE("net: cannot host: no level or no player body (level %p, body %p)", game::ref<void*>(game::kLevel), body);
    return false;
  }
  log_state("before host");
  g_body_ids.reset();  // a new level: the bodies made on the last one are gone with its world
  for (auto& ids : g_slot_body) ids.clear();
  for (auto& ids : g_slot_removed) ids.clear();
  for (auto& ids : g_slot_old) ids.clear();
  Players& p = players();
  if (game::ref<uintptr_t>(game::kNetHost) > 1) {
    // The next level of a session that is already there (the story went on): the ENet host and the joined
    // players' slots stay, only the "network game" flag (the level change took it off) comes back.
    if (g_host_reloaded && level && *level) {
      using Tell = void (*)(void*, const char*);
      reinterpret_cast<Tell>(game::at(game::kTellLevel))(nullptr, level);
      LOGI("net: the level came from a save: the joined players are told to load %s again", level);
    }
    game::ref<uint8_t>(game::kNetGame) = 1;
    LOGI("net: the session goes on in the new level (players %u)", p.size);
  } else {
    // Net start does nothing while a host is there; a single-player game has the marker 1 in its place.
    if (game::ref<uintptr_t>(game::kNetHost) == 1) game::ref<uintptr_t>(game::kNetHost) = 0;
    game::ref<uint8_t>(game::kNetGame) = 1;
    using NetStart = void (*)(void*, const char*);
    g_hosting_now = true;
    g_port_taken = false;
    reinterpret_cast<NetStart>(game::at(game::kNetStart))(nullptr, nullptr);
    g_hosting_now = false;
    if (g_port_taken) LOGE("net: this game is hosted, but its UDP port is taken by another program: nobody can join it");
    // Net start adds a slot of its own after the single-player one: only slot 0 (this PC) stays, so the first
    // peer becomes player 1.
    if (p.size > 1) p.size = 1;
  }
  // In the same frame: with the flag set the game's "controlled entity" is empty until slot 0 has an actor.
  if (p.size > 0) {
    using LinkActor = void (*)(uint8_t*, uint16_t);
    reinterpret_cast<LinkActor>(game::at(game::kLinkActor))(p.array, entity_id(body));
    using ActorOf = void*** (*)(uint8_t*, int);
    void*** h = reinterpret_cast<ActorOf>(game::at(game::kActorOf))(p.array, 1);
    void* resolved = h && *h ? **h : nullptr;
    LOGI("net: slot 0 linked to entity %X: resolves to %p (the body's handle target %p)", entity_id(body), resolved,
         body + game::kEntityHandle);
  }
  g_host_reloaded = false;
  log_state("after host");
  return game::ref<uintptr_t>(game::kNetHost) > 1;
}

uint8_t* entity_by_id(uint16_t id);
uint8_t* parent_of(uint8_t* e);

struct BodyPart {
  uint16_t source, copy, parent;
};
std::vector<BodyPart> g_last_body;  // the lab: the clone list of the last body made here

void log_part(const char* what, uint16_t id, uint16_t parent) {
  uint8_t* e = entity_by_id(id);
  if (!e) {
    LOGI("net:   %s %4X: gone", what, id);
    return;
  }
  const uint8_t* name = *reinterpret_cast<uint8_t**>(e + game::kEntityName);
  const float* at = reinterpret_cast<const float*>(e + game::kEntityPosition);
  uint8_t* on = parent_of(e);
  LOGI("net:   %s %4X parent %4X hangs on %4X class +%llX flags %X '%s' at (%.2f,%.2f,%.2f)", what, id, parent,
       on ? *reinterpret_cast<uint16_t*>(on + game::kEntityId) : 0xFFFF,
       static_cast<unsigned long long>(*reinterpret_cast<uintptr_t*>(e) - game::at(0)),
       *reinterpret_cast<uint32_t*>(e + game::kEntityFlags), name ? reinterpret_cast<const char*>(name) + 0x14 : "", at[0],
       at[1], at[2]);
}

void log_body() {
  LOGI("net: the last body made here: %zu entities", g_last_body.size());
  for (const BodyPart& p : g_last_body) {
    log_part("own ", p.source, 0);
    log_part("copy", p.copy, p.parent);
  }
}

uintptr_t entity_field(unsigned id, unsigned offset) {
  uint8_t* e = id == 0xFFFF ? controlled() : entity_by_id(static_cast<uint16_t>(id));
  return e ? reinterpret_cast<uintptr_t>(e + offset) : 0;
}

void poke_entity(unsigned id, unsigned offset, unsigned size, unsigned value) {
  uint8_t* e = id == 0xFFFF ? controlled() : entity_by_id(static_cast<uint16_t>(id));
  if (!e || (size != 1 && size != 2 && size != 4) || offset > 0x4000) {
    LOGW("net: poke: no entity %X or a wrong size", id);
    return;
  }
  uint32_t old = 0;
  memcpy(&old, e + offset, size);
  memcpy(e + offset, &value, size);
  LOGI("net: entity %X + %X: %X -> %X (%u bytes)", id, offset, old, value, size);
}

std::vector<uint16_t> g_lab_leave_out;

void lab_leave_out(unsigned a, unsigned b) {
  if (a == 0xA) {  // "!without A 1": ammunition as 0.1.2 handled it (to compare)
    g_lab_old_ammo = b != 0;
    LOGI("net: ammunition of copied bodies the old way: %d", g_lab_old_ammo);
    return;
  }
  g_lab_leave_out.clear();
  if (a) g_lab_leave_out.push_back(static_cast<uint16_t>(a));
  if (b) g_lab_leave_out.push_back(static_cast<uint16_t>(b));
  LOGI("net: the next bodies are made without %X %X", a, b);
}

int add_empty_slot() {
  Players& p = players();
  if (game::ref<uintptr_t>(game::kNetHost) <= 1 || game::ref<int>(game::kNetLocalPlayer) != 0) return -1;
  using Reserve = int (*)(Players*, unsigned);
  using Ctor = void (*)(uint8_t*);
  if (p.capacity < p.size + 1u && !reinterpret_cast<Reserve>(game::at(game::kNetPlayersReserve))(&p, p.size + 1u)) return -1;
  reinterpret_cast<Ctor>(game::at(game::kNetPlayerCtor))(p.array + game::kNetPlayerSize * p.size);
  LOGI("net: an empty slot %u (nobody sits in it)", p.size);
  return p.size++;
}

bool spawn_body(int slot) {
  Players& p = players();
  uint8_t* body = controlled();
  void* server = game::ref<void*>(game::kServer);
  if (game::ref<int>(game::kNetLocalPlayer) != 0 || !body || !server || slot <= 0 || slot >= p.size) {
    LOGE("net: cannot spawn a body for slot %d (local slot %d, body %p, players %u)", slot,
         game::ref<int>(game::kNetLocalPlayer), body, p.size);
    return false;
  }
  send_way(slot);
  struct List {
    uint16_t* data;
    uint16_t capacity;
    uint16_t size;
  } list{};
  void* source = *reinterpret_cast<void**>(body + game::kEntityHandle);  // the entity's handle object
  void* none = nullptr;
  using CloneList = void (*)(void*, void**, List*, uint16_t);
  using SpawnEvent = void (*)(void*, uint8_t, List*, void**, void**, void**, int, int, int, uint8_t, int, void*, void*);
  reinterpret_cast<CloneList>(game::at(game::kCloneList))(server, &source, &list, 0xFFFF);
  uint16_t n = list.size;
  if (!n) {
    LOGE("net: the clone list of the player is empty");
    return false;
  }
  // Entries are {entity of the host's body, its copy's id, the copy's parent id}; the body itself is the last.
  // The lab can leave entities out of the copy ("!without"), with what hangs on them.
  std::vector<uint16_t> out_sources = g_lab_leave_out;
  if (!out_sources.empty()) {
    std::vector<uint16_t> out_copies;
    for (bool again = true; again;) {  // the parts of a weapon come before the weapon: run until nothing more goes
      again = false;
      for (uint16_t i = 0; i + 1 < n; ++i) {
        uint16_t src = list.data[i * 3], copy = list.data[i * 3 + 1], parent = list.data[i * 3 + 2];
        bool out = std::find(out_sources.begin(), out_sources.end(), src) != out_sources.end() ||
                   std::find(out_copies.begin(), out_copies.end(), parent) != out_copies.end();
        if (!out) continue;
        out_copies.push_back(copy);
        memmove(list.data + i * 3, list.data + (i + 1) * 3, (n - i - 1) * 3 * sizeof(uint16_t));
        --n;
        --i;
        again = true;
      }
    }
    LOGI("net: %zu entities left out of the body", out_copies.size());
    list.size = n;
  }
  uint16_t root = list.data[(n - 1) * 3 + 1];
  std::string ids;
  for (uint16_t i = 0; i < n && i < 40; ++i) {
    char one[24];
    snprintf(one, sizeof(one), " %X>%X", list.data[i * 3], list.data[i * 3 + 1]);
    ids += one;
  }
  LOGI("net: a body for slot %d: %u entities, root %X:%s", slot, n, root, ids.c_str());
  if (slot < 32) {
    g_body_made[slot] = root;
    g_body_made_at[slot] = GetTickCount64();
  }
  if (slot < 32) g_slot_body[slot].clear();
  g_last_body.clear();
  for (uint16_t i = 0; i < n; ++i) {
    g_last_body.push_back({list.data[i * 3], list.data[i * 3 + 1], list.data[i * 3 + 2]});
    g_body_ids.set(list.data[i * 3 + 1]);
    if (slot < 32) g_slot_body[slot].push_back(list.data[i * 3 + 1]);
  }
  reinterpret_cast<SpawnEvent>(game::at(game::kSpawnEvent))(nullptr, 0, &list, &source, &none, &none, 0, 1, slot, 0, 0,
                                                             nullptr, nullptr);
  log_state("after spawn");
  return true;
}

uint8_t* entity_by_id(uint16_t id) {
  uint8_t* server = game::ref<uint8_t*>(game::kServer);
  if (!server) return nullptr;
  uint8_t* table = *reinterpret_cast<uint8_t**>(server + game::kServerEntities);
  unsigned count = *reinterpret_cast<uint16_t*>(server + game::kServerEntityCount);
  for (unsigned i = 0; i < count; ++i) {
    uint8_t* e = *reinterpret_cast<uint8_t**>(table + i * 16 + 8);
    if (e && *reinterpret_cast<uint16_t*>(e + game::kEntityId) == id) return e;
  }
  return nullptr;
}

unsigned long lab_main_thread() { return g_main_thread; }
unsigned long long lab_last_frame() { return g_last_frame; }
std::string lab_tree_lock() {
  char text[96];
  snprintf(text, sizeof(text), "the collision tree's writer stood aside %u times, %u ms at the longest", g_tree_waits.load(),
           g_tree_longest_ms.load());
  return text;
}

std::string lab_updating() {
  const uint8_t* e = t_updating;
  if (!e) return "";
  char text[400];
  std::string who = lab_whose(reinterpret_cast<uintptr_t>(e));
  if (IsBadReadPtr(e, 0x1C0)) {
    snprintf(text, sizeof(text), "%p (not readable) %s", static_cast<const void*>(e), who.c_str());
    return text;
  }
  const uint8_t* name = *reinterpret_cast<uint8_t* const*>(e + game::kEntityName);
  // its parent: a handle at +280 (handle -> the parent's +0xA0), the parent's id at +288; children: a word at +346
  uintptr_t handle = *reinterpret_cast<const uintptr_t*>(e + 280);
  uintptr_t parent = handle && !IsBadReadPtr(reinterpret_cast<const void*>(handle), 8) ? *reinterpret_cast<const uintptr_t*>(handle) : 0;
  std::string parent_is = parent ? lab_whose(parent - 0xA0) : std::string("none");
  snprintf(text, sizeof(text), "%X '%.40s' flags %X at %p (%s), model %p, %u children, parent id %X (%s)",
           *reinterpret_cast<const uint16_t*>(e + game::kEntityId),
           name && !IsBadReadPtr(name, 0x40) ? reinterpret_cast<const char*>(name) + 0x14 : "?",
           *reinterpret_cast<const uint32_t*>(e + game::kEntityFlags), static_cast<const void*>(e), who.c_str(),
           *reinterpret_cast<void* const*>(e + 256), *reinterpret_cast<const uint16_t*>(e + 346),
           *reinterpret_cast<const uint16_t*>(e + 288), parent_is.c_str());
  return text;
}

// lab, crash log. Entities are big objects of unknown size: an address up to 0x2000 past an entity's start counts.
std::string lab_whose(uintptr_t address) {
  constexpr uintptr_t kSpan = 0x2000;
  char text[160];
  uint8_t* server = game::ref<uint8_t*>(game::kServer);
  if (server && !IsBadReadPtr(server + game::kServerEntities, 16)) {
    uint8_t* table = *reinterpret_cast<uint8_t**>(server + game::kServerEntities);
    unsigned count = *reinterpret_cast<uint16_t*>(server + game::kServerEntityCount);
    if (table && !IsBadReadPtr(table, count * 16)) {
      for (unsigned i = 0; i < count; ++i) {
        uintptr_t e = *reinterpret_cast<uintptr_t*>(table + i * 16 + 8);
        if (!e || address < e || address >= e + kSpan) continue;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(e);
        if (IsBadReadPtr(p, 0x1C0)) continue;
        const uint8_t* name = *reinterpret_cast<uint8_t* const*>(p + game::kEntityName);
        snprintf(text, sizeof(text), "in the world: %X '%.40s' flags %X, +%llX", *reinterpret_cast<const uint16_t*>(p + game::kEntityId),
                 name && !IsBadReadPtr(name, 0x40) ? reinterpret_cast<const char*>(name) + 0x14 : "?",
                 *reinterpret_cast<const uint32_t*>(p + game::kEntityFlags), static_cast<unsigned long long>(address - e));
        return text;
      }
    }
  }
  unsigned n = g_gone_count.load();
  ULONGLONG now = GetTickCount64();
  for (unsigned k = 0; k < 256 && k < n; ++k) {
    const GoneNote& g = g_gone_ring[(n - 1 - k) % 256];
    if (!g.at || address < g.at || address >= g.at + kSpan) continue;
    snprintf(text, sizeof(text), "TAKEN OUT %llu ms ago: %X '%s' flags %X, +%llX", now - g.when, g.id, g.name, g.flags,
             static_cast<unsigned long long>(address - g.at));
    return text;
  }
  return "";
}

// lab, crash log: who has a pointer to the object this address lies in (up to 0x600 into it)
std::string lab_held_by(uintptr_t address) {
  constexpr uintptr_t kInto = 0x600;
  if (address < 0x10000) return "";
  char text[200];
  uintptr_t best = kInto;
  text[0] = 0;
  uint8_t* server = game::ref<uint8_t*>(game::kServer);
  if (server && !IsBadReadPtr(server + game::kServerEntities, 16)) {
    uint8_t* table = *reinterpret_cast<uint8_t**>(server + game::kServerEntities);
    unsigned count = *reinterpret_cast<uint16_t*>(server + game::kServerEntityCount);
    if (table && !IsBadReadPtr(table, count * 16)) {
      for (unsigned i = 0; i < count; ++i) {
        const uint8_t* e = *reinterpret_cast<uint8_t**>(table + i * 16 + 8);
        if (!e) continue;
        for (unsigned part = 0; part < 12; ++part) {
          if (IsBadReadPtr(e + part * 512, 512)) break;
          const uintptr_t* w = reinterpret_cast<const uintptr_t*>(e + part * 512);
          for (unsigned k = 0; k < 64; ++k) {
            if (!w[k] || address < w[k] || address - w[k] >= best) continue;
            best = address - w[k];
            const uint8_t* name = *reinterpret_cast<uint8_t* const*>(e + game::kEntityName);
            snprintf(text, sizeof(text), "+%llX of what %X '%.40s' (in the world, flags %X) holds at its +%X",
                     static_cast<unsigned long long>(best), *reinterpret_cast<const uint16_t*>(e + game::kEntityId),
                     name && !IsBadReadPtr(name, 0x40) ? reinterpret_cast<const char*>(name) + 0x14 : "?",
                     *reinterpret_cast<const uint32_t*>(e + game::kEntityFlags), part * 512 + k * 8);
          }
        }
      }
    }
  }
  unsigned n = g_gone_count.load();
  ULONGLONG now = GetTickCount64();
  for (unsigned j = 0; j < 256 && j < n; ++j) {
    const GoneNote& g = g_gone_ring[(n - 1 - j) % 256];
    for (unsigned k = 0; k < g.words_n; ++k) {
      if (!g.words[k] || address < g.words[k] || address - g.words[k] >= best) continue;
      best = address - g.words[k];
      snprintf(text, sizeof(text), "+%llX of what %X '%s' (TAKEN OUT %llu ms ago, flags %X) held at its +%X",
               static_cast<unsigned long long>(best), g.id, g.name, now - g.when, g.flags, k * 8);
    }
  }
  return text;
}

void destroy_entities(const std::vector<uint16_t>& ids) {
  void* server = game::ref<void*>(game::kServer);
  if (!server) return;
  using Destroy = void (*)(void*, void**, int);
  Destroy destroy = reinterpret_cast<Destroy>(game::at(game::kDestroyEntity));
  // What the body carries first, the body itself last.
  for (uint16_t id : ids) {
    uint8_t* e = entity_by_id(id);
    if (!e) continue;
    void* handle = *reinterpret_cast<void**>(e + game::kEntityHandle);
    if (handle) destroy(server, &handle, 0);
  }
  for (uint16_t id : ids) g_body_ids.reset(id);
}

void refresh_item(unsigned id) {
  uint8_t* e = entity_by_id(static_cast<uint16_t>(id));
  if (!e) {
    LOGI("net: no entity %X", id);
    return;
  }
  uint32_t before = *reinterpret_cast<uint32_t*>(e + game::kEntityFlags);
  uint64_t bits = *reinterpret_cast<uint64_t*>(e + 0x90);
  using Refresh = void (*)(uint8_t*);
  (*reinterpret_cast<Refresh**>(e))[0xB88 / 8](e);
  LOGI("net: entity %X asked anew: flags %X -> %X, bits %llX -> %llX", id, before,
       *reinterpret_cast<uint32_t*>(e + game::kEntityFlags), static_cast<unsigned long long>(bits),
       static_cast<unsigned long long>(*reinterpret_cast<uint64_t*>(e + 0x90)));
}

// The entity an item hangs on: entity + 0x118 is a handle (its first field points at the parent's + 0xA0).
uint8_t* parent_of(uint8_t* e) {
  uint8_t** handle = *reinterpret_cast<uint8_t***>(e + 0x118);
  return handle && *handle ? *handle - game::kEntityHandle : nullptr;
}

void log_children_of(uint16_t id, int depth) {
  uint8_t* server = game::ref<uint8_t*>(game::kServer);
  uint8_t* parent = entity_by_id(id);
  if (!server || !parent || depth > 3) return;
  uint8_t* table = *reinterpret_cast<uint8_t**>(server + game::kServerEntities);
  unsigned count = *reinterpret_cast<uint16_t*>(server + game::kServerEntityCount);
  for (unsigned i = 0; i < count; ++i) {
    uint8_t* e = *reinterpret_cast<uint8_t**>(table + i * 16 + 8);
    if (!e || e == parent || parent_of(e) != parent) continue;
    uint16_t own = *reinterpret_cast<uint16_t*>(e + game::kEntityId);
    const uint8_t* name = *reinterpret_cast<uint8_t**>(e + game::kEntityName);
    LOGI("net:   %*s%4X class +%llX flags %X pid %X '%s'", depth * 2, "", own,
         static_cast<unsigned long long>(*reinterpret_cast<uintptr_t*>(e) - game::at(0)),
         *reinterpret_cast<uint32_t*>(e + game::kEntityFlags), *reinterpret_cast<uint16_t*>(e + 0x120),
         name ? reinterpret_cast<const char*>(name) + 0x14 : "");
    if (own != id) log_children_of(own, depth + 1);
  }
}

void log_children(unsigned id) {
  if (id == 0xFFFF) {
    uint8_t* body = controlled();
    if (!body) return;
    id = *reinterpret_cast<uint16_t*>(body + game::kEntityId);
  }
  LOGI("net: what hangs on entity %X:", id);
  log_children_of(static_cast<uint16_t>(id), 0);
}

// Hangs an entity on a parent the way the game does it (the parent's inventory takes it).
bool attach(uint8_t* e, uint8_t* parent) {
  if (!e || !parent) return false;
  void* handle = *reinterpret_cast<void**>(parent + game::kEntityHandle);
  if (!handle) return false;
  void* out = nullptr;
  uint8_t flags = 0;
  using Attach = void* (*)(uint8_t*, void**, void**, uint8_t*);
  reinterpret_cast<Attach>(game::at(game::kAttach))(e, &out, &handle, &flags);
  return parent_of(e) == parent;
}

void lab_attach(unsigned id, unsigned parent) {
  uint8_t* e = entity_by_id(static_cast<uint16_t>(id));
  uint8_t* p = entity_by_id(static_cast<uint16_t>(parent));
  uint32_t before = e ? *reinterpret_cast<uint32_t*>(e + game::kEntityFlags) : 0;
  bool ok = attach(e, p);
  LOGI("net: entity %X on %X: %s, flags %X -> %X", id, parent, ok ? "hangs" : "does not hang", before,
       e ? *reinterpret_cast<uint32_t*>(e + game::kEntityFlags) : 0);
}

void look_entity(unsigned id, unsigned offset, unsigned count) {
  uint8_t* e = id == 0xFFFF ? controlled() : entity_by_id(static_cast<uint16_t>(id));
  if (!e || offset > 0x4000 || count > 64) return;
  for (unsigned i = 0; i < count; ++i) {
    uint64_t v = *reinterpret_cast<uint64_t*>(e + offset + i * 8);
    uint64_t first = 0;
    if (v > 0x10000 && !IsBadReadPtr(reinterpret_cast<void*>(v), 8)) first = *reinterpret_cast<uint64_t*>(v);
    if (first >= game::at(0) && first < game::at(0) + game::kSizeOfImage)
      LOGI("net: entity %X + %X: %016llX -> an object of class +%llX", id, offset + i * 8, static_cast<unsigned long long>(v),
           static_cast<unsigned long long>(first - game::at(0)));
    else
      LOGI("net: entity %X + %X: %016llX", id, offset + i * 8, static_cast<unsigned long long>(v));
  }
}

std::vector<uint8_t> g_snap;
unsigned g_snap_id = 0;
constexpr size_t kSnapSize = 0x2800;

void snap_entity(unsigned id) {
  uint8_t* e = id == 0xFFFF ? controlled() : entity_by_id(static_cast<uint16_t>(id));
  if (!e || IsBadReadPtr(e, kSnapSize)) {
    LOGI("net: snap: no entity %X", id);
    return;
  }
  g_snap.assign(e, e + kSnapSize);
  g_snap_id = id;
  LOGI("net: entity %X remembered", id);
}

void diff_entity(unsigned id) {
  uint8_t* e = id == 0xFFFF ? controlled() : entity_by_id(static_cast<uint16_t>(id));
  if (!e || g_snap.size() != kSnapSize || g_snap_id != id || IsBadReadPtr(e, kSnapSize)) {
    LOGI("net: diff: no entity %X or nothing remembered", id);
    return;
  }
  std::string out;
  int n = 0;
  for (size_t o = 0; o + 4 <= kSnapSize && n < 40; o += 4) {
    uint32_t a, b;
    memcpy(&a, g_snap.data() + o, 4);
    memcpy(&b, e + o, 4);
    if (a == b || a > 100000 || b > 100000) continue;  // counters, not pointers / floats
    char one[48];
    snprintf(one, sizeof(one), " +%zX: %u -> %u;", o, a, b);
    out += one;
    ++n;
  }
  LOGI("net: entity %X changed:%s", id, out.empty() ? " nothing small" : out.c_str());
  out.clear();
  n = 0;
  for (size_t o = 0; o + 4 <= kSnapSize && n < 60; o += 4) {  // angles and unit vectors
    float a, b;
    memcpy(&a, g_snap.data() + o, 4);
    memcpy(&b, e + o, 4);
    if (!(fabsf(a) <= 7.f) || !(fabsf(b) <= 7.f) || fabsf(a - b) < 0.02f || (fabsf(a) < 1e-6f && fabsf(b) < 1e-6f)) continue;
    char one[56];
    snprintf(one, sizeof(one), " +%zX: %.3f -> %.3f;", o, a, b);
    out += one;
    ++n;
  }
  LOGI("net: entity %X changed (floats):%s", id, out.empty() ? " nothing" : out.c_str());
}

void log_inventory(unsigned id) {
  uint8_t* body = id == 0xFFFF ? controlled() : entity_by_id(static_cast<uint16_t>(id));
  uint8_t* inv = body ? *reinterpret_cast<uint8_t**>(body + 0xB70) : nullptr;
  uint8_t* server = game::ref<uint8_t*>(game::kServer);
  if (!inv || !server) {
    LOGI("net: no inventory for %X", id);
    return;
  }
  uint8_t** items = *reinterpret_cast<uint8_t***>(inv + 0xF0);
  unsigned n = *reinterpret_cast<uint16_t*>(inv + 0xFA);
  LOGI("net: the inventory of %X (class +%llX) holds %u items:", body ? *reinterpret_cast<uint16_t*>(body + game::kEntityId) : 0,
       static_cast<unsigned long long>(*reinterpret_cast<uintptr_t*>(inv) - game::at(0)), n);
  uint8_t* table = *reinterpret_cast<uint8_t**>(server + game::kServerEntities);
  unsigned count = *reinterpret_cast<uint16_t*>(server + game::kServerEntityCount);
  for (unsigned k = 0; k < n && k < 64 && items; ++k) {
    uint8_t* a = items[k];
    uint8_t* owner = nullptr;
    for (unsigned i = 0; i < count && a; ++i) {
      uint8_t* e = *reinterpret_cast<uint8_t**>(table + i * 16 + 8);
      if (e && a >= e && a < e + 0x1000 && (!owner || e > owner)) owner = e;
    }
    const uint8_t* name = owner ? *reinterpret_cast<uint8_t**>(owner + game::kEntityName) : nullptr;
    LOGI("net:   item %2u: %X '%s' (entity + %llX), slot %u, hangs on %X", k,
         owner ? *reinterpret_cast<uint16_t*>(owner + game::kEntityId) : 0xFFFF,
         name ? reinterpret_cast<const char*>(name) + 0x14 : "?", static_cast<unsigned long long>(owner ? a - owner : 0),
         a ? a[game::kItemSlot] : 255, owner && parent_of(owner) ? *reinterpret_cast<uint16_t*>(parent_of(owner) + game::kEntityId) : 0xFFFF);
  }
}

void lab_select_slot(unsigned slot) {
  uint8_t* body = controlled();
  uint8_t* inv = body ? *reinterpret_cast<uint8_t**>(body + game::kActorInventory) : nullptr;
  if (!inv) return;
  using Select = int64_t (*)(uint8_t*, char, int64_t, int);
  int64_t r = (*reinterpret_cast<Select**>(inv))[0x40 / 8](inv, static_cast<char>(slot), 0, 0);
  LOGI("net: slot %u asked of the inventory (class +%llX): answer %lld, wanted weapon %u", slot,
       static_cast<unsigned long long>(*reinterpret_cast<uintptr_t*>(inv) - game::at(0)), static_cast<long long>(r),
       *reinterpret_cast<uint16_t*>(inv + 0x6B0));
}

bool aim_error(unsigned id, float* yaw, float* distance) {
  uint8_t* me = controlled();
  uint8_t* it = entity_by_id(static_cast<uint16_t>(id));
  if (!me || !it || me == it) return false;
  const float* forward = reinterpret_cast<const float*>(me + 0xE0);  // the third row of the 4x4 matrix at + 0xC0
  const float* a = reinterpret_cast<const float*>(me + game::kEntityPosition);
  const float* b = reinterpret_cast<const float*>(it + game::kEntityPosition);
  float dx = b[0] - a[0], dz = b[2] - a[2];
  float d = sqrtf(dx * dx + dz * dz);
  if (d < 0.01f) return false;
  float cross = forward[0] * dz - forward[2] * dx, dot = forward[0] * dx + forward[2] * dz;
  *yaw = atan2f(cross, dot);
  *distance = d;
  return true;
}

void lab_inventory_action(unsigned action) {
  uint8_t* body = controlled();
  uint8_t* inv = body ? *reinterpret_cast<uint8_t**>(body + game::kActorInventory) : nullptr;
  if (!inv || *reinterpret_cast<uintptr_t*>(inv) != game::at(game::kPlayerInventory)) return;
  using Action = char (*)(uint8_t*, int, char);
  Action act = reinterpret_cast<Action>(game::at(game::kInventoryAction));
  int down = act(inv, static_cast<int>(action), 1);
  int up = act(inv, static_cast<int>(action), 0);
  LOGI("net: inventory action %u: pressed -> %d, let go -> %d, wanted weapon %u", action, down, up,
       *reinterpret_cast<uint16_t*>(inv + 0x6B0));
}

void log_near(unsigned metres) {
  uint8_t* me = controlled();
  uint8_t* server = game::ref<uint8_t*>(game::kServer);
  if (!me || !server) return;
  const float* a = reinterpret_cast<const float*>(me + game::kEntityPosition);
  uint8_t* table = *reinterpret_cast<uint8_t**>(server + game::kServerEntities);
  unsigned count = *reinterpret_cast<uint16_t*>(server + game::kServerEntityCount);
  unsigned shown = 0;
  using AsActor = uint8_t* (*)(uint8_t*);
  for (unsigned i = 0; i < count && shown < 60; ++i) {
    uint8_t* e = *reinterpret_cast<uint8_t**>(table + i * 16 + 8);
    if (!e || e == me) continue;
    if (!(*reinterpret_cast<AsActor**>(e))[0x700 / 8](e)) continue;  // not an actor (a player, a human, a monster)
    const float* b = reinterpret_cast<const float*>(e + game::kEntityPosition);
    float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    float d = sqrtf(dx * dx + dy * dy + dz * dz);
    if (d > static_cast<float>(metres)) continue;
    const uint8_t* name = *reinterpret_cast<uint8_t**>(e + game::kEntityName);
    LOGI("net:   near %4X class +%llX health %.2f at %.1f m (%.1f,%.1f,%.1f) flags %X '%s'", *reinterpret_cast<uint16_t*>(e + game::kEntityId),
         static_cast<unsigned long long>(*reinterpret_cast<uintptr_t*>(e) - game::at(0)), *reinterpret_cast<float*>(e + game::kEntityHealth),
         d, b[0], b[1], b[2], *reinterpret_cast<uint32_t*>(e + game::kEntityFlags), name ? reinterpret_cast<const char*>(name) + 0x14 : "");
    ++shown;
  }
  LOGI("net: %u actors within %u m", shown, metres);
}

// --- riding ------------------------------------------------------------------------------------------------------
uint8_t* mover_of(uint8_t* actor) {
  uint8_t* mover = *reinterpret_cast<uint8_t**>(actor + game::kActorMover);
  if (!mover) {
    reinterpret_cast<void (*)(uint8_t*)>(game::at(game::kMoverMake))(actor);
    mover = *reinterpret_cast<uint8_t**>(actor + game::kActorMover);
  }
  return mover;
}

// Puts a character on a moving thing the way the level script does. True when the thing is its parent afterwards.
bool ride(uint8_t* body, uint8_t* platform) {
  uint8_t* mover = mover_of(body);
  void* handle = *reinterpret_cast<void**>(platform + game::kEntityHandle);
  if (!mover || !handle) return false;
  reinterpret_cast<void (*)(uint8_t*, void**)>(game::at(game::kPlatformAttach))(mover, &handle);
  return parent_of(body) == platform;
}

void leave_platform(uint8_t* body) {
  uint8_t* mover = *reinterpret_cast<uint8_t**>(body + game::kActorMover);
  if (mover) reinterpret_cast<void (*)(uint8_t*)>(game::at(game::kPlatformDetach))(mover);
}

void lab_ride(unsigned id, unsigned platform) {
  uint8_t* body = id == 0xFFFF ? controlled() : entity_by_id(static_cast<uint16_t>(id));
  uint8_t* p = platform == 0xFFFF ? nullptr : entity_by_id(static_cast<uint16_t>(platform));
  if (!body || (platform != 0xFFFF && !p)) {
    LOGI("net: no entity %X / %X to ride", id, platform);
    return;
  }
  const float* at = reinterpret_cast<const float*>(body + game::kEntityPosition);
  if (p) {
    bool ok = ride(body, p);
    LOGI("net: entity %X put on %X: %s, at (%.2f,%.2f,%.2f)", id, platform, ok ? "rides" : "NOT taken", at[0], at[1], at[2]);
  } else {
    leave_platform(body);
    uint8_t* parent = parent_of(body);
    LOGI("net: entity %X taken off: hangs on %X now", id, parent ? *reinterpret_cast<uint16_t*>(parent + game::kEntityId) : 0xFFFF);
  }
}

void hang_on(uint8_t* child, uint8_t* parent);
void come_off(uint8_t* child);

// "!mem <id> <offset of a pointer in the entity> <count>": the object that pointer leads to, as 32-bit words
// with their value as a number (finding a field by watching it change).
void look_object(unsigned id, unsigned offset, unsigned count) {
  uint8_t* e = id == 0xFFFF ? controlled() : entity_by_id(static_cast<uint16_t>(id));
  if (!e) {
    LOGI("net: no entity %X", id);
    return;
  }
  uint8_t* o = *reinterpret_cast<uint8_t**>(e + offset);
  if (!o || IsBadReadPtr(o, count * 4)) {
    LOGI("net: entity %X + %X points nowhere (%p)", id, offset, o);
    return;
  }
  std::string out;
  for (unsigned i = 0; i < count; ++i) {
    uint32_t w;
    float f;
    memcpy(&w, o + i * 4, 4);
    memcpy(&f, o + i * 4, 4);
    char one[64];
    if (w == 0)
      snprintf(one, sizeof(one), " +%X:0", i * 4);
    else if (fabsf(f) > 1e-4f && fabsf(f) < 1e7f)
      snprintf(one, sizeof(one), " +%X:%.3f", i * 4, f);
    else
      snprintf(one, sizeof(one), " +%X:%X", i * 4, w);
    out += one;
  }
  LOGI("net: entity %X + %X -> %p (class +%llX):%s", id, offset, o,
       static_cast<unsigned long long>(*reinterpret_cast<uintptr_t*>(o) - game::at(0)), out.c_str());
}

// The lab: `count` u32 words of the object an entity's field points at, from `at` on.
void look_deep(unsigned id, unsigned offset, unsigned at, unsigned count) {
  uint8_t* e = id == 0xFFFF ? controlled() : entity_by_id(static_cast<uint16_t>(id));
  uint8_t* o = e ? *reinterpret_cast<uint8_t**>(e + offset) : nullptr;
  if (!o || IsBadReadPtr(o + at, count * 4)) {
    LOGI("net: entity %X + %X: nothing there", id, offset);
    return;
  }
  std::string out;
  for (unsigned i = 0; i < count && i < 64; ++i) {
    char one[32];
    snprintf(one, sizeof(one), " +%X:%u", at + i * 4, *reinterpret_cast<uint32_t*>(o + at + i * 4));
    out += one;
  }
  LOGI("net: entity %X + %X -> (class +%llX):%s", id, offset, static_cast<unsigned long long>(*reinterpret_cast<uintptr_t*>(o) - game::at(0)), out.c_str());
}

void lab_hang(unsigned id, unsigned parent) {
  uint8_t* e = id == 0xFFFF ? controlled() : entity_by_id(static_cast<uint16_t>(id));
  uint8_t* p = parent == 0xFFFF ? nullptr : entity_by_id(static_cast<uint16_t>(parent));
  if (!e || (parent != 0xFFFF && !p)) {
    LOGI("net: no entity %X / %X to hang", id, parent);
    return;
  }
  if (p)
    hang_on(e, p);
  else
    come_off(e);
  uint8_t* now = parent_of(e);
  LOGI("net: entity %X %s %X: hangs on %X now", id, p ? "hung on" : "taken off", parent,
       now ? *reinterpret_cast<uint16_t*>(now + game::kEntityId) : 0xFFFF);
}

// "!bring <class> [metres]": the nearest living entity of a class (a monster, also one that sleeps far away) is put
// so many metres in front of this PC's body and woken the way the engine wakes one - a fight where the lab needs it.
void lab_bring(unsigned class_rva, float metres) {
  uint8_t* me = controlled();
  uint8_t* server = game::ref<uint8_t*>(game::kServer);
  if (!me || !server) return;
  const float* a = reinterpret_cast<const float*>(me + game::kEntityMatrix);
  uint8_t* table = *reinterpret_cast<uint8_t**>(server + game::kServerEntities);
  unsigned count = *reinterpret_cast<uint16_t*>(server + game::kServerEntityCount);
  uint8_t* best = nullptr;
  float best_d = 1e30f;
  for (unsigned i = 0; i < count; ++i) {
    uint8_t* e = *reinterpret_cast<uint8_t**>(table + i * 16 + 8);
    if (!e || e == me || *reinterpret_cast<uintptr_t*>(e) != game::at(class_rva)) continue;
    if (!(*reinterpret_cast<float*>(e + game::kEntityHealth) > 0.f) || parent_of(e)) continue;
    const float* b = reinterpret_cast<const float*>(e + game::kEntityPosition);
    float dx = b[0] - a[12], dy = b[1] - a[13], dz = b[2] - a[14];
    float d = dx * dx + dy * dy + dz * dz;
    if (d < best_d) best_d = d, best = e;
  }
  if (!best) {
    LOGI("net: nothing of class +%X to bring", class_rva);
    return;
  }
  uint32_t before = *reinterpret_cast<uint32_t*>(best + game::kEntityFlags);
  float m[16];
  memcpy(m, best + game::kEntityMatrix, sizeof(m));
  float fx = a[8], fz = a[10];
  float len = sqrtf(fx * fx + fz * fz);
  if (len < 0.01f) fx = 0, fz = 1, len = 1;
  m[12] = a[12] + fx / len * metres, m[13] = a[13] + 0.1f, m[14] = a[14] + fz / len * metres;
  using SetMatrix = void (*)(uint8_t*, const float*, int);
  (*reinterpret_cast<SetMatrix**>(best))[game::kEntitySetMatrix / 8](best, m, 0);
  reinterpret_cast<int (*)(uint8_t*)>(game::at(0x6AC950))(best);  // the engine's "wake": flags & ~0xE000, back into the scene
  // the level script's "make it active" (a monster waiting for its scene has flag 0x20: it does nothing and takes no damage)
  reinterpret_cast<void* (*)(uint8_t*, char)>(game::at(game::kSetActive))(best, 1);
  const uint8_t* name = *reinterpret_cast<uint8_t**>(best + game::kEntityName);
  LOGI("net: brought %X '%s' from %.0f m to (%.1f,%.1f,%.1f), flags %X -> %X", *reinterpret_cast<uint16_t*>(best + game::kEntityId),
       name ? reinterpret_cast<const char*>(name) + 0x14 : "", sqrtf(best_d), m[12], m[13], m[14], before,
       *reinterpret_cast<uint32_t*>(best + game::kEntityFlags));
}

// "!world <n>": every entity of the world into world-<n>.txt next to the exe (id, class, flags, health of an actor,
// position, what it hangs on, name) - two PCs' files are compared by tools/lab/world.py.
void dump_world(unsigned n) {
  uint8_t* server = game::ref<uint8_t*>(game::kServer);
  if (!server) return;
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring file(path);
  file = file.substr(0, file.find_last_of(L'\\')) + L"\\world-" + std::to_wstring(n) + L".txt";
  FILE* f = _wfopen(file.c_str(), L"wb");
  if (!f) return;
  uint8_t* table = *reinterpret_cast<uint8_t**>(server + game::kServerEntities);
  unsigned count = *reinterpret_cast<uint16_t*>(server + game::kServerEntityCount);
  unsigned written = 0;
  using AsActor = uint8_t* (*)(uint8_t*);
  for (unsigned i = 0; i < count; ++i) {
    uint8_t* e = *reinterpret_cast<uint8_t**>(table + i * 16 + 8);
    if (!e) continue;
    bool actor = (*reinterpret_cast<AsActor**>(e))[0x700 / 8](e) != nullptr;
    const float* at = reinterpret_cast<const float*>(e + game::kEntityPosition);
    const float* fwd = reinterpret_cast<const float*>(e + 0xE0);
    uint8_t* parent = parent_of(e);
    const uint8_t* name = *reinterpret_cast<uint8_t**>(e + game::kEntityName);
    fprintf(f, "%X\t%llX\t%X\t%s\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%X\t%s\n", *reinterpret_cast<uint16_t*>(e + game::kEntityId),
            static_cast<unsigned long long>(*reinterpret_cast<uintptr_t*>(e) - game::at(0)),
            *reinterpret_cast<uint32_t*>(e + game::kEntityFlags),
            actor ? std::to_string(*reinterpret_cast<float*>(e + game::kEntityHealth)).c_str() : "-", at[0], at[1], at[2],
            fwd[0], fwd[2], parent ? *reinterpret_cast<uint16_t*>(parent + game::kEntityId) : 0xFFFF,
            name ? reinterpret_cast<const char*>(name) + 0x14 : "");
    ++written;
  }
  fclose(f);
  LOGI("net: world %u written: %u entities", n, written);
}

void move_entity(unsigned id, float x, float y, float z) {
  uint8_t* e = id == 0xFFFF ? controlled() : entity_by_id(static_cast<uint16_t>(id));
  if (!e) {
    LOGI("net: no entity %X to move", id);
    return;
  }
  float m[16];
  memcpy(m, e + game::kEntityMatrix, sizeof(m));
  float was[3] = {m[12], m[13], m[14]};
  m[12] = x, m[13] = y, m[14] = z;
  using SetMatrix = void (*)(uint8_t*, const float*, int);
  (*reinterpret_cast<SetMatrix**>(e))[game::kEntitySetMatrix / 8](e, m, 0);
  const float* at = reinterpret_cast<const float*>(e + game::kEntityPosition);
  LOGI("net: entity %X moved from (%.2f,%.2f,%.2f) to (%.2f,%.2f,%.2f)", id, was[0], was[1], was[2], at[0], at[1], at[2]);
}

// The lab: entity `id` stands `metres` in front of the body this PC controls, facing it (turned half round).
void place_in_front(unsigned id, float metres) {
  uint8_t* me = controlled();
  uint8_t* e = entity_by_id(static_cast<uint16_t>(id));
  if (!me || !e || me == e) {
    LOGI("net: nothing to put in front (%X)", id);
    return;
  }
  const float* mm = reinterpret_cast<const float*>(me + game::kEntityMatrix);
  float fx = mm[8], fz = mm[10];
  float len = sqrtf(fx * fx + fz * fz);
  if (len < 1e-3f) return;
  fx /= len, fz /= len;
  float m[16];
  memcpy(m, mm, sizeof(m));
  for (int k : {0, 1, 2, 8, 9, 10}) m[k] = -m[k];
  m[12] = mm[12] + fx * metres, m[14] = mm[14] + fz * metres;
  using SetMatrix = void (*)(uint8_t*, const float*, int);
  (*reinterpret_cast<SetMatrix**>(e))[game::kEntitySetMatrix / 8](e, m, 0);
  LOGI("net: entity %X stands %.2f m in front of the own body, facing it", id, metres);
}

bool aim_pitch_error(unsigned id, float height, float* pitch) {
  uint8_t* me = controlled();
  uint8_t* it = entity_by_id(static_cast<uint16_t>(id));
  if (!me || !it || me == it) return false;
  const float* a = reinterpret_cast<const float*>(me + game::kEntityPosition);
  const float* b = reinterpret_cast<const float*>(it + game::kEntityPosition);
  float dx = b[0] - a[0], dz = b[2] - a[2];
  float flat = sqrtf(dx * dx + dz * dz);
  if (flat < 0.05f) return false;
  const float eye = 1.7f;  // the eyes above the body's feet
  float wanted = atan2f((a[1] + eye) - (b[1] + height), flat);
  *pitch = wanted - *reinterpret_cast<const float*>(me + game::kPlayerPitch);
  return true;
}

void lab_hit(unsigned attacker, unsigned victim) {
  if (game::ref<int>(game::kNetLocalPlayer) != 0 || game::ref<uintptr_t>(game::kNetHost) <= 1) return;
  // the game's heap, as its own code takes the 14 bytes of an event (it frees them itself)
  using Heap = uint8_t* (*)();
  using Alloc = uint16_t* (*)(void*, unsigned);
  uint8_t* heap = reinterpret_cast<Heap>(game::at(0x63C7D0))();
  ++*reinterpret_cast<int*>(heap + 256);
  EnterCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(heap));
  uint16_t* event = reinterpret_cast<Alloc>(game::at(0x63E9A0))(*reinterpret_cast<void**>(heap + 56), 14);
  LeaveCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(heap));
  if (!event) return;
  event[0] = 1;  // kind 1: a hit
  event[1] = static_cast<uint16_t>(attacker);
  event[2] = static_cast<uint16_t>(victim);
  event[3] = 0x3F81, event[4] = 0x054E, event[5] = 0x0439, event[6] = 0x86CC;  // a real AK hit at 1 m (the trace)
  using Receive = void (*)(void*, uint16_t*, int);
  reinterpret_cast<Receive>(game::at(0x68E600))(reinterpret_cast<void*>(game::at(game::kNetHost)), event, 0);
  LOGI("net: a hit event given to the game: %X hits %X", attacker, victim);
}

unsigned nearest_of_class(unsigned class_rva, float metres, float* health, const char* name_part) {
  uint8_t* me = controlled();
  uint8_t* server = game::ref<uint8_t*>(game::kServer);
  if (!me || !server) return 0xFFFF;
  const float* a = reinterpret_cast<const float*>(me + game::kEntityPosition);
  uint8_t* table = *reinterpret_cast<uint8_t**>(server + game::kServerEntities);
  unsigned count = *reinterpret_cast<uint16_t*>(server + game::kServerEntityCount);
  unsigned best = 0xFFFF;
  float best_d = metres;
  for (unsigned i = 0; i < count; ++i) {
    uint8_t* e = *reinterpret_cast<uint8_t**>(table + i * 16 + 8);
    if (!e || e == me || *reinterpret_cast<uintptr_t*>(e) != game::at(class_rva)) continue;
    float h = *reinterpret_cast<float*>(e + game::kEntityHealth);
    if (!(h > 0.f)) continue;
    if (name_part && *name_part) {
      const uint8_t* name = *reinterpret_cast<uint8_t**>(e + game::kEntityName);
      if (!name || !strstr(reinterpret_cast<const char*>(name) + 0x14, name_part)) continue;
    }
    const float* b = reinterpret_cast<const float*>(e + game::kEntityPosition);
    float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    float d = sqrtf(dx * dx + dy * dy + dz * dz);
    if (d < best_d) {
      best_d = d;
      best = *reinterpret_cast<uint16_t*>(e + game::kEntityId);
      if (health) *health = h;
    }
  }
  return best;
}

void destroy_entity(unsigned id) {
  bool there = entity_by_id(static_cast<uint16_t>(id)) != nullptr;
  if (there && game::ref<int>(game::kNetLocalPlayer) == 0) destroy_entities({static_cast<uint16_t>(id)});
  LOGI("net: entity %X %s", id, !there ? "is not there" : game::ref<int>(game::kNetLocalPlayer) == 0 ? "taken out of the world" : "is the host's to take out");
}

bool remove_body(int slot) {
  if (slot <= 0 || slot >= 32 || g_slot_body[slot].empty() || !game::ref<void*>(game::kServer) ||
      game::ref<int>(game::kNetLocalPlayer) != 0)
    return false;
  std::vector<uint16_t> ids;
  ids.swap(g_slot_body[slot]);
  unsigned present = 0;
  for (uint16_t id : ids) present += entity_by_id(id) != nullptr;
  LOGI("net: taking the body of slot %d out of the world: entity %X, %u of its %zu entities are there", slot, ids.back(),
       present, ids.size());
  destroy_entities(ids);
  g_slot_removed[slot] = ids;
  // The slot forgets the body: a player who comes into it later gets a new one.
  Players& p = players();
  if (slot < p.size) *reinterpret_cast<uint16_t*>(p.array + game::kNetPlayerSize * slot + 0x64) = 0xFFFF;
  return true;
}

bool respawn_body(int slot) {
  if (slot <= 0 || slot >= 32 || g_slot_body[slot].empty()) return false;
  std::vector<uint16_t> old;
  old.swap(g_slot_body[slot]);
  if (!spawn_body(slot)) {
    g_slot_body[slot].swap(old);
    return false;
  }
  g_slot_old[slot] = old;
  return true;
}

void remove_old_body(int slot) {
  if (slot <= 0 || slot >= 32 || g_slot_old[slot].empty()) return;
  std::vector<uint16_t> ids;
  ids.swap(g_slot_old[slot]);
  LOGI("net: taking the old body of slot %d out of the world: entity %X", slot, ids.back());
  destroy_entities(ids);
  g_slot_removed[slot] = ids;
}

int removed_body_left(int slot) {
  if (slot <= 0 || slot >= 32 || g_slot_removed[slot].empty()) return -1;
  int left = 0;
  for (uint16_t id : g_slot_removed[slot]) left += entity_by_id(id) != nullptr;
  return left;
}

void finish_load() {
  using Finish = void (*)(void*);
  LOGI("net: finish load (map state %d)", game::ref<int>(game::kMapState));
  reinterpret_cast<Finish>(game::at(game::kLoaderFinish))(reinterpret_cast<void*>(game::at(game::kLoader)));
}

void pop_loader_pauses() {
  uint8_t* loader = reinterpret_cast<uint8_t*>(game::at(game::kLoader));
  using Pop = void (*)(void*, void**);
  for (int guard = 0; guard < 16; ++guard) {
    uint16_t n = *reinterpret_cast<uint16_t*>(loader + game::kPauseStack + 0xA);
    void** stack = *reinterpret_cast<void***>(loader + game::kPauseStack);
    void* found = nullptr;
    for (int i = n - 1; i >= 0 && !found; --i) {
      const char* name = stack[i] ? static_cast<const char*>(stack[i]) + 0x14 : "";
      if (!strcmp(name, "loader") || !strcmp(name, "qloader")) found = stack[i];
    }
    if (!found) break;
    LOGI("net: taking the leftover pause '%s' off", static_cast<const char*>(found) + 0x14);
    reinterpret_cast<Pop>(game::at(game::kPausePop))(loader, &found);
  }
}

void log_pauses(const char* why) {
  uint8_t* loader = reinterpret_cast<uint8_t*>(game::at(game::kLoader));
  uint16_t n = *reinterpret_cast<uint16_t*>(loader + game::kPauseStack + 0xA);
  void** stack = *reinterpret_cast<void***>(loader + game::kPauseStack);
  std::string names;
  for (int i = 0; i < n && i < 16 && stack; ++i) {
    if (!names.empty()) names += ", ";
    names += stack[i] ? static_cast<const char*>(stack[i]) + 0x14 : "?";
  }
  LOGI("net: %s; pauses: %s", why, names.empty() ? "none" : names.c_str());
}

void unpause_all() {
  uint8_t* loader = reinterpret_cast<uint8_t*>(game::at(game::kLoader));
  using Pop = void (*)(void*, void**);
  for (int guard = 0; guard < 16; ++guard) {
    uint16_t n = *reinterpret_cast<uint16_t*>(loader + game::kPauseStack + 0xA);
    if (!n) break;
    void* handle = (*reinterpret_cast<void***>(loader + game::kPauseStack))[n - 1];
    LOGI("net: taking the pause '%s' off", handle ? static_cast<const char*>(handle) + 0x14 : "?");
    reinterpret_cast<Pop>(game::at(game::kPausePop))(loader, &handle);
  }
}

}  // namespace net
