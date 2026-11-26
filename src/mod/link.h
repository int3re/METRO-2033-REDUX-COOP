// The mod's own messages between the PCs of a session, inside the engine's ENet connection.
//
// The engine's network game carries what its makers needed for their multiplayer: players, characters near
// them, shots, spawned copies. A story level also has things only its script moves (a handcar, a lift, a door)
// and each PC's script moves its own copy. Those travel here: the host says, the joined games do.
//
// A message is an ENet packet on channel 0 whose first byte - where the engine keeps its message type, 0..13 -
// is 0xFE. The engine's dispatcher leaves types above 13 alone, so a PC without this code (or with an older
// mod) simply does not see them. Layout: FE 'M' 'N' <type> <payload>.
#pragma once
#include <cstddef>
#include <cstdint>

namespace link {

constexpr uint16_t kProtocol = 3;  // changes when a message's layout or meaning does (2: kUse, 0.1.5; 3: zones, physics, hands, 0.4.1)

enum Type : uint8_t {
  kHello = 1,  // {u16 protocol; char version[16]} - both ways, once per connection
  kRails = 2,  // the host's rail vehicles: {u16 count; {u16 id; float time; float speed}[count]}
  kSeat = 3,   // a joined player's body sits on a vehicle: {u16 body; u16 carrier (FFFF: off); float x, y, z}
  kActors = 4, // the host's characters: {u16 count; {u16 id; u16 parent; u8 bits; float x, y, z}[count]}
  kWay = 5,    // the host's last steps, sent with a new body: {u16 count; {float x, y, z}[count]}
  kUse = 6,    // a player used a thing: {u16 nth use block of the level; u16 how many the sender has; u8 the player's slot; u16 its body}
  // Since 0.2.0. A PC with an older mod does not know them and leaves them alone, so the protocol's number stays.
  kMyName = 7, // a joined player's name, to the host: UTF-8 bytes, no terminator
  kNames = 8,  // the host's list of who plays: {u8 count; {u8 slot; u8 length; char name[length]}[count]}
  // Since 0.4.0: a player used a thing, named by the things the use block waits on (their entity ids), not by
  // the block's number - the PCs' lists of blocks differ (a joined game makes a level's blocks twice: from the
  // level and from the host's save). {u8 from; u16 user; u8 n; u16 target[4]; u16 nth; u16 count; u8 from_end}
  kUseKey = 9,
  kDowned = 10,  // a player is down (1) / up again (0): {u8 slot; u8 down} - from that player's PC; the host tells all
  kRevive = 11,  // a player raised a fallen one: {u8 slot raised; u8 by}
  // Since 0.4.0: the motions the host's things started (doors a story character opens, gates, levers): {u8 count;
  // {u16 id; u32 name hash; u16 motion; u16 part; float speed; float time; u8 looped; u8 raw}[count]} - to the
  // joined games; kMotionsAsk (empty): a joined game whose level runs asks for all of the level's so far.
  kMotions = 12,
  kMotionsAsk = 13,
  kRaising = 14,  // a player holds Use over a fallen one (1) or let go (0): {u8 slot raised; u8 by; u8 on}
  kHands = 15,    // the host's level script has put its player's hands away (1: a scene) or let them out (0): {u8 away}
  kPhysics = 16,  // loose things a PC's physics moved: {u8 from slot; u8 count; {u16 id; u32 name hash; float m[12]}[count]}
  // Since 0.4.6 (drive.cpp): a player is at a thing (1) / has let go of it (0): {u8 slot; u8 on; u8 n; u16 ids[4]};
  // a command the level's script gave the "motion control" of a thing that player turns: {u8 slot; u8 command;
  // u8 n; u16 ids[4]; u8 from_end; u16 motion; float time}. The host passes both on to the other joined games.
  kBusy = 17,
  kDrive = 18,
  kAskLoad = 19,  // a joined player chose "load the last checkpoint": {u8 slot} - the host loads it for everybody
};

void install();

// Main thread. Sends to every connected PC (the host: every joined game; a joined game: the host), or to one
// player slot of the host's (to_slot >= 1). False when there is nobody to send to.
bool send(Type type, const void* data, size_t size, bool reliable, int to_slot = -1);

// `fn` gets every message of `type`, on the main thread, from poll(). from = the sender's player slot on the
// host (0 on a joined game: the host).
using Handler = void (*)(int from, const uint8_t* data, size_t size);
void on(Type type, Handler fn);

// Main thread, every tick: hands the received messages over; says hello to a new connection.
void poll();

// How many PCs are connected to this one right now.
int peers();
// How many connections were greeted since the game started (grows by one with every new connection).
uint32_t connections();
// The host: the address (IPv4 as in sockaddr_in) of the PC in a player slot; 0 when nobody sits there.
uint32_t address_of_slot(int slot);

}  // namespace link
