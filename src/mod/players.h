// Who plays: a name for every player slot of the session, the same list on every PC.
//
// A player's name is its Steam name. The host knows the Steam user behind every connection that came through
// Steam; a player who came by IP address tells its own name (its Steam name when Steam runs on that PC, else
// [net] name of its metronet.ini). A player with no name at all is "Игрок N". The host sends the list to
// everybody whenever it changes.
#pragma once
#include <string>

#include "mod/config.h"

namespace players {

void install(const Config& cfg);

// The name of the player in a slot (0 = the host), UTF-8; "" when nobody sits there.
std::string name(int slot);

}  // namespace players
