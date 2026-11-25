// metronet-launcher.exe: what its parts share. The launcher carries the mod inside itself: it finds the
// game (Metro 2033 Redux, the GOG build), puts the mod in, asks GitHub for a newer release (a newer
// launcher, with a newer mod inside), gets it, checks it, installs from it, and starts the game.
//   model: what is on the screen (main.cpp changes it, view.cpp draws it)
//   core:  the game folder, the installed mod, settings, the release (core.cpp)
//   pkg:   the mod inside a launcher file, putting it into the game folder and taking it out (package.cpp)
//   net:   HTTP through WinHTTP, with or without a proxy (net.cpp)
//   mset:  the mod's own settings, [net] of metronet.ini, on the СЕТЬ station (modset.h, modset.cpp)
//   gcfg:  the game's own settings, user.cfg, on the ГРАФИКА and УПРАВЛЕНИЕ stations (gamecfg.h, gamecfg.cpp)
#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "launcher/gfx.h"

namespace lz {

constexpr int kW = 960, kH = 600;  // the window, in units of 1/96 inch
constexpr wchar_t kLauncherVersion[] = L"1.1.0";
constexpr wchar_t kRepoUrl[] = L"https://github.com/int3re/METRO-2033-REDUX-COOP";
constexpr wchar_t kRepoText[] = L"github.com/int3re/METRO-2033-REDUX-COOP";

// What the ИГРА page is about. WrongBuild: the folder holds a metro.exe the mod does not know.
enum class Screen { Checking, Update, Downloading, Ready, Install, NoGame, WrongBuild, Offline, Running, Problem };

// A release of the mod: the newest on GitHub (by its metronet-latest.json), or the one inside this launcher.
struct Release {
  bool known = false;   // the version is known
  bool full = false;    // GitHub's whole description came (the launcher file's name, size and checksum)
  std::wstring version;
  int protocol = 0;
  std::wstring date;    // "25 ноября 2026"
  std::wstring title;
  std::vector<std::wstring> notes;
  std::wstring pkg_name;          // the release's launcher file: the mod is inside it
  uint64_t pkg_size = 0;
  std::string pkg_sha256;
  std::wstring launcher_version;  // that launcher's own version
  std::vector<std::pair<std::wstring, int>> protocols;  // version -> protocol
};

struct Installed {
  bool present = false;   // the mod's files are in the game folder
  std::wstring version;   // empty: not known (put there by hand)
  int protocol = 0;       // 0: not known
};

struct Settings {
  std::wstring game_dir;
  bool auto_check = true;
  bool auto_self = true;     // a newer launcher on GitHub takes this one's place at the start, by itself
  bool auto_install = true;  // a newer mod on GitHub is installed at the start without a question (since 1.1.0)
  std::wstring self_skip;    // a launcher version that could not take this one's place: not tried by itself again
  bool close_on_play = true;
  int proxy = 0;  // 0 automatic, 1 none, 2 the address below
  std::wstring proxy_addr;
  // the notes of the installed version (kept from the release it came from)
  std::wstring notes_version, notes_title, notes_date;
  std::vector<std::wstring> notes;
};

enum class Tab { Game, Net, Graphics, Controls, Launcher };  // the stations of the line: the launcher's pages
constexpr int kTabs = 5;

enum class Pending { None, Install, Uninstall, Launcher };
enum class Fix { None, Retry, Back, Recheck };
enum Field { F_None = 0, F_Proxy, F_Name, F_Address };  // the field that is typed into

struct Model {
  Screen screen = Screen::Checking;
  Tab tab = Tab::Game;
  Settings set;
  Installed inst;
  Release rel;   // what GitHub says
  Release pack;  // what is inside this launcher
  bool has_game = false;
  bool exe_ok = true;           // metro.exe is the build the mod works with
  bool repo = false;            // the mod has a repository to ask (core::has_repo)
  bool unchecked = false;       // no answer asked of GitHub (the check at start is off)
  bool assets_blocked = false;  // github.com answers, its download host does not
  bool no_release = false;      // GitHub answered: nothing is published there (yet)
  bool steam = false;           // Steam runs on this PC (the co-op goes through it)
  // the update
  Pending pending = Pending::None;
  int stage = 0;  // 0 the download, 1 the checksum, 2 the installer
  uint64_t got = 0, total = 0;
  std::wstring checked_at, via;  // when GitHub answered last (HH:MM), and by what way if not directly
  std::wstring flash;            // a passing note at the foot of the page: what has just happened
  ULONGLONG flash_until = 0;
  // passing states of buttons
  bool launching = false, rechecking = false, confirm_uninstall = false;
  bool swapped = false;  // a newer launcher is in this one's place: it opens at the next start
  // the Problem screen
  std::wstring problem_title, problem_text, problem_primary, problem_secondary;
  Fix fix = Fix::None, fix2 = Fix::None;
  // the pointer and the keyboard
  int hover = 0, pressed = 0;
  int focus = F_None;
  std::wstring edit;  // the text of the name or the address while it is typed (written when the field is left)
  int capture = -1;   // УПРАВЛЕНИЕ: the key's row that waits for a key (-1: none)
  int keys_page = 0;  // ... and the page of the keys that is shown
  double time = 0;    // seconds, for what moves
};

// What can be pressed. The view tells where each one is; main.cpp acts.
enum HitId {
  H_None = 0,
  H_Min, H_Close,
  H_Tab0, H_Tab1, H_Tab2, H_Tab3, H_Tab4,  // the stations: ИГРА, СЕТЬ, ГРАФИКА, УПРАВЛЕНИЕ, ЛАУНЧЕР
  H_Primary, H_Secondary, H_Cancel,
  // СЕТЬ
  H_Coop, H_Names, H_FriendlyFire, H_NameField, H_IpPrev, H_IpNext, H_AddressField,
  // УПРАВЛЕНИЕ
  H_KeysPrev, H_KeysNext,
  // ЛАУНЧЕР
  H_Folder, H_Reinstall, H_Uninstall, H_Recheck, H_NewLauncher, H_AutoCheck, H_AutoSelf, H_AutoInstall, H_ClosePlay,
  H_ProxyPrev, H_ProxyNext, H_ProxyField, H_OpenLog, H_OpenReadme, H_Repo,
  H_Count,
  // A row of the game's settings (gcfg::Row r): H_Row + r * kRowParts + a part.
  H_Row = 1000,
  H_Bind = 2000,       // + the key's row: the row itself (a press: it waits for a key)
  H_BindReset = 3000,  // + the key's row: back to the game's own key
};
constexpr int kRowParts = 4;
enum RowPart { P_Prev, P_Next, P_Slide };  // "<", ">" (or the switch), the line of a slider
struct Hit {
  int id;
  D2D1_RECT_F r;     // in the space of `inv`
  gfx::Matrix inv;   // window -> that space
};

namespace view {
// Draws the whole window; `hits` gets what can be pressed (null: not wanted).
void draw(gfx::Canvas& c, const Model& m, std::vector<Hit>* hits);
bool primary_enabled(const Model& m);
// The model of a screen as the design shows it (launcher --shot <screen>).
bool demo(const std::wstring& screen, Model& m);
}  // namespace view

namespace net {
struct Route {
  int kind = 0;        // 0 the system's settings, 1 no proxy, 2 a proxy
  std::wstring proxy;  // host:port
  std::wstring label;  // for the log and the footer
};
struct Response {
  bool sent = false;   // the server answered
  int status = 0;
  std::wstring location;  // of a redirect that was not followed
  std::string body;
  DWORD error = 0;
};
// GET. follow: redirects are followed. to_file: the body goes there (and `progress` hears of it).
Response get(const std::wstring& url, const Route& route, bool follow, const std::wstring& to_file = L"",
             const std::function<void(uint64_t got, uint64_t total)>& progress = nullptr,
             const std::atomic<bool>* cancel = nullptr);
bool port_open(const wchar_t* host, int port, int timeout_ms);
}  // namespace net

namespace core {
extern std::wstring g_repo_url;      // tests: the repository's page served from elsewhere (--repo)
std::wstring repo();                 // its address: github.com/..., unless a test says otherwise
extern std::wstring g_settings_path; // tests: another settings file (--settings)
extern std::wstring g_cache_dir;     // tests: another folder for downloads (--cache)
extern int g_steam;                  // tests: Steam "runs" (1) or does not (0) whatever this PC has (--steam); -1: as it is
extern bool g_scripted;              // tests: no prompt of Windows is ever raised (a folder closed to writing is "denied"),
                                     // and nothing of the player's own is read (the game's user.cfg)
extern int g_game;                   // tests: the game "runs" (1) or does not (0) whatever this PC has (--game-running); -1: as it is

std::wstring downloads_dir();        // where downloads are kept (and a copy of the mod's settings before a change)
std::wstring join(const std::wstring& dir, const std::wstring& name);
std::wstring trimmed(std::wstring dir);  // a folder without the slash at its end
bool exists(const std::wstring& path);
std::string read_all(const std::wstring& path);
std::string narrow(const std::wstring& s);
std::wstring wide(const std::string& utf8);
int compare_versions(const std::wstring& a, const std::wstring& b);
int known_protocol(const Release& rel, const std::wstring& version);

void load_settings(Settings& s);
void save_settings(const Settings& s);

bool has_game(const std::wstring& dir);           // metro.exe is there
bool has_repo();                                  // there is a repository to ask for updates
bool exe_supported(const std::wstring& dir);      // ... and it is the build the mod works with (its PE header says)
// GOG's record in the registry, the launcher's own folder, the game's uninstall entry, folders beside the
// launcher. `trace`: every place that was looked at, a line each (for --where).
std::wstring find_game_dir(std::wstring* trace = nullptr);
Installed read_installed(const std::wstring& dir);
bool game_running(const std::wstring& dir);
int close_game(const std::wstring& dir);          // asks its windows to close; how many were asked
bool launch_game(const std::wstring& dir);
bool steam_running();                             // a steam.exe is among this PC's processes
bool game_process_running(const std::wstring& dir);  // a metro.exe of that folder runs (no folder: any metro.exe)
bool pick_folder(HWND owner, const std::wstring& initial, std::wstring& out);

// The newest release. Tries every route the settings allow; `route` gets the one that worked.
struct CheckResult {
  Release rel;
  bool answered = false;        // github.com answered at all
  bool assets_blocked = false;  // ... but the release's files did not come
  bool no_release = false;      // ... and said plainly that there is no release (or no repository) yet
  net::Route route;
  std::wstring log;
};
CheckResult check(const Settings& s, const std::atomic<bool>& cancel);
bool parse_manifest(const std::string& json, Release& rel);

// The mod inside this launcher (always there), and which version there is to install: GitHub's if it is
// newer than the one inside and its file can be had, else the one inside.
const Release& bundled();
bool newer_on_github(const Release& rel, bool assets_blocked);
const Release& target(const Release& rel, bool assets_blocked);

enum class Step { Ok, Cancelled, NetFail, BadSum, BadPackage, Failed, Denied };
struct UpdateHooks {
  std::function<void(int stage)> stage;
  std::function<void(uint64_t got, uint64_t total)> progress;
  std::function<void(bool waiting)> game_open;  // the game holds the mod's files: waiting for it to close
};
Step install(const Release& rel, bool download, const Settings& s, const net::Route& first, const UpdateHooks& hooks,
             const std::atomic<bool>& cancel, std::wstring* log, std::wstring* package_file);
Step uninstall(const Settings& s, const UpdateHooks& hooks, const std::atomic<bool>& cancel, std::wstring* log);
Step fetch_launcher(const Release& rel, const Settings& s, const net::Route& first, const UpdateHooks& hooks,
                    const std::atomic<bool>& cancel, std::wstring* log, std::wstring* package_file);
bool replace_self(const std::wstring& newer);
bool drop_old_self();  // true: nothing of the launcher this one replaced is left
std::wstring quoted(std::wstring path);  // a path as one argument of a command line
}  // namespace core

namespace pkg {
struct Entry {
  std::wstring name;
  int id = 0;              // the resource with its bytes
  bool loader = false, keep = false;
};
class Package {
 public:
  Package() = default;
  ~Package();
  Package(const Package&) = delete;
  Package& operator=(const Package&) = delete;
  bool open(const std::wstring& file);  // a launcher file, read as data; empty: this launcher itself
  bool bytes(int id, const void*& data, DWORD& size) const;
  bool holds(const std::wstring& name, const std::wstring& path) const;  // the file at `path` is this package's `name`, byte for byte
  std::wstring version, launcher_version;
  int protocol = 0;
  std::vector<Entry> entries;
  std::string info;  // its release.json

 private:
  HMODULE mod_ = nullptr;
  bool owned_ = false;
};
enum class Result { Ok, Failed, Denied };
Result install(const Package& p, const std::wstring& dir, std::wstring& log);
Result uninstall(const std::wstring& dir, std::wstring& log);
}  // namespace pkg

}  // namespace lz
