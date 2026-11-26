// Lines of text on the game's screen, drawn by the engine's own text output (the fonts and the calls its
// frame counter and debug texts use) - no overlay of the mod's own.
#pragma once
#include <string>
#include <vector>

#include "mod/config.h"

namespace hud {

void install(const Config& cfg);

// A line for the player, shown for some seconds (any thread). UTF-8. `english` is shown instead when the game
// runs in a language whose fonts have no Cyrillic letters.
void notice(const std::string& text, const std::string& english, unsigned seconds = 8);

// Main or drawing thread. A text in the codes the game's fonts take, as the game's own counted string (chars at
// +0x14, the count at +8: the caller lets it go with one decrement); nullptr when the game has no language yet.
uint8_t* counted_text(const std::string& text);
// The language the game runs in has Cyrillic letters in its fonts.
bool cyrillic();

// A caption under the middle of the screen, with a bar under it (fraction 0..1; below 0: no bar), drawn every frame
// until it is set again or taken off (main thread). Empty text: off.
void progress(const std::string& text, const std::string& english, float fraction);

// A name that floats over a place of the world (a player's head). The whole list is given anew every frame
// (main thread); an empty list takes the names off.
struct Tag {
  float x, y, z;
  std::string text;
};
void tags(std::vector<Tag> list);

// A label of the main menu's desk at a place of the world: big letters and a small hint under them, lit (hot) when
// the pointer is on it (main thread; off: taken away). Where it was drawn last (centre, width, height, in the
// screen's -1..1): false when it is not on the screen.
void desk_label(bool on, float x, float y, float z, const std::string& text, const std::string& english, const std::string& hint,
                const std::string& hint_english, bool hot);
bool desk_label_rect(float* cx, float* cy, float* w, float* h);

// The lab: "!text <font 1-3> <x> <y> <height> <colour hex> <text>" keeps a line on the screen ("\xNN" = a byte);
// "!text" alone takes it off.
void lab_text(const char* args);
// The lab: looks through the game's global objects for the camera (its place, a projection matrix) and logs
// where they lie.
void lab_scan();

}  // namespace hud
