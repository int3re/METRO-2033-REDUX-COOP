// The motions of the level's things (doors, gates, wheels, hatches, lifts: anything with a simple skeleton that is
// not a character) are the same on every PC of a session.
//
// Every PC runs the level's script itself, and characters are driven by the host. A thing a character moves -
// a door a story character opens in a scene - is moved by the host's script only: the joined game's copy of
// that character never reaches its "open" (its brain runs as a client), and the door stays shut there. The game
// starts every motion of such a thing in one place (exe+6A71A0: entity, motion number, bone part, speed, ...).
// The host tells every joined game which motion a thing started; a joined game starts the same motion on its
// copy unless its own script already did.
#pragma once
#include <string>

#include "mod/config.h"

namespace motions {

void install(const Config& cfg);

// Every tick of the main thread (sends the host's batch, plays what arrived).
void tick();

// The lab: log every motion a thing starts (with who started it) for that many seconds.
void lab_trace(unsigned seconds);
// The lab: start a motion of a thing by its name on this PC only, as a script would (the host's script: the joined
// games get it as any other); with an unknown name, the thing's motions are listed.
void lab_play(unsigned id, const std::string& name);
// The lab: the motion started last on a thing on this PC, and how far it has come.
void lab_state(unsigned id);

}  // namespace motions
