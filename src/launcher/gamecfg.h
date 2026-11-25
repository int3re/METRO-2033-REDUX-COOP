// The game's own settings on the launcher's ГРАФИКА and УПРАВЛЕНИЕ stations: user.cfg, which Metro 2033
// Redux keeps in %LOCALAPPDATA%\4A Games\Metro 2033 - a text of "name value" lines and "bind action key"
// lines that the game runs at its start and writes again at its end. The names, the values each takes and
// the game's own keys are the game's (its options menu and its built-in defaults); nothing is invented.
// The file is the player's: a change rewrites the lines it is about and leaves every other line and the
// order as they were; before the first change one copy, user.cfg.metronet-backup, is put beside it and
// never written again. While the game runs nothing is written: the game would write its own over it.
#pragma once
#include <string>
#include <vector>

namespace lz {
namespace gcfg {

enum Row {
  kResolution, kFullscreen, kVsync, kQuality, kSsaa, kAf, kTess, kBlur, kFov, kGamma,  // ГРАФИКА
  kSens, kInvert, kAim, kVibration,                                                   // УПРАВЛЕНИЕ
  kRows
};
constexpr int kFirstControl = kSens;
enum class Kind { Choice, Switch, Slider };  // "< value >"; on / off; a line with a knob

extern std::wstring g_user_dir;  // tests: the folder that stands for the game's user folder (--userdir)

// Reads user.cfg (again: the game may have written it). game_dir: whose metro.exe must not be running.
void load(const std::wstring& game_dir);
// Values for a picture: no file is read or written.
void load_canned();

bool has_file();           // user.cfg is there (the game makes it at its first start)
std::wstring file();
const std::wstring& error();  // why the last change was not written

Kind kind(Row r);
const wchar_t* label(Row r);
const char* key(Row r);    // its name for the lab: res, fullscreen, ...
bool usable(Row r);        // the file has its line
bool changed(Row r);       // it is not what the file had when the launcher first read it
std::string raw(Row r);    // its line(s) as the file has them, for the lab's log

// Choice and Switch (a switch: 0 off, 1 on).
int count(Row r);
int value(Row r);
std::wstring name(Row r, int v);
bool set(Row r, int v);    // false: not written, error() says why

// Slider: where the knob is, 0 .. 1, where the game's own default is (-1: not marked), the value in words.
float fraction(Row r);
float default_fraction(Row r);
std::wstring text(Row r);
bool slide(Row r, float fraction);

// --- the keys: the file's "bind <action> <key>" lines ---------------------------------------------------
int binds();
const std::string& action(int i);
std::wstring action_label(int i, bool* raw_name = nullptr);  // in plain words, or the game's own name (raw_name)
const std::string& bound(int i);                             // "kW", "mouse0"
std::wstring key_label(const std::string& token);            // "W", "Левый Shift", "ЛКМ"
bool bind_changed(int i);
bool bind_is_default(int i);                                 // the game's own key for the action (or it has none known)
// Gives the action that key; the action that had it takes this one's. False: not written.
bool bind(int i, const std::string& token);
bool bind_reset(int i);                                      // back to the game's own key
std::string token_of_key(int dik);                           // a DirectInput key code as the file names it ("": not a key to bind)
std::string token_of_mouse(int button);                      // 0 left, 1 right, 2 middle, 3 and 4 the side ones
// The keys by pages: ДВИЖЕНИЕ, ОРУЖИЕ, СНАРЯЖЕНИЕ, ПРОЧЕЕ.
constexpr int kPageRows = 14;
int pages();
std::wstring page_name(int p);
const std::vector<int>& page(int p);

}  // namespace gcfg
}  // namespace lz
