// The level's loose physical things (crates, barrels, bottles, planks, swinging doors) lie alike on every PC.
//
// Each PC runs its own physics: a crate one player kicks moves on that player's PC only, and on the others it
// stays where it was - a player walks into a crate the other one sees two metres away. The PC whose physics moved
// a thing says where it is now (ten times a second while it moves); the host passes it on, and every other PC
// puts its copy there. A thing another PC is moving is left to that PC for a while.
#pragma once

#include "mod/config.h"

namespace physics {

void install(const Config& cfg);

// Every tick of the main thread.
void tick();

}  // namespace physics
