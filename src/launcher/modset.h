// The mod's own settings on the launcher's СЕТЬ station: section [net] of metronet.ini next to
// metro.exe. The file is the player's: a change rewrites the one line it is about and leaves every other
// line, comment and blank as it was. The game reads the file once, at its start.
#pragma once
#include <string>

namespace lz {
namespace mset {

// [net] mode. Auto: nothing is set by hand, friends come in through Steam. Off: the mod does nothing.
// Host / Join: a fixed role, for a game by IP address.
enum class Mode { Off, Auto, Host, Join };

struct Values {
  Mode mode = Mode::Auto;
  bool names = true;           // the other players' names over their heads
  bool friendly_fire = false;  // the players' bullets hurt each other
  std::wstring name;           // this player's name when Steam is not running
  std::wstring address;        // join: the host's IP address
  bool borderless = false;     // the game in a window is shown without a frame over the whole monitor
};

// Reads metronet.ini of that folder (again: it may have been changed by hand).
void load(const std::wstring& game_dir);
// Values for a picture: no file is read or written.
void load_canned(Mode mode, const wchar_t* name, const wchar_t* address);

bool has_ini();  // metronet.ini is there (it comes with the mod)
const Values& get();
const Values& first();  // what the file had when the launcher first read it (a value changed since is shown so)
std::wstring ini_file();

// Each writes its line at once. False: not written, error() says why.
bool set_mode(Mode m);
bool set_names(bool on);
bool set_friendly_fire(bool on);
bool set_borderless(bool on);
bool set_name(const std::wstring& name);
bool set_address(const std::wstring& address);
const std::wstring& error();

// What the fields take while they are typed into.
bool name_char(wchar_t ch);
bool address_char(wchar_t ch);
constexpr size_t kNameMax = 24, kAddressMax = 64;

}  // namespace mset
}  // namespace lz
