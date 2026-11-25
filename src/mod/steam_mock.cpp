// The lab's stand-in for Steam ([lab] steam = N, this copy's number): two or three test copies on one PC go
// through the mod's whole Steam path - lobby, friends, sessions, names, messages - with no Steam account.
//
//   users      SteamID 76561198860265728 + N, with a fixed name per N (Cyrillic and Latin, to test both)
//   friends    every stand-in user is every other one's friend
//   messages   UDP on 127.0.0.1, port 28800 + N; like Steam, a stranger's first messages are held until
//              accept() (or until this side has sent that user something)
//   lobbies    small text files in %TEMP%\metronet-mock-steam, shared by the copies; a member whose process
//              is gone drops out, and when that was the owner the next member owns the lobby - as on Steam
#include <winsock2.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <set>

#include "common/log.h"
#include "mod/steam.h"

namespace steam {
namespace {

constexpr uint64_t kFirstUser = 76561197960265728ull + 900000000ull;
constexpr uint64_t kFirstLobby = 0x0186000000000000ull + 900000000ull;
constexpr int kFirstPort = 28800;

const char* fixed_name(int n) {
  switch (n) {
    case 1: return "Хантер";
    case 2: return "Ульман";
    case 3: return "Bourbon_3";
    default: return nullptr;
  }
}

bool process_lives(DWORD pid) {
  if (pid == GetCurrentProcessId()) return true;
  HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (!h) return false;
  bool lives = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
  CloseHandle(h);
  return lives;
}

struct Member {
  uint64_t id = 0;
  DWORD pid = 0;
  std::string name;
};

struct Lobby {
  uint64_t owner = 0;
  std::vector<Member> members;
  std::map<std::string, std::string> data;
};

class Files {  // every lobby file, under one mutex of the whole PC
 public:
  Files() {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    dir_ = std::wstring(temp) + L"metronet-mock-steam\\";
    CreateDirectoryW(dir_.c_str(), nullptr);
    mutex_ = CreateMutexW(nullptr, FALSE, L"Local\\metronet-mock-steam");
  }

  struct Hold {
    explicit Hold(HANDLE m) : m_(m) { WaitForSingleObject(m_, 2000); }
    ~Hold() { ReleaseMutex(m_); }
    HANDLE m_;
  };
  HANDLE mutex() const { return mutex_; }

  std::wstring path(uint64_t lobby) const {
    wchar_t name[64];
    swprintf(name, 64, L"lobby-%llu.txt", static_cast<unsigned long long>(lobby));
    return dir_ + name;
  }

  // False when there is no such lobby (any more). Members whose process is gone are dropped here.
  bool read(uint64_t lobby, Lobby& out) {
    FILE* f = _wfopen(path(lobby).c_str(), L"rb");
    if (!f) return false;
    char line[600];
    bool changed = false;
    while (fgets(line, sizeof(line), f)) {
      size_t n = strlen(line);
      while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
      if (!strncmp(line, "owner=", 6)) {
        out.owner = strtoull(line + 6, nullptr, 10);
      } else if (!strncmp(line, "member=", 7)) {
        Member m;
        char* end = nullptr;
        m.id = strtoull(line + 7, &end, 10);
        m.pid = end && *end == '|' ? strtoul(end + 1, &end, 10) : 0;
        m.name = end && *end == '|' ? end + 1 : "";
        if (process_lives(m.pid))
          out.members.push_back(m);
        else
          changed = true;
      } else if (!strncmp(line, "data:", 5)) {
        char* eq = strchr(line + 5, '=');
        if (eq) out.data[std::string(line + 5, eq)] = eq + 1;
      }
    }
    fclose(f);
    if (out.members.empty()) {
      DeleteFileW(path(lobby).c_str());
      return false;
    }
    bool owner_here = false;
    for (const Member& m : out.members) owner_here |= m.id == out.owner;
    if (!owner_here) {
      out.owner = out.members[0].id;
      changed = true;
    }
    if (changed) write(lobby, out);
    return true;
  }

