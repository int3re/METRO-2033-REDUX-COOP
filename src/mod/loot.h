// Loot in a session: every player takes their own. What lies in the level - ammunition, medkits, filters from a
// cabinet, a box or the dead, and weapons or other things in a stash or given as a reward - each player picks up
// themselves; what one took stays for the others until they take it. A weapon an enemy had (dropped, or on the dead)
// is one for all: who takes it first has it.
//
// The game makes a "take" an event every PC runs (see loot.cpp): a player's own take runs on that player's PC only,
// the host's removal of what its own player took leaves the others' things alone, and the host takes a joined
// player's "take" on trust (it cannot know what they look at).
#pragma once
#include <cstdint>

#include "mod/config.h"

namespace loot {

void install(const Config& cfg);

// Every tick of the main thread.
void tick();

// The lab: log for that many seconds who hangs things on whom (exe+6B1040) and what inventories take, with the
// callers.
void lab_trace(unsigned seconds);
// The lab: this PC's player takes a thing as the thing's own "use" does it (an act event of kind 0).
void lab_take(unsigned id, int kind);
// The lab: may this PC's player take that thing now (the game's own check, with what it looks at)?
void lab_can_take(unsigned id);
// The host's "these entities are gone" event, before the game carries it out: the things this PC's player has not
// taken yet are left out of it (the lab's hook of that event in net.cpp calls it; outside the lab loot.cpp hooks it).
void filter_gone(uint8_t* data);
// Called by the inventory-take hook (net.cpp) first: the lab's trace.
void lab_on_take(uint8_t* actor, uint8_t* child, uint8_t* flags);

}  // namespace loot
