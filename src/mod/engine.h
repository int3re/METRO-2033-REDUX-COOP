// The game's engine from the mod's side: its log, its console, the moment its start-up is over.
#pragma once
#include <functional>
#include <string>

#include "mod/config.h"

namespace engine {

// Hooks the engine's log (every line also goes to metronet.log). Call before the game's code runs.
void install(const Config& cfg);

// True once the engine wrote "initialization finished" (the console and the main loop are there).
bool ready();
// Seconds since ready(), 0 before it.
double since_ready();

// A console command of the game. queued = run by the main loop at its next turn (safe from any thread);
// otherwise run right here (main thread only).
void command(const std::string& line, bool queued = true);

// Runs `fn` on the game's main thread at its next turn of the window message pump (also while a movie plays,
// when the console's own queue stands still).
void on_main(std::function<void()> fn);

// `fn` gets every line the engine logs, on the thread that logs it (keep it short: flags and counters).
void on_log(void (*fn)(const char* line));
// `fn` runs on the main thread at every turn of the window message pump.
void on_tick(void (*fn)());

// Main thread. Back to the main menu. A host or a single player: the way the game's own "Exit to the main menu"
// goes (its handler "main_menu_handler", exe+666B70, without its question: the mark [exe+D01E50]+0x1DB0 = 1, then
// "change_map <menu>"). A joined game: "disconnect", then "map 000" as at the game's start (see engine.cpp).
void to_main_menu();

// The last lines the engine logged that contain `needle` (for the lab's checks), newest last.
int count_log(const char* needle);

}  // namespace engine
