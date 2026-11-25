// Real Steam under Spacewar (AppID 480). Built only with the Steamworks SDK (steam_stub.cpp otherwise).
//
// The game folder already holds a steam_api64.dll: GOG's stand-in, 16 functions that do nothing, which the exe
// imports by name. The real one therefore ships under another name (metronet_steam_api64.dll), is loaded by its
// full path and is called through its "flat" exports only - an import library would be bound to the stand-in.
// The SDK's headers are used for the structures and constants, none of their functions is called.
#include <windows.h>

#pragma warning(push, 0)
#include <steam/steam_api.h>
#include <steam/isteamnetworkingmessages.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingtypes.h>
#pragma warning(pop)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>

#include "common/log.h"
#include "mod/steam.h"

namespace steam {
namespace {

const wchar_t kDll[] = L"metronet_steam_api64.dll";

// --- the flat exports this code calls ---------------------------------------------------------------------
#define STEAM_API(X)                                                                                                        \
  X(bool, SteamAPI_IsSteamRunning, ())                                                                                      \
  X(ESteamAPIInitResult, SteamAPI_InitFlat, (SteamErrMsg*))                                                                 \
  X(void, SteamAPI_Shutdown, ())                                                                                            \
  X(HSteamPipe, SteamAPI_GetHSteamPipe, ())                                                                                 \
  X(void, SteamAPI_ManualDispatch_Init, ())                                                                                 \
  X(void, SteamAPI_ManualDispatch_RunFrame, (HSteamPipe))                                                                   \
  X(bool, SteamAPI_ManualDispatch_GetNextCallback, (HSteamPipe, CallbackMsg_t*))                                            \
  X(void, SteamAPI_ManualDispatch_FreeLastCallback, (HSteamPipe))                                                           \
  X(bool, SteamAPI_ManualDispatch_GetAPICallResult, (HSteamPipe, SteamAPICall_t, void*, int, int, bool*))                   \
  X(ISteamUser*, SteamAPI_SteamUser_v023, ())                                                                               \
  X(ISteamFriends*, SteamAPI_SteamFriends_v018, ())                                                                         \
  X(ISteamMatchmaking*, SteamAPI_SteamMatchmaking_v009, ())                                                                 \
  X(ISteamNetworkingMessages*, SteamAPI_SteamNetworkingMessages_SteamAPI_v002, ())                                          \
  X(ISteamNetworkingUtils*, SteamAPI_SteamNetworkingUtils_SteamAPI_v004, ())                                                \
  X(uint64, SteamAPI_ISteamUser_GetSteamID, (ISteamUser*))                                                                  \
  X(const char*, SteamAPI_ISteamFriends_GetPersonaName, (ISteamFriends*))                                                   \
  X(const char*, SteamAPI_ISteamFriends_GetFriendPersonaName, (ISteamFriends*, uint64))                                     \
  X(int, SteamAPI_ISteamFriends_GetFriendCount, (ISteamFriends*, int))                                                      \
  X(uint64, SteamAPI_ISteamFriends_GetFriendByIndex, (ISteamFriends*, int, int))                                            \
  X(bool, SteamAPI_ISteamFriends_GetFriendGamePlayed, (ISteamFriends*, uint64, FriendGameInfo_t*))                          \
  X(bool, SteamAPI_ISteamFriends_SetRichPresence, (ISteamFriends*, const char*, const char*))                               \
  X(void, SteamAPI_ISteamFriends_ClearRichPresence, (ISteamFriends*))                                                       \
  X(SteamAPICall_t, SteamAPI_ISteamMatchmaking_CreateLobby, (ISteamMatchmaking*, ELobbyType, int))                          \
  X(SteamAPICall_t, SteamAPI_ISteamMatchmaking_JoinLobby, (ISteamMatchmaking*, uint64))                                     \
  X(void, SteamAPI_ISteamMatchmaking_LeaveLobby, (ISteamMatchmaking*, uint64))                                              \
  X(int, SteamAPI_ISteamMatchmaking_GetNumLobbyMembers, (ISteamMatchmaking*, uint64))                                       \
  X(uint64, SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex, (ISteamMatchmaking*, uint64, int))                            \
  X(const char*, SteamAPI_ISteamMatchmaking_GetLobbyData, (ISteamMatchmaking*, uint64, const char*))                        \
  X(bool, SteamAPI_ISteamMatchmaking_RequestLobbyData, (ISteamMatchmaking*, uint64))                                        \
  X(bool, SteamAPI_ISteamMatchmaking_SetLobbyData, (ISteamMatchmaking*, uint64, const char*, const char*))                  \
  X(uint64, SteamAPI_ISteamMatchmaking_GetLobbyOwner, (ISteamMatchmaking*, uint64))                                         \
  X(EResult, SteamAPI_ISteamNetworkingMessages_SendMessageToUser,                                                           \
    (ISteamNetworkingMessages*, const SteamNetworkingIdentity*, const void*, uint32, int, int))                             \
  X(int, SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel,                                                        \
    (ISteamNetworkingMessages*, int, SteamNetworkingMessage_t**, int))                                                      \
  X(bool, SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser, (ISteamNetworkingMessages*, const SteamNetworkingIdentity*)) \
  X(bool, SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser, (ISteamNetworkingMessages*, const SteamNetworkingIdentity*))  \
  X(ESteamNetworkingConnectionState, SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo,                            \
    (ISteamNetworkingMessages*, const SteamNetworkingIdentity*, SteamNetConnectionInfo_t*, SteamNetConnectionRealTimeStatus_t*)) \
  X(void, SteamAPI_SteamNetworkingMessage_t_Release, (SteamNetworkingMessage_t*))                                           \
  X(void, SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess, (ISteamNetworkingUtils*))                                  \
  X(bool, SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32, (ISteamNetworkingUtils*, ESteamNetworkingConfigValue, int32))

struct Api {
#define X(ret, name, args) ret(__cdecl* name) args = nullptr;
  STEAM_API(X)
#undef X
} api;

HMODULE g_dll = nullptr;
bool g_inited = false;

// "" when every export is there.
std::string load(std::wstring dir) {
  if (!g_dll) {
    std::wstring path = dir + kDll;
    g_dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_dll) {
      DWORD err = GetLastError();
      char line[160];
      snprintf(line, sizeof(line), "metronet_steam_api64.dll is not next to metro.exe or does not load (error %lu)", err);
      return line;
    }
  }
#define X(ret, name, args)                                                              \
  api.name = reinterpret_cast<ret(__cdecl*) args>(GetProcAddress(g_dll, #name));         \
  if (!api.name) return std::string("metronet_steam_api64.dll has no ") + #name + " (another version of the file?)";
  STEAM_API(X)
#undef X
  return "";
}

// No C++ objects here: __try needs a frame without unwinding.
int init_guarded(SteamErrMsg* msg) {
  __try {
    return static_cast<int>(api.SteamAPI_InitFlat(msg));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}

bool running_guarded() {
  __try {
    return api.SteamAPI_IsSteamRunning();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

SteamNetworkingIdentity ident(uint64_t id) {
  SteamNetworkingIdentity i;
  i.Clear();
  i.SetSteamID64(id);
  return i;
}

const char* state_name(ESteamNetworkingConnectionState s) {
  switch (s) {
    case k_ESteamNetworkingConnectionState_None: return "no session";
    case k_ESteamNetworkingConnectionState_Connecting: return "connecting";
    case k_ESteamNetworkingConnectionState_FindingRoute: return "finding a route";
    case k_ESteamNetworkingConnectionState_Connected: return "connected";
    case k_ESteamNetworkingConnectionState_ClosedByPeer: return "closed by the other side";
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: return "a problem on this side";
    default: return "?";
  }
}

class Real final : public Backend {
 public:
  Real() {
    user_ = api.SteamAPI_SteamUser_v023();
    friends_ = api.SteamAPI_SteamFriends_v018();
    match_ = api.SteamAPI_SteamMatchmaking_v009();
    msgs_ = api.SteamAPI_SteamNetworkingMessages_SteamAPI_v002();
    utils_ = api.SteamAPI_SteamNetworkingUtils_SteamAPI_v004();
  }
  bool complete() const { return user_ && friends_ && match_ && msgs_ && utils_; }

  void tune() {
    api.SteamAPI_ManualDispatch_Init();
    // The relays' list now, not at the first message.
    api.SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess(utils_);
    // The game sends a joining player its quick save (400 - 450 KB) in ENet's own bursts; Steam's default
    // 256 KB/s and 512 KB of waiting bytes would hold that back or refuse it.
    api.SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32(utils_, k_ESteamNetworkingConfig_SendBufferSize, 8 * 1024 * 1024);
    api.SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32(utils_, k_ESteamNetworkingConfig_SendRateMin, 1024 * 1024);
    api.SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32(utils_, k_ESteamNetworkingConfig_SendRateMax, 1024 * 1024);
    api.SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32(utils_, k_ESteamNetworkingConfig_RecvBufferSize, 8 * 1024 * 1024);
    api.SteamAPI_ISteamNetworkingUtils_SetGlobalConfigValueInt32(utils_, k_ESteamNetworkingConfig_RecvBufferMessages, 20000);
  }

  void pump() override {
    HSteamPipe pipe = api.SteamAPI_GetHSteamPipe();
    api.SteamAPI_ManualDispatch_RunFrame(pipe);
    CallbackMsg_t msg;
    while (api.SteamAPI_ManualDispatch_GetNextCallback(pipe, &msg)) {
      if (msg.m_iCallback == SteamAPICallCompleted_t::k_iCallback) {
        auto* done = reinterpret_cast<SteamAPICallCompleted_t*>(msg.m_pubParam);
        std::vector<uint8_t> result(done->m_cubParam ? done->m_cubParam : 1);
        bool failed = false;
        if (api.SteamAPI_ManualDispatch_GetAPICallResult(pipe, done->m_hAsyncCall, result.data(), static_cast<int>(done->m_cubParam),
                                                         done->m_iCallback, &failed))
          on_callback(done->m_iCallback, result.data(), done->m_cubParam, failed);
      } else {
        on_callback(msg.m_iCallback, msg.m_pubParam, static_cast<uint32_t>(msg.m_cubParam), false);
      }
      api.SteamAPI_ManualDispatch_FreeLastCallback(pipe);
    }
  }

  bool next_event(Event& e) override {
    if (events_.empty()) return false;
    e = events_.front();
    events_.pop_front();
    return true;
  }

  uint64_t my_id() override { return api.SteamAPI_ISteamUser_GetSteamID(user_); }

  std::string my_name() override {
    const char* n = api.SteamAPI_ISteamFriends_GetPersonaName(friends_);
    return n ? n : "";
  }

  std::string name_of(uint64_t user) override {
    if (user == my_id()) return my_name();
    const char* n = api.SteamAPI_ISteamFriends_GetFriendPersonaName(friends_, user);
    return n && *n && strcmp(n, "[unknown]") != 0 ? n : "";
  }

  bool send(uint64_t to, const void* data, uint32_t size, bool reliable, int channel) override {
    int flags = (reliable ? k_nSteamNetworkingSend_ReliableNoNagle : k_nSteamNetworkingSend_UnreliableNoNagle) |
                k_nSteamNetworkingSend_AutoRestartBrokenSession;
    SteamNetworkingIdentity id = ident(to);
    EResult r = api.SteamAPI_ISteamNetworkingMessages_SendMessageToUser(msgs_, &id, data, size, flags, channel);
    if (r != k_EResultOK && r != last_send_error_) LOGW("steam: a message to %llu was not taken (result %d)", static_cast<unsigned long long>(to), static_cast<int>(r));
    last_send_error_ = r;
    return r == k_EResultOK;
  }

  int receive(int channel, Message* out, int max) override {
    SteamNetworkingMessage_t* in[64];
    int n = api.SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel(msgs_, channel, in, max < 64 ? max : 64);
    for (int i = 0; i < n; ++i) {
      const uint8_t* p = static_cast<const uint8_t*>(in[i]->m_pData);
      out[i].from = in[i]->m_identityPeer.GetSteamID64();
      out[i].data.assign(p, p + in[i]->m_cbSize);
      api.SteamAPI_SteamNetworkingMessage_t_Release(in[i]);
    }
    return n > 0 ? n : 0;
  }

  void accept(uint64_t user) override {
    SteamNetworkingIdentity id = ident(user);
    api.SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser(msgs_, &id);
  }

  void close(uint64_t user) override {
    SteamNetworkingIdentity id = ident(user);
    api.SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser(msgs_, &id);
  }

  std::string link_state(uint64_t user) override {
    SteamNetworkingIdentity id = ident(user);
    SteamNetConnectionInfo_t info{};
    SteamNetConnectionRealTimeStatus_t now{};
    ESteamNetworkingConnectionState s = api.SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo(msgs_, &id, &info, &now);
    char line[200];
    if (s == k_ESteamNetworkingConnectionState_Connected)
      snprintf(line, sizeof(line), "connected, %s, ping %d ms, quality %.0f%% out / %.0f%% in",
               (info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Relayed) ? "through Valve's relay" : "directly", now.m_nPing,
               now.m_flConnectionQualityLocal * 100.f, now.m_flConnectionQualityRemote * 100.f);
    else
      snprintf(line, sizeof(line), "%s%s%s", state_name(s), info.m_szEndDebug[0] ? ": " : "", info.m_szEndDebug);
    return line;
  }

  void create_lobby(int max_members) override {
    api.SteamAPI_ISteamMatchmaking_CreateLobby(match_, k_ELobbyTypeFriendsOnly, max_members);
  }
  void join_lobby(uint64_t lobby) override { api.SteamAPI_ISteamMatchmaking_JoinLobby(match_, lobby); }
  void leave_lobby(uint64_t lobby) override { api.SteamAPI_ISteamMatchmaking_LeaveLobby(match_, lobby); }
  uint64_t lobby_owner(uint64_t lobby) override { return api.SteamAPI_ISteamMatchmaking_GetLobbyOwner(match_, lobby); }

  std::vector<uint64_t> lobby_members(uint64_t lobby) override {
    std::vector<uint64_t> out;
    int n = api.SteamAPI_ISteamMatchmaking_GetNumLobbyMembers(match_, lobby);
    for (int i = 0; i < n && i < 256; ++i) out.push_back(api.SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex(match_, lobby, i));
    return out;
  }

  void request_lobby_data(uint64_t lobby) override { api.SteamAPI_ISteamMatchmaking_RequestLobbyData(match_, lobby); }

  void set_lobby_data(uint64_t lobby, const char* key, const std::string& value) override {
    api.SteamAPI_ISteamMatchmaking_SetLobbyData(match_, lobby, key, value.c_str());
  }

  std::string lobby_data(uint64_t lobby, const char* key) override {
    const char* v = api.SteamAPI_ISteamMatchmaking_GetLobbyData(match_, lobby, key);
    return v ? v : "";
  }

  std::vector<FriendGame> friends_in_lobbies() override {
    std::vector<FriendGame> out;
    int n = api.SteamAPI_ISteamFriends_GetFriendCount(friends_, k_EFriendFlagImmediate);
    for (int i = 0; i < n; ++i) {
      uint64_t who = api.SteamAPI_ISteamFriends_GetFriendByIndex(friends_, i, k_EFriendFlagImmediate);
      FriendGameInfo_t game{};
      if (!api.SteamAPI_ISteamFriends_GetFriendGamePlayed(friends_, who, &game)) continue;
      if (game.m_gameID.AppID() != kAppId || !game.m_steamIDLobby.IsValid()) continue;
      out.push_back({who, name_of(who), game.m_steamIDLobby.ConvertToUint64()});
    }
    return out;
  }

  void set_presence(const char* key, const char* value) override {
    if (key)
      api.SteamAPI_ISteamFriends_SetRichPresence(friends_, key, value);
    else
      api.SteamAPI_ISteamFriends_ClearRichPresence(friends_);
  }

 private:
  void on_callback(int id, const void* data, uint32_t size, bool failed) {
    Event e;
    switch (id) {
      case LobbyCreated_t::k_iCallback: {
        if (size < sizeof(LobbyCreated_t)) return;
        auto* p = static_cast<const LobbyCreated_t*>(data);
        e.kind = Event::LobbyCreated;
        e.ok = !failed && p->m_eResult == k_EResultOK;
        e.id = p->m_ulSteamIDLobby;
        if (!e.ok) e.text = "Steam's result " + std::to_string(failed ? -1 : static_cast<int>(p->m_eResult));
        break;
      }
      case LobbyEnter_t::k_iCallback: {
        if (size < sizeof(LobbyEnter_t)) return;
        auto* p = static_cast<const LobbyEnter_t*>(data);
        e.kind = Event::LobbyEntered;
        e.ok = !failed && p->m_EChatRoomEnterResponse == k_EChatRoomEnterResponseSuccess;
        e.id = p->m_ulSteamIDLobby;
        if (!e.ok) e.text = "Steam's answer " + std::to_string(failed ? -1 : static_cast<int>(p->m_EChatRoomEnterResponse));
        break;
      }
      case LobbyChatUpdate_t::k_iCallback: {
        if (size < sizeof(LobbyChatUpdate_t)) return;
        e.kind = Event::LobbyMembers;
        e.id = static_cast<const LobbyChatUpdate_t*>(data)->m_ulSteamIDLobby;
        break;
      }
      case LobbyDataUpdate_t::k_iCallback: {
        if (size < sizeof(LobbyDataUpdate_t)) return;
        auto* p = static_cast<const LobbyDataUpdate_t*>(data);
        if (p->m_ulSteamIDMember != p->m_ulSteamIDLobby) return;  // a member's own data, not the lobby's
        e.kind = Event::LobbyData;
        e.id = p->m_ulSteamIDLobby;
        e.ok = !failed && p->m_bSuccess;
        break;
      }
      case GameLobbyJoinRequested_t::k_iCallback: {
        if (size < sizeof(GameLobbyJoinRequested_t)) return;
        e.kind = Event::JoinRequested;
        e.id = static_cast<const GameLobbyJoinRequested_t*>(data)->m_steamIDLobby.ConvertToUint64();
        break;
      }
      case GameRichPresenceJoinRequested_t::k_iCallback: {
        if (size < sizeof(GameRichPresenceJoinRequested_t)) return;
        auto* p = static_cast<const GameRichPresenceJoinRequested_t*>(data);
        char text[k_cchMaxRichPresenceValueLength + 1] = {};
        memcpy(text, p->m_rgchConnect, k_cchMaxRichPresenceValueLength);
        const char* at = strstr(text, "+connect_lobby ");
        if (!at) return;
        e.kind = Event::JoinRequested;
        e.id = strtoull(at + 15, nullptr, 10);
        break;
      }
      case SteamNetworkingMessagesSessionRequest_t::k_iCallback: {
        if (size < sizeof(SteamNetworkingMessagesSessionRequest_t)) return;
        e.kind = Event::SessionRequest;
        e.id = static_cast<const SteamNetworkingMessagesSessionRequest_t*>(data)->m_identityRemote.GetSteamID64();
        break;
      }
      case SteamNetworkingMessagesSessionFailed_t::k_iCallback: {
        if (size < sizeof(SteamNetworkingMessagesSessionFailed_t)) return;
        auto* p = static_cast<const SteamNetworkingMessagesSessionFailed_t*>(data);
        e.kind = Event::SessionFailed;
        e.id = p->m_info.m_identityRemote.GetSteamID64();
        e.text = std::string(p->m_info.m_szEndDebug) + " (reason " + std::to_string(p->m_info.m_eEndReason) + ")";
        break;
      }
      default:
        return;
    }
    events_.push_back(e);
  }

  ISteamUser* user_;
  ISteamFriends* friends_;
  ISteamMatchmaking* match_;
  ISteamNetworkingMessages* msgs_;
  ISteamNetworkingUtils* utils_;
  std::deque<Event> events_;
  EResult last_send_error_ = k_EResultOK;
};

}  // namespace

std::unique_ptr<Backend> make_real(std::string& why_not, bool& later) {
  later = false;
  why_not = load(module_dir());
  if (!why_not.empty()) return nullptr;
  if (!g_inited) {
    if (!running_guarded()) {
      why_not = "Steam is not running on this PC";
      later = true;
      return nullptr;
    }
    // Steam takes the application from the environment first; no steam_appid.txt is written into the game folder.
    char id[16];
    snprintf(id, sizeof(id), "%u", kAppId);
    SetEnvironmentVariableA("SteamAppId", id);
    SetEnvironmentVariableA("SteamGameId", id);
    // Steam puts its overlay into a process that starts its API; with this it draws nothing over the game.
    SetEnvironmentVariableA("SteamNoOverlayUIDrawing", "1");
    SteamErrMsg msg = {};
    int r = init_guarded(&msg);
    if (r != 0) {
      char line[1200];
      snprintf(line, sizeof(line), "Steam did not let the game in (result %d%s%s)", r, msg[0] ? ": " : "", msg);
      why_not = line;
      later = r == static_cast<int>(k_ESteamAPIInitResult_NoSteamClient);
      return nullptr;
    }
    g_inited = true;
  }
  auto real = std::make_unique<Real>();
  if (!real->complete()) {
    why_not = "this Steam client lacks an interface the mod needs (an old client?)";
    return nullptr;
  }
  real->tune();
  return real;
}

}  // namespace steam
