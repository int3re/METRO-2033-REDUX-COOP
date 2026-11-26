// Print Screen in the game. The game runs in exclusive full screen and reads the keyboard through DirectInput:
// the system's own Print Screen copies nothing then (0.4.1, the user: "Print Screen puts nothing into the clipboard"),
// and the game has no screenshot of its own. The mod reads the frame the game presents and puts it into the
// clipboard as a picture.
#pragma once

#include "mod/config.h"

namespace screenshot {

void install(const Config& cfg);
// Main thread, every tick: the Present hook is set the first time (a device of the mod's own, made for a moment
// on a hidden window, gives the address), the key is watched.
void tick();
// DirectInput saw the Print Screen key down (its keyboard state, from revive's hook).
void key_seen();

}  // namespace screenshot
