// The engine's own network game (4A's development multiplayer, left in the exe) from the mod's side:
// making a loaded single-player level a server, the fixes a second player needs, joining.
//
// The way through this code was mapped by two public projects (both MIT): MetroDeveloper (tsnest) - the
// player-slot fix - and metro2033redux-coop (realblastex) - hosting a loaded level, linking the player to
// slot 0, what stalls a joined game. Their addresses are for other builds of the exe; the ones here are the
// GOG build's.
#pragma once
#include <cstdint>
#include <string>

#include "mod/config.h"

namespace net {

struct Players {  // net_core + 0x18
  uint8_t* array;
  uint16_t capacity;
  uint16_t size;
};

void install(const Config& cfg);

// Main thread. Turns the level that is loaded and running into a network server (player slot 0 = this PC's
// body). False when there is no level or no body to link.
bool host(const char* level = nullptr);
// Main thread, about 10 times a second (the host's deferred work).
void tick();
// Main thread, every frame: the mod's own messages, the rail vehicles, the seat beside the host.
void frame();
// mode = auto, before the first network session of a run: a copy of the player's autosave beside it
// (<prefix>_auto_save_before_coop), so that whatever a session does to the story's save can be taken back.
void backup_save();
// Main thread. Sets / clears the engine's "network game" flag.
void set_network_game(bool on);
// Main thread. Lets a loaded map start (what a key press on the loading screen does).
void finish_load();
// One log line: the names on the engine's pause stack (why: what the moment is).
void log_pauses(const char* why);
// Main thread. Takes every pause off the engine's pause stack (a joined game stays paused by its loader).
void unpause_all();
// Main thread. Takes the loader's own pauses ("loader", "qloader") off: one of them stays behind after a level
// starts, and with it the game clock stands still - a host sends nothing, a joined game never runs. A pause
// the player set (the menu) is not touched.
void pop_loader_pauses();
// Main thread, host only. Gives player `slot` a body: a copy of the host's own player (with its children:
// hands, torch, map, weapons), made by the engine's clone and sent to every PC by its spawn event.
bool spawn_body(int slot);
// The lab, one copy of the game: a slot nobody sits in, so that a body can be made and looked at without a second
// copy. Returns the slot's number or -1.
int add_empty_slot();
// The lab: what the last body made here consists of - every entity of the copy and the entity of the host's
// own body it was copied from (id, parent, class, name, flags).
void log_body();
// The lab: writes `size` bytes (1, 2, 4) of `value` at entity + offset and logs what was there.
void poke_entity(unsigned id, unsigned offset, unsigned size, unsigned value);
// The lab: the address of a field of an entity (0xFFFF = the body this PC controls); 0 when there is none.
uintptr_t entity_field(unsigned id, unsigned offset);
// The lab, the host: takes one entity out of the world (the game tells the joined players).
void destroy_entity(unsigned id);
// The lab, the host: a hit event as a joined player's bullet sends it (attacker, victim, the event's four data
// words as seen in a real one), given to the game's own receiver.
void lab_hit(unsigned attacker, unsigned victim);
// The lab: puts an entity at a place (the game's own "set matrix", the rotation stays).
void move_entity(unsigned id, float x, float y, float z);
// The lab: entity `id` stands `metres` in front of the body this PC controls, facing it.
void place_in_front(unsigned id, float metres);
// The lab: the living things (entities with a health between 0 and 10) within `metres` of the body this PC
// controls: id, class, health, distance, name.
void log_near(unsigned metres);
void dump_world(unsigned n);
// lab, crash log: the entity (alive, or taken out lately) an address lies in; "" if none
std::string lab_whose(uintptr_t address);
// ... and the entity that holds a pointer to (or just before) the address: "its animator", "its model"
std::string lab_held_by(uintptr_t address);
unsigned long lab_main_thread();
std::string lab_tree_lock();  // lab: how the collision tree's write lock fared in this run
unsigned long long lab_last_frame();  // lab: GetTickCount64 of the main loop's last turn (0: none yet)
std::string lab_updating();  // lab, crash log: the entity this thread is updating right now (exe+6B1720)
void lab_uses();
void look_deep(unsigned id, unsigned offset, unsigned at, unsigned count);
void lab_use(unsigned n);
// The lab: fires, as this PC's player, the use block that waits on the entity with this name - the k-th of such
// blocks counted from the one made last (a cabinet has two).
void lab_use_named(const std::string& name, unsigned k);
// The lab: fires, as this PC's player, the k-th use block (from the end) that waits on the entity with this id.
void lab_use_id(unsigned id, unsigned k);
void lab_use_on(unsigned id);  // the lab: the use blocks waiting on that entity are on (the script has not come to them)
void lab_bring(unsigned class_rva, float metres);
void lab_ride(unsigned id, unsigned platform);  // platform FFFF: off
void lab_hang(unsigned id, unsigned parent);    // parent FFFF: off
void look_object(unsigned id, unsigned offset, unsigned count);
// The lab's aiming: how far (radians, + = the target is to the right) the body this PC controls looks past
// entity `id`, and the distance to it. False when either is not there. Reads memory only (any thread).
bool aim_error(unsigned id, float* yaw, float* distance);
// How far (radians, + = the look must go down) the look is from a point `height` metres above the entity's feet.
bool aim_pitch_error(unsigned id, float height, float* pitch);
// The lab: the nearest living entity of a class (vtable RVA) to the body this PC controls, within `metres`;
// 0xFFFF when there is none. Reads memory only.
unsigned nearest_of_class(unsigned class_rva, float metres, float* health, const char* name_part = nullptr);
// The lab: asks the inventory of the body this PC controls for the weapon of a slot (1, 2, 16), as the keys do.
void lab_select_slot(unsigned slot);
// The lab: an input action as the keys send it to the inventory of the body this PC controls (25 / 26 / 64 =
// weapon 1 / 2 / 3, 62 / 63 = next / previous weapon), pressed and let go.
void lab_inventory_action(unsigned action);
// The lab: what a body's inventory holds (its item array), each entry named by the entity it lies in.
void log_inventory(unsigned id);
// The lab: remember an entity's memory / log the 4-byte values that changed since (small numbers only).
void snap_entity(unsigned id);
void diff_entity(unsigned id);
// The lab: `count` qwords of an entity's memory from `offset`.
void look_entity(unsigned id, unsigned offset, unsigned count);
// The lab: hangs entity `id` on entity `parent` with the game's own attach.
void lab_attach(unsigned id, unsigned parent);
// The lab: entities of the host's body (ids) that the next bodies are made without, with what hangs on them.
void lab_leave_out(unsigned a, unsigned b);
// The lab: every entity whose parent is `id` (FFFF = the body this PC controls), with what hangs on those.
void log_children(unsigned id);
// The lab: how the other players' first-person hands are put away on this PC from now on (0 = not; see hide_hands).
void lab_hands(int mode);
// The lab: asks the game to decide anew whether the player on this PC may take the item (entity vtbl + 0xB88).
void refresh_item(unsigned id);
// Main thread, host. Takes the body this game made for the slot out of the world (the player left).
// False when the slot has no body of ours.
bool remove_body(int slot);
// Main thread, host. A new body for a slot that has one (its player died); the old one stays until
// remove_old_body (the joined game must switch to the new one first).
bool respawn_body(int slot);
void remove_old_body(int slot);
// Main thread. How many entities of the body last taken out of the slot are still in the world (-1: none was).
int removed_body_left(int slot);
// Main thread. Logs the entity with this id (name, flags, position) or that there is none; id 0xFFFF = how many
// entities the world holds and how many of them are spawned copies.
void log_entity(unsigned id);
// Lab, main thread: sets the health of the entity (0xFFFF = the body this PC controls).
void set_health(unsigned id, float health);
// Main thread. The health of the entity with this id; -1000 when there is none.
float health_of(unsigned id);
// Lab: the physics character controllers; all of them, or only those whose shape has no owner.
void log_controllers(bool all);
// One log line: the session as this PC sees it.
void log_state(const char* why);

}  // namespace net