  void write(uint64_t lobby, const Lobby& l) {
    if (l.members.empty()) {
      DeleteFileW(path(lobby).c_str());
      return;
    }
    FILE* f = _wfopen(path(lobby).c_str(), L"wb");
    if (!f) return;
    fprintf(f, "owner=%llu\n", static_cast<unsigned long long>(l.owner));
    for (const Member& m : l.members) fprintf(f, "member=%llu|%lu|%s\n", static_cast<unsigned long long>(m.id), m.pid, m.name.c_str());
    for (const auto& d : l.data) fprintf(f, "data:%s=%s\n", d.first.c_str(), d.second.c_str());
    fclose(f);
  }

  std::vector<uint64_t> all() {
    std::vector<uint64_t> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir_ + L"lobby-*.txt").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
      out.push_back(wcstoull(fd.cFileName + 6, nullptr, 10));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
  }

 private:
  std::wstring dir_;
  HANDLE mutex_ = nullptr;
};

class Mock final : public Backend {
 public:
  Mock(int n, const std::string& name) : n_(n), id_(kFirstUser + n), name_(name) {}

  bool open(std::string& why_not) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<u_short>(kFirstPort + n_));
    if (sock_ == INVALID_SOCKET || bind(sock_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
      why_not = "the stand-in's UDP port " + std::to_string(kFirstPort + n_) + " is taken (another copy with the same number?)";
      return false;
    }
    u_long on = 1;
    ioctlsocket(sock_, FIONBIO, &on);
    int big = 4 * 1024 * 1024;
    setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&big), sizeof(big));
    return true;
  }

  ~Mock() override {
    if (lobby_) leave_lobby(lobby_);
    if (sock_ != INVALID_SOCKET) closesocket(sock_);
  }

  void pump() override {}

  bool next_event(Event& e) override {
    if (events_.empty()) return false;
    e = events_.front();
    events_.pop_front();
    return true;
  }

  uint64_t my_id() override { return id_; }
  std::string my_name() override { return name_; }

  std::string name_of(uint64_t user) override {
    if (user == id_) return name_;
    Files::Hold hold(files_.mutex());
    for (uint64_t lobby : files_.all()) {
      Lobby l;
      if (!files_.read(lobby, l)) continue;
      for (const Member& m : l.members)
        if (m.id == user) return m.name;
    }
    return "";
  }

  bool send(uint64_t to, const void* data, uint32_t size, bool, int channel) override {
    if (to <= kFirstUser || to > kFirstUser + 1000 || size > 60000) return false;
    known_.insert(to);
    release(to);
    std::vector<char> packet(9 + size);
    packet[0] = static_cast<char>(channel);
    memcpy(&packet[1], &id_, 8);
    if (size) memcpy(&packet[9], data, size);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<u_short>(kFirstPort + (to - kFirstUser)));
    return sendto(sock_, packet.data(), static_cast<int>(packet.size()), 0, reinterpret_cast<sockaddr*>(&a), sizeof(a)) > 0;
  }

  int receive(int channel, Message* out, int max) override {
    static char buffer[65536];
    for (int i = 0; i < 512; ++i) {
      int n = recvfrom(sock_, buffer, sizeof(buffer), 0, nullptr, nullptr);
      if (n < 9) break;
      Message m;
      memcpy(&m.from, buffer + 1, 8);
      m.data.assign(reinterpret_cast<uint8_t*>(buffer) + 9, reinterpret_cast<uint8_t*>(buffer) + n);
      int ch = static_cast<uint8_t>(buffer[0]);
      if (known_.count(m.from)) {
        inbox_[ch].push_back(std::move(m));
      } else {
        if (!held_.count(m.from)) events_.push_back({Event::SessionRequest, m.from, true, ""});
        auto& held = held_[m.from];
        if (held.size() < 256) held.emplace_back(ch, std::move(m));
      }
    }
    auto& q = inbox_[channel];
    int got = 0;
    while (got < max && !q.empty()) {
      out[got++] = std::move(q.front());
      q.pop_front();
    }
    return got;
  }

  void accept(uint64_t user) override {
    known_.insert(user);
    release(user);
  }

  void close(uint64_t user) override {
    known_.erase(user);
    held_.erase(user);
  }

  std::string link_state(uint64_t) override { return "connected, the lab's stand-in"; }

  void create_lobby(int) override {
    Files::Hold hold(files_.mutex());
    uint64_t lobby = kFirstLobby + static_cast<uint64_t>(n_) * 1000 + (GetTickCount64() / 100) % 1000;
    Lobby l;
    l.owner = id_;
    l.members.push_back({id_, GetCurrentProcessId(), name_});
    files_.write(lobby, l);
    lobby_ = lobby;
    events_.push_back({Event::LobbyCreated, lobby, true, ""});
  }

  void join_lobby(uint64_t lobby) override {
    Files::Hold hold(files_.mutex());
    Lobby l;
    if (!files_.read(lobby, l)) {
      events_.push_back({Event::LobbyEntered, lobby, false, "no such lobby"});
      return;
    }
    l.members.erase(std::remove_if(l.members.begin(), l.members.end(), [&](const Member& m) { return m.id == id_; }), l.members.end());
    l.members.push_back({id_, GetCurrentProcessId(), name_});
    files_.write(lobby, l);
    lobby_ = lobby;
    events_.push_back({Event::LobbyEntered, lobby, true, ""});
  }

  void leave_lobby(uint64_t lobby) override {
    Files::Hold hold(files_.mutex());
    Lobby l;
    if (files_.read(lobby, l)) {
      l.members.erase(std::remove_if(l.members.begin(), l.members.end(), [&](const Member& m) { return m.id == id_; }), l.members.end());
      if (l.owner == id_ && !l.members.empty()) l.owner = l.members[0].id;
      files_.write(lobby, l);
    }
    if (lobby_ == lobby) lobby_ = 0;
  }

  uint64_t lobby_owner(uint64_t lobby) override {
    Files::Hold hold(files_.mutex());
    Lobby l;
    return files_.read(lobby, l) ? l.owner : 0;
  }

  std::vector<uint64_t> lobby_members(uint64_t lobby) override {
    Files::Hold hold(files_.mutex());
    std::vector<uint64_t> out;
    Lobby l;
    if (files_.read(lobby, l))
      for (const Member& m : l.members) out.push_back(m.id);
    return out;
  }

  void request_lobby_data(uint64_t lobby) override {
    Files::Hold hold(files_.mutex());
    Lobby l;
    events_.push_back({Event::LobbyData, lobby, files_.read(lobby, l), ""});
  }

  void set_lobby_data(uint64_t lobby, const char* key, const std::string& value) override {
    Files::Hold hold(files_.mutex());
    Lobby l;
    if (!files_.read(lobby, l)) return;
    l.data[key] = value;
    files_.write(lobby, l);
  }

  std::string lobby_data(uint64_t lobby, const char* key) override {
    Files::Hold hold(files_.mutex());
    Lobby l;
    if (!files_.read(lobby, l)) return "";
    auto it = l.data.find(key);
    return it == l.data.end() ? "" : it->second;
  }

  std::vector<FriendGame> friends_in_lobbies() override {
    Files::Hold hold(files_.mutex());
    std::vector<FriendGame> out;
    for (uint64_t lobby : files_.all()) {
      Lobby l;
      if (!files_.read(lobby, l)) continue;
      for (const Member& m : l.members)
        if (m.id != id_) out.push_back({m.id, m.name, lobby});
    }
    return out;
  }

  void set_presence(const char*, const char*) override {}

 private:
  void release(uint64_t user) {
    auto it = held_.find(user);
    if (it == held_.end()) return;
    for (auto& m : it->second) inbox_[m.first].push_back(std::move(m.second));
    held_.erase(it);
  }

  int n_;
  uint64_t id_;
  std::string name_;
  SOCKET sock_ = INVALID_SOCKET;
  Files files_;
  uint64_t lobby_ = 0;
  std::deque<Event> events_;
  std::map<int, std::deque<Message>> inbox_;
  std::set<uint64_t> known_;
  std::map<uint64_t, std::vector<std::pair<int, Message>>> held_;
};

}  // namespace

std::unique_ptr<Backend> make_mock(const Config& cfg, std::string& why_not) {
  const char* fixed = fixed_name(cfg.lab_steam);
  std::string name = fixed ? fixed : "Игрок " + std::to_string(cfg.lab_steam);
  auto mock = std::make_unique<Mock>(cfg.lab_steam, name);
  if (!mock->open(why_not)) return nullptr;
  return mock;
}

}  // namespace steam
