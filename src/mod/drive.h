// A thing one player turns or holds (a wheel, a crank, a lever the script moves by "motion control") is turned by
// that player alone, and every PC sees it go as far as it went there.
//
// The level's script turns such a thing with the block "actions/entity/play motion control" (class exe+BE8A80):
// its inputs fwd / pause / bwd / stop / acc / decc start the thing's motion forwards, stop it where it is, run it
// back, and its outputs fwd_end / bwd_end / pause_end tell the script that the motion came to an end (then the
// gate rises, the story goes on). A wheel: the use hangs the player on it, and holding Use sends fwd, letting go
// sends pause or bwd. Every PC runs that script only for its own player's hold, so the friend's wheel stood still
// on the host, the host's own copy of the friend's use ran it back, and the story waiting for fwd_end never went on
// there (the user's game on 2026-11-26).
//
// Now the PC of the player who uses the thing tells the others every command its script gives such a block (with
// how far the thing's motion has come), and they give the same command to their own copy of the block; their own
// commands for it are left out while another player is at it. So the thing turns alike everywhere, and the end of
// its motion comes on every PC - the story goes on everywhere, in the chain of that player's use (its scene blocks
// only finished, see net.cpp). And while one player is at a thing, the others cannot use it ("only one turns").
#pragma once
#include <cstdint>

#include "mod/config.h"

namespace drive {

void install(const Config& cfg);
// Main thread, every tick.
void tick();

// This PC's own player is about to use a thing that waits on these entities: false when another player is at it
// now (then *by is that player's slot; the use does not happen, a line says who is at it).
bool may_use(const uint16_t* ids, int n, int* by);
// This PC's own player uses these things now (the others are told; they cannot use them until this player has
// let go of them for a while).
void used(const uint16_t* ids, int n);
// Another player's use of these things (told by kUseKey): they are that player's for a while.
void remote_used(int slot, const uint16_t* ids, int n);
// The use of player `slot` (another PC's) is still going on: what its chain does to "the player" now is that
// player's action (a hand on the wheel, the camera at it) - not a scene for everybody.
bool acting(int slot);

// The lab: a game input action (35: use) pressed for this PC's player, held for `ms`, then let go - as the
// game's input gives it to the level's script (the off-screen copies get no input of their own).
void lab_hold(unsigned action, unsigned ms);
// The lab: may this PC's player use that thing now (logged; nothing is used).
void lab_may_use(unsigned id);

}  // namespace drive
