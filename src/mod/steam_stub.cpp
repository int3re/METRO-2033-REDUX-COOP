// The mod built without the Steamworks SDK: no real Steam (steam_real.cpp is the real thing).
#include "mod/steam.h"

namespace steam {

std::unique_ptr<Backend> make_real(std::string& why_not, bool& later) {
  later = false;
  why_not = "this build of the mod was made without Steam";
  return nullptr;
}

}  // namespace steam
