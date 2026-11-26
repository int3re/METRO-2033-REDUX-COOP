// "СЕТЕВАЯ ИГРА" in the main menu.
//
// The main menu of Redux is a desk (the level 2033\000) whose items are labels on its things, driven by the
// level's script; a new item cannot be put into that script. The mod draws its own label on the desk, beside the
// gramophone, in the game's font; a click on it, the key N or the pad's Y opens the game's own notebook page that
// the PC build never shows (the leaderboard: "mainmenu leaderboard"), turned into the network page: the Steam
// friends who play now, one row each (name and chapter), "Войти" (Enter) goes into the chosen one's game,
// "Обновить" (Y) asks Steam again, Esc goes back. The page's words are the game's own text calls with other
// words put in (exe+707770), its rows the game's own answer of the leaderboard service, called with the friends'
// games instead of scores (exe+664CA0).
#pragma once

#include "mod/config.h"

namespace netmenu {

void install(const Config& cfg);
// Main thread, every tick: the label on the desk, the pointer and the keys, filling the page.
void tick();
// The keyboard (DirectInput, the revive module's hook) or the pad saw the page's key go down.
void key_seen();

// The lab: open the page as a click on the label would; "press" Enter on it.
void lab_open();
void lab_enter();

}  // namespace netmenu
