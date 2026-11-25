// The session through Steam: the game's own network packets travel as Steam messages between friends, so
// nobody opens a port or sets up a VPN.
//
// The engine's network game talks UDP through ENet, which is built into the exe. ENet is left as it is - its
// two socket calls (WSASendTo, WSARecvFrom) are caught instead: every Steam user of the session has a made-up
// IPv4 address (240.0.0.0 - 247.255.255.255, a range that exists on no network), the engine "connects" to
// that address, and what ENet sends there goes out as a Steam message; what Steam brings is handed to ENet as a
// datagram from that address. Real addresses pass through untouched, so a host takes Steam friends and plain IP
// players at the same time.
//
// A user's address is made from its Steam account number alone, so it is the same on every PC. It has to be:
// the engine's network game is a mesh - the host tells every player the addresses of the others ("connecting to
// peer ...") and the players connect to each other directly. (With addresses numbered by each PC for itself the
// third player took the host's address for the second player's, connected to the host three times over and
// fell: the lab, 2026-11-24.)
//
// The host keeps a friends-only Steam lobby (marked as a metronet game); a joining game looks through the
// friends' lobbies, enters the one that is a metronet game and connects to its owner. Only members of the lobby
// are let through.
#pragma once
#include <cstdint>
#include <string>

#include "mod/config.h"

namespace steamnet {

// Starts the Steam thread and catches the two socket calls when this run uses Steam at all.
void install(const Config& cfg);

// [net] address = steam / steam:<a part of the friend's name> / steam:<lobby number>
bool join_by_steam(const Config& cfg);

// --- mode = auto: the roles change while the game runs (any thread; the Steam thread acts within a second) ---
// This game plays a story level (true: Steam friends see it and may come in) or stands in the menu (false).
void play_as_host(bool on);
// A friend's game is knocking at this one (a level that is played, not a network game yet).
bool knocked();
// A friend's game this one could join: found while this game stands in the menu.
struct Offer {
  uint64_t lobby = 0;
  std::string name;   // the friend's Steam name
  std::string level;  // the level as the game names it ("2033\l01_hunter")
};
bool offer(Offer& out);
// The player's answer: yes = go there (host_address() gives the address when it is found), no = not asked
// about this game again for a while.
void answer_offer(uint64_t lobby, bool yes);
// This game is on its way into a friend's game or in it.
bool joining();
// A joined game is back in its menu: out of the friend's lobby.
void leave_session();

// The host: a player who came through Steam has left (out of the lobby and silent for some seconds). Gives the
// made-up address it had; false when there is no such news.
bool next_left(uint32_t& ip);
// A joined game: milliseconds since the host's game was last heard (a huge number when never).
uint64_t host_silent_ms();

// Any thread.
bool up();                         // Steam is there and this player is known
std::string my_name();             // this player's Steam name, UTF-8 ("" until up)
std::string host_address();        // a joining game: the made-up address of the host found in Steam ("" until found)
bool is_ours(uint32_t ip);         // a made-up address (network byte order, as in sockaddr_in)
std::string name_at(uint32_t ip);  // the Steam name of the player behind a made-up address ("" when unknown)
void set_level(const std::string& level);  // the host: what its lobby says about the level

}  // namespace steamnet
