// Test runs of the mod's own development ([lab] in metronet.ini): the game's window stays off the screen and
// is never the active one, the game gets no keyboard, mouse or cursor, its user folder is the lab's own,
// and a script of console commands runs by the clock. Nothing of this is on in a player's game.
#pragma once
#include "mod/config.h"

namespace lab {

void install(const Config& cfg);

}  // namespace lab
