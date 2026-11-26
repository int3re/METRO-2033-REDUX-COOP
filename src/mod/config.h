// metronet.ini, read once when the mod starts.
#pragma once
#include <string>
#include <vector>

#include "common/log.h"

struct Config {
  tlog::Level log_level = tlog::Info;
  bool log_engine = true;  // the engine's own log lines go into metronet.log too

  // [net]: the co-op session.
  // Auto: nothing is set by hand. Whoever plays the story can be joined by Steam friends (the level becomes a
  // network game when the first of them comes in); a game that stands in the main menu while a friend plays
  // asks its player whether to join. Host / Join: the role is fixed for the whole run (the lab; play by IP).
  enum class NetMode { Off, Host, Join, Auto };
  NetMode net_mode = NetMode::Auto;
  std::string net_address;       // join: the host's IP address, or "steam" (a Steam friend's game), "steam:<a part of the
                                 // friend's name>", "steam:<lobby number>"
  bool net_revive = true;        // [net] revive: a player who would die falls, a teammate raises them; the checkpoint
                                 // only when everybody is down (0: death as in single player)
  bool net_loot = true;          // [net] loot: what one player takes (ammunition, medkits, filters), every player gets
  bool net_motions_back = true;  // [net] motions_back: a joined game's own one-off motions go to the host too (0.4.1 had it
                                 //   on: the motions its level load sets the doors and cabinets with went to the host, and
                                 //   every door and box stood open for everybody)
  bool net_motions = true;       // [net] motions: the host's things (doors a character opens, gates, levers) move alike
                                 //   on every PC
  int net_hide_hands = 0;        // [net] hide_hands: the other players' first-person hands are put away on this PC
                                 //   (0.4.2: off - the game's "hide hands" goes through the server and took the hands of
                                 //   that player on every PC, theirs too: nobody could pick anything up)
                                 //   (1: as the game's own "hide hands"; 2: entity flag 0x4000; 3: off the body, far
                                 //   down; 0: left as the engine has them)
  bool net_own_gear = true;      // [net] own_gear: a joined player's rounds (and the military ones - the money) go with
                                 //   them to their next body (a level change, a death) instead of the host's
  bool net_physics = true;       // [net] physics: crates, barrels and other loose things one PC's physics moves lie
                                 //   alike on every PC
  bool net_scene_hands = false;  // [net] scene_hands (0.4.2: off, the same way as hide_hands): while a scene of the host's level script keeps its player's hands
                                 //   away, the joined players' hands are away too
  bool borderless = false;       // [net] borderless: the game in a window shows without a frame over the whole monitor
                                 //   (the launcher's "Режим экрана: Без рамки"; the game's own r_fullscreen must be off)
  bool net_menu = true;          // [net] menu: "СЕТЕВАЯ ИГРА" on the main menu's desk and its page
  bool screenshot = true;        // [video] print_screen: Print Screen puts the game's frame into the clipboard
  bool net_names = true;         // [net] names: the other players' names float over their heads
  bool net_steam = true;         // [net] steam: the game goes through Steam too, under Spacewar (0: Steam is never touched)
  std::string net_name;          // [net] name: this player's name for the others when Steam is not there (UTF-8)
  int net_spawn_delay = 2;       // host: seconds between a joined game being ready and its player's body
  bool net_tree_lock = true;     // [net] tree_lock: the game's collision tree is taken for writing without holding the readers back
                                 // (0: the game's own way, which can lock the game up for good in a long fight)
  int net_timeout = 45;          // [net] timeout: seconds a PC that sends nothing is waited for before it is taken for gone
                                 // (0: the game's own 5 - 20 s, which a slow level load of the other PC runs over)
  bool net_friendly_fire = false;  // [net] friendly_fire: players' bullets hurt each other
  bool net_ride = true;            // [net] ride: a joined player sits beside the host on a rail vehicle (0: stays where it stands)
  bool net_use_on_host = false;    // [net] use_on_host: the host plays a joined player's use too (0.4.3: off - the
                                   //   use's scene went on by its own timers on the host and pulled the host's player)
  bool net_use_on_joined = false;  // [net] use_on_joined: a joined game plays the others' uses too (0.4.2: off - the
                                   //   host's use of a wheel ran its scene there and opened the door at once)
  bool net_use_scene_own = true;   // [net] use_scene: when another player's "use" is played here (the door opens here
                                   //   too), its scene (camera to the wheel, the hands' animation) stays theirs
  bool net_zone_own = true;        // [net] zone_own: what a zone does to the player where they stand (civil mode at a
                                   //   station, the gas mask, fire) goes by each player's own steps
  bool net_zones = true;           // [net] zones: a level's "the player walked in" zones count every player (0: each PC its own)
  bool net_actors = true;          // [net] actors: the host tells the state of the characters the engine does not send (0: for finding faults)
  bool net_use = true;             // [net] use: a door / lever / talk one player used is used on every PC (0: on that PC only)
  bool net_missing_target = true;  // [net] missing_target: a joined game skips a character's action from the host that names an entity
                                   // which is not there any more (0: the game as it is - it reads the id as a motion's number and falls)

  // [lab]: test runs of the mod's own development. Nothing of it is on in a player's game.
  bool lab = false;            // the window off the screen and never active, no keyboard / mouse / cursor
  std::wstring lab_userdir;    // what the game gets as the user's "local application data" folder
  int lab_quit_after = 0;      // seconds after the engine's start-up, 0 = never
  int lab_steam = 0;           // the lab's stand-in for Steam: this copy's number (1, 2, 3...), 0 = real Steam
  int lab_bind_port = -1;      // the UDP port the engine's socket takes instead of 1000 (0 = any), -1 = as is
  bool lab_autostart = true;   // a loaded map starts by itself (no "press any key")
  bool lab_desk = false;       // [lab] desk: log what the main menu's desk script shows and opens
  int lab_fps = 30;            // the test copy draws no more frames a second than this (0 = as many as it can)
  int lab_cores = 4;           // ... and runs on this many processor cores only (0 = all), at a lowered priority
  // "seconds: command" steps, in order. "N:" = N seconds after the end of the engine's start-up, "+N:" = N
  // seconds after the step before. A command is a console command of the game (">cmd" = run now on the main
  // thread), one of the mod's own ("!host", "!net" ...), or "?text" = wait for an engine log line with the text.
  struct Step {
    double at = 0;
    bool relative = false;
    std::string cmd;
  };
  std::vector<Step> script;
};

// Folder of metro.exe with a trailing slash.
std::wstring module_dir();
// Returns false when the file does not exist (defaults are kept).
bool load_config(const std::wstring& path, Config& cfg);
