// The game's window without a frame over the whole monitor ("Без рамки" in the launcher): the game itself knows
// only full screen and a framed window (user.cfg r_fullscreen). With [net] borderless = 1 and the game in a window,
// the mod takes the frame off and stretches the window over the monitor it is on.
#pragma once

#include "mod/config.h"

namespace window {

void install(const Config& cfg);
// Main thread, every tick (looks twice a second: the game sets its window anew when the picture changes).
void tick();

}  // namespace window
