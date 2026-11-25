// The few Steam calls metronet makes, behind one interface: real Steam (steam_real.cpp; steam_stub.cpp when
// the mod is built without the Steamworks SDK) or the lab's stand-in (steam_mock.cpp), which lets two or
// three test copies on one PC go through the same code with no Steam account at all.
//
// The game runs under Valve's free sample application, Spacewar (AppID 480), as the sister mods do: every
// Steam user may run it, and it gives lobbies, friends and Valve's relays - no open ports, no VPN.
//
// Every call is made on the one Steam thread of steamnet.cpp.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mod/config.h"

namespace steam {

constexpr uint32_t kAppId = 480;

struct Message {
  uint64_t from = 0;
  std::vector<uint8_t> data;
};

struct Event {
  enum Kind {
    SessionRequest,  // id = a user whose first message is held until accept()
    SessionFailed,   // id = the user, text = why
    LobbyCreated,    // id = the lobby (ok) / text = why not
    LobbyEntered,    // id = the lobby, ok
    LobbyMembers,    // id = the lobby: somebody came or went
    JoinRequested,   // id = a lobby the user asked to join in Steam itself (an invitation, "Join game")
    LobbyData,       // id = a lobby whose data was asked for with request_lobby_data(); ok = it is there
  };
  Kind kind = SessionRequest;
  uint64_t id = 0;
  bool ok = false;
  std::string text;
};

struct FriendGame {
  uint64_t who = 0;
  std::string name;
  uint64_t lobby = 0;
};

class Backend {
 public:
  virtual ~Backend() = default;
  virtual void pump() = 0;  // Steam's callbacks -> events
  virtual bool next_event(Event& e) = 0;

  virtual uint64_t my_id() = 0;
  virtual std::string my_name() = 0;                // UTF-8
  virtual std::string name_of(uint64_t user) = 0;   // UTF-8, "" when Steam does not know it (yet)

  virtual bool send(uint64_t to, const void* data, uint32_t size, bool reliable, int channel) = 0;
  virtual int receive(int channel, Message* out, int max) = 0;
  virtual void accept(uint64_t user) = 0;
  virtual void close(uint64_t user) = 0;
  virtual std::string link_state(uint64_t) { return ""; }  // for the log: "connected, relayed, 43 ms"

  virtual void create_lobby(int max_members) = 0;   // -> LobbyCreated
  virtual void join_lobby(uint64_t lobby) = 0;      // -> LobbyEntered
  virtual void leave_lobby(uint64_t lobby) = 0;
  virtual uint64_t lobby_owner(uint64_t lobby) = 0;
  virtual std::vector<uint64_t> lobby_members(uint64_t lobby) = 0;
  // Asks Steam for the data of a lobby this user does not sit in (-> LobbyData; lobby_data() works after it).
  virtual void request_lobby_data(uint64_t lobby) = 0;
  virtual void set_lobby_data(uint64_t lobby, const char* key, const std::string& value) = 0;
  virtual std::string lobby_data(uint64_t lobby, const char* key) = 0;
  // The user's friends who sit in a lobby of the same application right now.
  virtual std::vector<FriendGame> friends_in_lobbies() = 0;
  virtual void set_presence(const char* key, const char* value) = 0;
};

// nullptr when Steam cannot be used; `why_not` says why (for the log), `later` whether another try may do
// (Steam is not running yet).
std::unique_ptr<Backend> make_real(std::string& why_not, bool& later);
std::unique_ptr<Backend> make_mock(const Config& cfg, std::string& why_not);

}  // namespace steam
