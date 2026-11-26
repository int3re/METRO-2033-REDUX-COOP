// The co-op session as a player gets it ([net] in metronet.ini): the mod does by itself what the lab's script
// commands do by hand.
//   mode = host : every story level this game starts becomes a network server once it runs; a player who
//                 joins gets a body of their own (a copy of the host's player) a few seconds later.
//   mode = join : once the main menu is up the game connects to `address`, loads the host's level from the
//                 host's quick save and enters it.
#pragma once
#include <cstdint>
#include "mod/config.h"

namespace session {

void install(const Config& cfg);
// The main menu runs (its level, nothing loading).
bool on_main_menu();
// The player chose a friend's game on the menu's network page (Steam lobby): the session goes there.
void join_game(uint64_t lobby);

}  // namespace session
