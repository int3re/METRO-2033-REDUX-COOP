// The game's own menu from the mod's side: its yes / no window with a text of the mod's.
#pragma once
#include <string>

#include "mod/config.h"

namespace menu {

void install(const Config& cfg);

// Main thread. Opens the game's own confirmation window (the one "Return to the main menu?" uses) with this
// text; `yes` is called on the main thread when the player confirms. `english` is shown when the game runs in
// a language without Cyrillic letters. False when the window cannot be opened now.
bool ask(const std::string& text, const std::string& english, void (*yes)());

// A page of the game's 2D menu is open right now (by the engine's own "Activate menu" / "Deactivate menu" lines).
bool page_open();

}  // namespace menu
