// Addresses inside metro.exe (Metro 2033 Redux, GOG build of Sep 11 2014), as offsets from the image base:
// the exe is relocated (ASLR), so every use goes through game::at().
#pragma once
#include <windows.h>

#include <cstdint>

namespace game {

// What tells this exe from another build: PE timestamp and image size.
constexpr uint32_t kTimeDateStamp = 0x5411A479;
constexpr uint32_t kSizeOfImage = 0x1358000;

inline uintptr_t base() {
  static uintptr_t b = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  return b;
}
inline uintptr_t at(uintptr_t rva) { return base() + rva; }
template <typename T>
inline T& ref(uintptr_t rva) {
  return *reinterpret_cast<T*>(at(rva));
}

// --- start-up -------------------------------------------------------------------------------------
constexpr uintptr_t kAlreadyRunning = 0x63C520;  // bool (): WinMain's "another copy is already running" check

// --- core ---------------------------------------------------------------------------------------
constexpr uintptr_t kLogLine = 0x641990;      // void (const char* line): one finished line of the engine's log
constexpr uintptr_t kConsoleGet = 0x64C900;   // console* (): makes the console on the first call
constexpr uintptr_t kConsolePtr = 0xD01E30;   // console*
// console vtable: +0x40 execute now (printf-like), +0x48 put into the queue of the main loop (printf-like,
// safe from any thread: a spin lock at +0x20C0 guards the queue).
constexpr size_t kConsoleExecNow = 0x40;
constexpr size_t kConsoleExecQueued = 0x48;

// --- loader -------------------------------------------------------------------------------------
// A loaded map waits ("press any key") in a state object; its begin() decides whether a key is needed.
constexpr uintptr_t kLoadWaitBegin = 0x67FCF0;  // void (state*): state+0x10 = int, 1 = go on without a key
constexpr uintptr_t kMapState = 0xD04540;       // int: 2 = loaded and waiting

// --- window -------------------------------------------------------------------------------------
constexpr uintptr_t kWndProc = 0x71F7B0;
constexpr uintptr_t kWindowActive = 0xD044D4;   // byte: the game's window is the active one
constexpr uintptr_t kWindow = 0xD04508;         // HWND

// --- loader: pauses ---------------------------------------------------------------------------------
constexpr uintptr_t kLoader = 0xD04420;         // the engine's loader / main object ("g_loader")
constexpr uintptr_t kLoaderFinish = 0x67FF80;   // void (loader*): "loader_finish: mapstate_running"
constexpr uintptr_t kPausePop = 0x648FE0;       // void (loader*, handle*): "POP PAUSE - REASON [%s]"
constexpr size_t kPauseStack = 0x16810;         // loader + : handle** array; u16 size at +0xA (0x1681A)

// --- world ------------------------------------------------------------------------------------------
constexpr uintptr_t kLevel = 0xD01EA8;          // level* ("g_level"), null while no level is loaded
constexpr size_t kLevelControlled = 0x38;       // level + : the entity this PC controls (the player's body)
constexpr size_t kEntityHandle = 0xA0;          // entity + : what handles point at; the id is a u16 at handle + 0x68
constexpr size_t kHandleId = 0x68;
constexpr uintptr_t kServer = 0xD022D0;         // the entity server made by the "server" / "connect" command
constexpr size_t kServerEntities = 0x10;        // server + : 16-byte entries (entity* at +8); u16 count at server + 0x1A
constexpr size_t kServerEntityCount = 0x1A;
constexpr size_t kEntityId = 0x108;             // entity + : u16 id
constexpr size_t kEntityFlags = 0x10C;          // entity + : u32 flags (0x8 networked, 0x10000 a spawned copy)
constexpr size_t kEntityName = 0x1B0;           // entity + : name string object, characters at +0x14
constexpr size_t kEntityPosition = 0xF0;        // entity + : translation of its 4x4 matrix (+0xC0)

// --- saves --------------------------------------------------------------------------------------------
constexpr uintptr_t kSaveRequest = 0x6548D0;    // void (savemgr*, const char* name): queues a save ("" = auto_save)
constexpr uintptr_t kSaveQueued = 0x65BD30;     // int (): the main loop writes the queued save (worker 0x6579F0)
// Single-player code that takes "the player of this PC" (level + 0x38) for granted. A joined game has none from
// the moment the host's level is loaded until its body arrives.
constexpr uintptr_t kHudPickup = 0x542EE0;     // void (notifications*, item*, int count): the "picked up" line; takes the player for granted
constexpr uintptr_t kWatcherUpdate = 0x377890;  // void (entity*): per-frame update of an entity that watches the
                                                // player (state at +0x834); reads the player's position at +377B04
constexpr uintptr_t kPlayerScaled = 0x220160;   // int64 (obj, id, x, int by_player, char flag): a value scaled by
                                                // the player's weapon when by_player != 0; tests the player for
                                                // null and reads through it anyway (+2201E0)
constexpr uintptr_t kEntityWeapon = 0x6D8;      // virtual of the player: what kPlayerScaled reads through
constexpr uintptr_t kSavePrefix = 0xCF14C0;   // string handle (chars at +0x14): "2033", the first part of save names
constexpr uintptr_t kSaveName = 0x65B240;     // handle* (x, handle* out, const char* name): "<2033>_<name>"; no
                                              // name = auto_save. Every reader and writer of a save goes through
                                              // it, the level end's "save_player" (0x65B970) too
constexpr uintptr_t kTellLevel = 0x6977C0;    // void (x, const char* level): the host tells every joined game whose
                                              // slot is in state 1 ("changes the level") to load that level
                                              // (message 3 -> "change_map_cl <level>" there). The "map" command
                                              // does it by itself, a save load ("gameload") does not
constexpr uintptr_t kConsoleLine = 0x64B420;  // void (char* line): runs one console command (logs "^line")
constexpr size_t kEntityAsItem = 0x6E8;         // entity vtbl + : inventory item part of the entity (null: not an item)
constexpr size_t kItemSlot = 0xAA;              // item + : u8 inventory slot (the player's weapons: 1, 2 and 16)
constexpr size_t kEntityLife = 0x200;           // entity + : int, ms to live once it lies in the world (0: for ever); +4 the time it goes
constexpr uintptr_t kLifeTimer = 0x7188B0;      // void (entity*): arm / disarm the timer after the life time was changed
constexpr uintptr_t kSpawnRun = 0x68CEB0;       // void (x, event*): a spawn event is carried out; event: u8 kind (& 0x1F: 6 = a player's body), u16 @+8 (high byte & 0x1F = slot)
constexpr uintptr_t kAmmoClass = 0xA0D4E0;      // vtable of ammunition items
constexpr size_t kActorInventory = 0xB70;       // actor + : its inventory object
constexpr size_t kInventorySelect = 0x40;       // inventory vtbl + : int64 (inventory*, char slot, 0, 0): the weapon of a slot into the hands
constexpr uintptr_t kPlayerInventory = 0xAD7C30;  // vtable of the inventory of the player this PC controls (others have the base class, +AD7920)
constexpr uintptr_t kCloneCore = 0x689D30;      // (server*, list*, out sources, out copies): makes the copies of a clone list
constexpr uintptr_t kActorTake = 0x2776E0;      // (actor*, child entity*, u8* flags): an actor's inventory takes a child
constexpr size_t kActorWeaponMain = 0xDF8;      // actor + : link {handle*, u16 id @+8, bool valid @+10} to the weapon of slot 16
constexpr size_t kActorWeaponSecond = 0xE08;    // actor + : the same for the weapon of slot 2
constexpr uintptr_t kPlayerRemap = 0x1FE2E0;    // (player*, id map*): the player's vtbl + 0xB68, links to the new ids after a clone
constexpr uintptr_t kRemapLink = 0x208FE0;      // (link*, id map*)
constexpr uintptr_t kKeySlot1NetCheck = 0x4F9337;      // "je <single-player way>" (0F 84 rel32) in the handler of the "weapon 1" key (action 25)
constexpr uintptr_t kKeySlot2NetCheck = 0x4F9847;      // the same in the handler of the "weapon 2" key (action 26)
constexpr uintptr_t kKeySlot16NetCheck = 0x4FB04A;     // "je +0x39" after "cmp kNetGame, 0" in the handler of the "weapon 3" key (action 64)
constexpr uintptr_t kSelectSlot16NetCheck = 0x4F9CF6;  // "je +0xD" after "cmp kNetGame, 0" in the inventory's "weapon of slot 16"
constexpr size_t kEntityMatrix = 0xC0;          // entity + : 4x4 floats, the position is its last row (+0xF0)
constexpr size_t kEntitySetMatrix = 0x190;      // entity vtbl + : void (entity*, const float m[16], 0): put the entity there
constexpr uintptr_t kInventoryAction = 0x4F8D30;  // char (inventory*, int action, char flags (1 = pressed)): 25 / 26 / 64 = weapon of slot 1 / 2 / 16, 62 / 63 = next / previous
constexpr uintptr_t kUi = 0xD01E50;             // object; + 8: the HUD manager; that + 400: the weapon HUD
constexpr size_t kUiHudManager = 8, kHudManagerWeaponHud = 400;
constexpr size_t kWeaponHudPlayer = 1752;       // weapon HUD + : the player entity it shows (its inventory is read every refresh)
constexpr uintptr_t kWeaponHudBind = 0x539B40;  // (weapon HUD*, player entity*, char flags (0x10: show the ammunition list), char 0)
constexpr size_t kPlayerPitch = 0x650;          // player + : float, the look's pitch in radians (0 level, + down); the yaw is at + 0x654
constexpr uintptr_t kHitEventFilter = 0x68DFA0;  // int64 (u16* event, queue*): may this hit event pass; event: [0] kind, [1] attacker id, [2] victim id
constexpr uintptr_t kPlayerClass = 0xA28E70;    // vtable of the player entity
constexpr uintptr_t kHudModelStart = 0x25FA40;  // (holder*): starts a HUD model's animation; holder + 8 = the model, taken for granted

// --- riding (the level script's "actions/platform_attach" / "platform_deattach") ------------------------------
// A character stands on a moving thing (handcar, lift) through its mover: actor + 0x4F8 = {actor*, physics
// controller*}, made on first use. The attach makes the thing the actor's parent; the script calls exactly these.
constexpr size_t kActorMover = 0x4F8;
constexpr uintptr_t kMoverMake = 0x2400B0;       // void (actor*): makes actor + 0x4F8
constexpr uintptr_t kPlatformAttach = 0x3A7300;  // void (mover*, handle** platform)
constexpr uintptr_t kPlatformDetach = 0x3A7970;  // void (mover*)
// An entity is taken into the engine's network list (flag 0x8, "* relevant: name") only with flag 0x4, which the
// game gives to characters alone: 0x7147C0, asked by the entity's activity update 0x6AB140.
// uint8_t* (script context*, int network_game): the player a script block acts on (see net.cpp)
constexpr uintptr_t kScriptPlayer = 0x444224;
// The script's plain "actions/attach": the child keeps its place relative to the parent when no offset is given.
constexpr uintptr_t kAttachTo = 0x6B1630;        // void (handle** child, handle** parent, name** bone, float (*offset)[12] | null)
constexpr uintptr_t kDetach = 0x6A7F20;          // void (handle** child)
constexpr uint32_t kFlagNetCandidate = 0x4;
constexpr uint32_t kFlagNetworked = 0x8;
// Flag 0x20: the entity waits for its scene (a monster of an ambush before the ambush): it does nothing, takes no
// damage, is not in the network list. The level scripts switch it by this call (no effect on an attached entity).
constexpr uint32_t kFlagWaiting = 0x20;
constexpr uintptr_t kSetActive = 0x724590;       // void (entity*, char active)
constexpr uintptr_t kAttach = 0x6B1040;         // void* (entity*, handle** out, handle** parent, u8* flags): hang an entity on a parent (null: take it off)
constexpr uintptr_t kDestroyNow = 0x689050;     // void (server*, handle** entity): what every way of removing an entity ends in
constexpr uintptr_t kDestroyEntity = 0x6896E0;  // void (server*, handle** entity, int now): marks the entity
                                                // (flag 0x200) and, in a network game, sends the destroy event
                                                // (0x6975D0) to everybody; now != 0 = only this game's queue
constexpr size_t kEntityHealth = 0x308;         // float; <= 0 = dead (0x377890 tests its target this way)
// Physics (PhysX 2.8 character controllers).
constexpr uintptr_t kPhysics = 0xD02310;        // the physics world; +0xE0 the controller manager: vtbl[0] count,
                                                // [1] array of controllers, [2] create, [3] release
constexpr uintptr_t kFirstShape = 0x06AC60;     // shape* (actor*); shape+0x10 = user data = the game's character
                                                // physics object (its +0xC8 = the controller, +0x98 flags)
// A character's move (0x763BF0) goes over all controllers and reads each one's owner. A controller that was
// released stays in the list until the physics purges it (release 0x89C7D0 only queues it) with its owner
// already cleared; the loop reads through the null owner at +763E10. In single player no character moves in
// that window; a body moved by the network does. The two "owner is null" branches go to the loop's next
// controller (0x764192) instead.
constexpr uintptr_t kMoveOwnerNull1 = 0x763E04;  // 75 04 33 C0 EB 06       -> je 0x764192
constexpr uintptr_t kMoveOwnerNull2 = 0x763E38;  // 75 06 33 DB 8B C3 EB 08 -> je 0x764192; nop; nop
// The save loop over the world's entities (0x3E1890, 128 of them per call) asks each one "whose part are you"
// (virtual +0xB70; -1 = nobody's: the loop writes it itself) through this call: FF 90 70 0B 00 00.
constexpr uintptr_t kSaveLoopOwnerCall = 0x3E1B44;
constexpr uintptr_t kEntitySaveOwner = 0xB70;

// --- network (the engine's own: ENet inside the exe) ----------------------------------------------
constexpr uintptr_t kNetGame = 0xD044AA;        // byte: this is a network game (many branches of the game code)
constexpr uintptr_t kNetCore = 0xD1ADD0;        // net_core: +0 ENetHost* (1 = a local game without sockets),
constexpr uintptr_t kNetHost = 0xD1ADD0;        //   +8 int local player slot (0 = the server), -1 until the server tells it,
constexpr uintptr_t kNetLocalPlayer = 0xD1ADD8; //   +0x18 players { net_player* array; u16 capacity; u16 size }
constexpr uintptr_t kNetPlayers = 0xD1ADE8;
constexpr size_t kNetPlayerSize = 0x70;         // net_player: +0x30 ENet peer, +0x58 actor handle, +0x64 u16 actor id, +0x68 state
constexpr uintptr_t kNetStart = 0x695420;       // void (unused, const char* address or null = be the server)
constexpr uintptr_t kNetPlayerCtor = 0x023660;  // void (net_player*)
constexpr uintptr_t kNetPlayersReserve = 0x026500;  // int (players*, unsigned count)
constexpr uintptr_t kSendPropsPack = 0x023FD0;  // (unsigned player, props*)
constexpr uintptr_t kSendPropsPackNewPeer = 0x69219F;  // its return address in the "incoming connection" code
constexpr uintptr_t kLinkActor = 0x695E90;      // void (net_player*, u16 entity id)
constexpr uintptr_t kActorOf = 0x6967A0;        // handle** (net_player*, int resolve)
constexpr uintptr_t kNetPlayerOfEntity = 0x694DF0;  // net_player* (net_core*, handle**): the slot of an entity
// A body for a player: the server clones an entity with its children and sends the spawn event to everybody.
constexpr uintptr_t kCloneList = 0x689A60;      // void (server*, handle** source, list*, u16 parent id): {src, new, parent} ids
constexpr uintptr_t kSpawnEvent = 0x6978F0;     // (0, 0, list*, handle** source, x**, x**, int, is_player, slot, 0, 0, 0, 0)
constexpr uintptr_t kEnetHostCreate = 0x8EA1A0; // ENetHost* (ENetAddress* {u32 host; u16 port}, peers, ...)
constexpr uintptr_t kEnetService = 0x8F0000;      // int (ENetHost*, ENetEvent*, u32 timeout): called by the engine's receive loop 0x693470
constexpr uintptr_t kEnetPacketCreate = 0x8F02F0; // ENetPacket* (char first_byte, const void* data, size_t size, int flags): the engine's own variant
constexpr uintptr_t kEnetPeerSend = 0x8ED0B0;     // int (ENetPeer*, u8 channel, ENetPacket*)
constexpr uintptr_t kEnetFree = 0xD03F40;         // void (*)(void*): ENet's free callback
constexpr size_t kNetPlayerPeer = 0x30;           // net_player + : ENetPeer*

}  // namespace game
