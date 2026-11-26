// In a session a player who would die falls instead: lies wounded with the least health, a teammate raises them
// by holding "use" next to them; the last checkpoint is loaded for everybody only when every player is down.
//
// The game has the means: the damage of a hit is taken off the health by exe+233940 (health block = entity +
// 0x300), which stops at a floor (block + 0x118 = entity + 0x418) when one is set and the hit allows it (block +
// 0x120 = entity + 0x420: bit 1 lets a monster's blow, hit kind 2, through the floor). In a session the mod sets
// the floor of each PC's own player to 1 % and clears that bit: the health stops at 1 % - the player is "down".
// A hit the game marks as beyond the floor (a fall, a scripted death) still kills, as before.
#pragma once
#include "mod/config.h"

namespace revive {

void install(const Config& cfg);

// The player in this slot is down (any thread).
bool downed(int slot);

// This PC's own player is down: the input that moves, crouches, jumps, fires or uses is held back (looking
// around is not). The lab's scripted pad asks this too.
bool me_down();
// What a pad gives the game while this PC's player is down (buttons and sticks; the right stick stays).
void filter_pad(void* xinput_state);

// The lab: "use" is held that long, as if the pad's X were (no input of its own).
void lab_hold_use(unsigned ms);
// This PC's player holds Use now (the game's input action 35).
bool use_held();
// The lab: log the player's input actions for that many seconds (the game's action numbers).
void lab_actions(unsigned seconds);

}  // namespace revive
