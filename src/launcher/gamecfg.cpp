// The game's settings the launcher edits: see gamecfg.h. What each variable takes is the game's own: its
// options menu offers quality 0..3 (low, medium, high, very high), texture filtering 0 / 1 (AF 4x, AF 16x),
// motion blur 0 / 1 (off, normal), vibration 0..3 (off, weak, fairly strong, strong); its SSAA list writes
// "r_enum_ssaa <tenths of the factor>" (10 off, 5, 20, 30, 40) together with "r_supersample <factor>"; its
// tessellation list is the pair r_dx11_tess / r_tess_ss (0 0 off, 1 0 normal, 1 1 high, 1 2 very high); the
// field of view runs from 45 to 90, gamma from 0.5 to 1.5, the mouse's sensitivity from 0.1 to 2, the aim
// assist from 0 to 1. The keys below are the game's built-in ones.
#include "launcher/gamecfg.h"
#include "launcher/modset.h"

#include <windows.h>

#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>

#include "launcher/launcher.h"

namespace lz {
namespace gcfg {

std::wstring g_user_dir;

namespace {

const wchar_t kFile[] = L"user.cfg";
const wchar_t kBackup[] = L"user.cfg.metronet-backup";

// The actions of the game's keys: its own key for each, a name in plain words where the game's name says
// plainly what the action is (else the game's name is shown), and the page of the launcher's list.
const struct Action {
  const char* name;
  const char* key;
  const wchar_t* label;
  int group;  // 0 ДВИЖЕНИЕ, 1 ОРУЖИЕ, 2 СНАРЯЖЕНИЕ, 3 ПРОЧЕЕ
} kActions[] = {
    {"forward", "kW", L"Вперёд", 0},
    {"back", "kS", L"Назад", 0},
    {"lstrafe", "kA", L"Влево", 0},
    {"rstrafe", "kD", L"Вправо", 0},
    {"jump", "kSPACE", L"Прыжок", 0},
    {"sprint", "kLSHIFT", L"Бег", 0},
    {"crouch", "kLCONTROL", L"Присесть", 0},
    {"crouch_toggle", "kZ", L"Присесть (переключатель)", 0},
    {"use", "kE", L"Использовать", 0},
    {"wpn_fire", "mouse0", L"Огонь", 1},
    {"wpn_aim", "mouse1", L"Прицел", 1},
    {"wpn_reload", "kR", L"Перезарядка", 1},
    {"wpn_knife", "mouse2", L"Нож", 1},
    {"wpn_fast_knife", "kV", L"Быстрый удар ножом", 1},
    {"wpn_dynamite", "kC", L"Динамит", 1},
    {"wpn_next", "kLBRACKET", L"Следующее оружие", 1},
    {"wpn_prev", "kRBRACKET", L"Предыдущее оружие", 1},
    {"popup_weapons", "kTAB", L"Выбор оружия", 1},
    {"wpn_2", "k1", nullptr, 1},
    {"wpn_3", "k2", nullptr, 1},
    {"wpn_4", "k4", nullptr, 1},
    {"wpn_5", "k5", nullptr, 1},
    {"wpn_light", "kF", L"Фонарь", 2},
    {"gasmask", "kG", L"Противогаз", 2},
    {"filter", "kT", L"Сменить фильтр", 2},
    {"medkit", "kQ", L"Аптечка", 2},
    {"nightvision", "kN", L"Ночное видение", 2},
    {"map", "kM", L"Карта", 2},
    {"special", "k3", nullptr, 3},
    {"time", "kY", nullptr, 3},
    {"accel", "kX", nullptr, 3},
    {"pause", "kPAUSE", L"Пауза", 3},
    {"console", "kGRAVE", L"Консоль", 3},
    {"retranslate", "kF5", nullptr, 3},
    {"cam_zoom_in", "kADD", nullptr, 3},
    {"cam_zoom_out", "kSUBTRACT", nullptr, 3},
    {"menu_enter", "kRETURN", nullptr, 3},
    {"changemenumap", "kESCAPE", nullptr, 3},
    {"up", "kUP", nullptr, 3},
    {"down", "kDOWN", nullptr, 3},
    {"left", "kLEFT", nullptr, 3},
    {"right", "kRIGHT", nullptr, 3},
};
const wchar_t* const kGroups[4] = {L"ДВИЖЕНИЕ", L"ОРУЖИЕ", L"СНАРЯЖЕНИЕ", L"ПРОЧЕЕ"};

// The keys as the game names them ("k" and the key's DirectInput name), by their DirectInput code, and
// how the launcher shows each. Esc is not here: it calls the game's menu and cancels a wait for a key.
const struct Key {
  int dik;
  const char* token;
  const wchar_t* label;
} kKeys[] = {
    {0x02, "k1", L"1"}, {0x03, "k2", L"2"}, {0x04, "k3", L"3"}, {0x05, "k4", L"4"}, {0x06, "k5", L"5"},
    {0x07, "k6", L"6"}, {0x08, "k7", L"7"}, {0x09, "k8", L"8"}, {0x0A, "k9", L"9"}, {0x0B, "k0", L"0"},
    {0x0C, "kMINUS", L"-"}, {0x0D, "kEQUALS", L"="}, {0x0E, "kBACK", L"Backspace"}, {0x0F, "kTAB", L"Tab"},
    {0x10, "kQ", L"Q"}, {0x11, "kW", L"W"}, {0x12, "kE", L"E"}, {0x13, "kR", L"R"}, {0x14, "kT", L"T"},
    {0x15, "kY", L"Y"}, {0x16, "kU", L"U"}, {0x17, "kI", L"I"}, {0x18, "kO", L"O"}, {0x19, "kP", L"P"},
    {0x1A, "kLBRACKET", L"["}, {0x1B, "kRBRACKET", L"]"}, {0x1C, "kRETURN", L"Enter"}, {0x1D, "kLCONTROL", L"Левый Ctrl"},
    {0x1E, "kA", L"A"}, {0x1F, "kS", L"S"}, {0x20, "kD", L"D"}, {0x21, "kF", L"F"}, {0x22, "kG", L"G"},
    {0x23, "kH", L"H"}, {0x24, "kJ", L"J"}, {0x25, "kK", L"K"}, {0x26, "kL", L"L"},
    {0x27, "kSEMICOLON", L";"}, {0x28, "kAPOSTROPHE", L"'"}, {0x29, "kGRAVE", L"`"}, {0x2A, "kLSHIFT", L"Левый Shift"},
    {0x2B, "kBACKSLASH", L"\\"},
    {0x2C, "kZ", L"Z"}, {0x2D, "kX", L"X"}, {0x2E, "kC", L"C"}, {0x2F, "kV", L"V"}, {0x30, "kB", L"B"},
    {0x31, "kN", L"N"}, {0x32, "kM", L"M"},
    {0x33, "kCOMMA", L","}, {0x34, "kPERIOD", L"."}, {0x35, "kSLASH", L"/"}, {0x36, "kRSHIFT", L"Правый Shift"},
    {0x37, "kMULTIPLY", L"Num *"}, {0x38, "kLMENU", L"Левый Alt"}, {0x39, "kSPACE", L"Пробел"}, {0x3A, "kCAPITAL", L"Caps Lock"},
    {0x3B, "kF1", L"F1"}, {0x3C, "kF2", L"F2"}, {0x3D, "kF3", L"F3"}, {0x3E, "kF4", L"F4"}, {0x3F, "kF5", L"F5"},
    {0x40, "kF6", L"F6"}, {0x41, "kF7", L"F7"}, {0x42, "kF8", L"F8"}, {0x43, "kF9", L"F9"}, {0x44, "kF10", L"F10"},
    {0x45, "kNUMLOCK", L"Num Lock"}, {0x46, "kSCROLL", L"Scroll Lock"},
    {0x47, "kNUMPAD7", L"Num 7"}, {0x48, "kNUMPAD8", L"Num 8"}, {0x49, "kNUMPAD9", L"Num 9"}, {0x4A, "kSUBTRACT", L"Num -"},
    {0x4B, "kNUMPAD4", L"Num 4"}, {0x4C, "kNUMPAD5", L"Num 5"}, {0x4D, "kNUMPAD6", L"Num 6"}, {0x4E, "kADD", L"Num +"},
    {0x4F, "kNUMPAD1", L"Num 1"}, {0x50, "kNUMPAD2", L"Num 2"}, {0x51, "kNUMPAD3", L"Num 3"}, {0x52, "kNUMPAD0", L"Num 0"},
    {0x53, "kDECIMAL", L"Num ."}, {0x57, "kF11", L"F11"}, {0x58, "kF12", L"F12"},
    {0x9C, "kNUMPADENTER", L"Num Enter"}, {0x9D, "kRCONTROL", L"Правый Ctrl"}, {0xB5, "kDIVIDE", L"Num /"},
    {0xB8, "kRMENU", L"Правый Alt"}, {0xC5, "kPAUSE", L"Pause"}, {0xC7, "kHOME", L"Home"}, {0xC8, "kUP", L"Стрелка вверх"},
    {0xC9, "kPRIOR", L"Page Up"}, {0xCB, "kLEFT", L"Стрелка влево"}, {0xCD, "kRIGHT", L"Стрелка вправо"}, {0xCF, "kEND", L"End"},
    {0xD0, "kDOWN", L"Стрелка вниз"}, {0xD1, "kNEXT", L"Page Down"}, {0xD2, "kINSERT", L"Insert"}, {0xD3, "kDELETE", L"Delete"},
};
const struct {
  const char* token;
  const wchar_t* label;
} kOther[] = {{"kESCAPE", L"Esc"}, {"mouse0", L"ЛКМ"}, {"mouse1", L"ПКМ"}, {"mouse2", L"Колесо (нажатие)"},
              {"mouse3", L"Мышь 4"}, {"mouse4", L"Мышь 5"}};

// A line with a knob: the variable, its ends, the step of the knob, the game's own value.
const struct Slide {
  Row row;
  const char* name;
  float lo, hi, step, def;
} kSlides[] = {
    {kFov, "r_base_fov", 45.0f, 90.0f, 0.625f, 50.625f},
    {kGamma, "r_gamma", 0.5f, 1.5f, 0.05f, 1.0f},
    {kSens, "sens", 0.1f, 2.0f, 0.05f, 0.4f},
    {kAim, "aim_assist", 0.0f, 1.0f, 0.05f, 1.0f},
};

struct Info {
  const wchar_t* label;
  const char* key;
  Kind kind;
  const char* name;   // its line of user.cfg
  const char* also;   // a second line that goes with it (may be null)
};
const Info kInfo[kRows] = {
    {L"Разрешение", "res", Kind::Choice, "r_res_hor", "r_res_vert"},
    {L"Режим экрана", "fullscreen", Kind::Choice, "r_fullscreen", nullptr},
    {L"Вертикальная синхронизация", "vsync", Kind::Switch, "r_vsync", nullptr},
    {L"Качество", "quality", Kind::Choice, "r_quality_level", nullptr},
    {L"Сглаживание SSAA", "ssaa", Kind::Choice, "r_supersample", "r_enum_ssaa"},
    {L"Фильтрация текстур", "af", Kind::Choice, "r_af_level", nullptr},
    {L"Тесселяция", "tess", Kind::Choice, "r_dx11_tess", "r_tess_ss"},
    {L"Размытие в движении", "blur", Kind::Choice, "r_blur_level", nullptr},
    {L"Поле зрения", "fov", Kind::Slider, "r_base_fov", nullptr},
    {L"Гамма", "gamma", Kind::Slider, "r_gamma", nullptr},
    {L"Чувствительность мыши", "sens", Kind::Slider, "sens", nullptr},
    {L"Инверсия мыши по вертикали", "invert", Kind::Switch, "invert_y_axis", nullptr},
    {L"Помощь в прицеливании", "aim", Kind::Slider, "aim_assist", nullptr},
    {L"Вибрация геймпада", "vibration", Kind::Choice, "vibration", nullptr},
};

// SSAA as the game's list has it: tenths of the factor for r_enum_ssaa, the factor for r_supersample.
const struct {
  int tenths;
  const char* factor;
  const wchar_t* label;
} kSsaaList[] = {{10, "1.", L"Выкл"}, {5, "0.5", L"0.5x"}, {20, "2.", L"2x"}, {30, "3.", L"3x"}, {40, "4.", L"4x"}};

struct Line {
  std::string text, eol;
};
struct Bind {
  std::string action, key;
};
struct Mode {
  int w, h;
};
struct Page {
  std::wstring name;
  std::vector<int> rows;
};

struct State {
  std::wstring dir, path;
  bool present = false, canned = false;
  std::vector<Line> lines;
  std::map<std::string, std::string> first;  // what the file had when it was first read: name -> value, "bind <action>" -> key
  std::wstring first_path;
  bool first_read = false;
  std::vector<Bind> binds;
  std::vector<Mode> modes;
  std::vector<Page> pages;
  std::wstring error;
};
State g;

// What a picture of the pages shows (the game's own values, on a common screen).
const char kCanned[] =
    "aim_assist 1.\r\nunbindall\r\nbind changemenumap kESCAPE\r\nbind wpn_2 k1\r\nbind wpn_3 k2\r\nbind special k3\r\nbind wpn_4 k4\r\n"
    "bind wpn_5 k5\r\nbind popup_weapons kTAB\r\nbind medkit kQ\r\nbind forward kW\r\nbind use kE\r\nbind wpn_reload kR\r\n"
    "bind filter kT\r\nbind time kY\r\nbind wpn_next kLBRACKET\r\nbind wpn_prev kRBRACKET\r\nbind menu_enter kRETURN\r\n"
    "bind crouch kLCONTROL\r\nbind lstrafe kA\r\nbind back kS\r\nbind rstrafe kD\r\nbind wpn_light kF\r\nbind gasmask kG\r\n"
    "bind console kGRAVE\r\nbind sprint kLSHIFT\r\nbind crouch_toggle kZ\r\nbind accel kX\r\nbind wpn_dynamite kC\r\n"
    "bind wpn_fast_knife kV\r\nbind nightvision kN\r\nbind map kM\r\nbind jump kSPACE\r\nbind retranslate kF5\r\n"
    "bind cam_zoom_out kSUBTRACT\r\nbind cam_zoom_in kADD\r\nbind pause kPAUSE\r\nbind up kUP\r\nbind left kLEFT\r\n"
    "bind right kRIGHT\r\nbind down kDOWN\r\nbind wpn_fire mouse0\r\nbind wpn_aim mouse1\r\nbind wpn_knife mouse2\r\n"
    "invert_y_axis off\r\nr_af_level 1\r\nr_base_fov 50.625\r\nr_blur_level 1\r\nr_dx11_tess 1\r\nr_enum_ssaa 10\r\n"
    "r_fullscreen on\r\nr_gamma 1.\r\nr_quality_level 2\r\nr_res_hor 1920\r\nr_res_vert 1080\r\nr_supersample 1.\r\n"
    "r_tess_ss 0\r\nr_vsync off\r\nsens 0.4\r\nvibration 2\r\n";

std::vector<Line> split(const std::string& raw) {
  std::vector<Line> lines;
  size_t p = 0;
  while (p < raw.size()) {
    const size_t e = raw.find_first_of("\r\n", p);
    if (e == std::string::npos) {
      lines.push_back({raw.substr(p), ""});
      break;
    }
    const bool two = raw[e] == '\r' && e + 1 < raw.size() && raw[e + 1] == '\n';
    lines.push_back({raw.substr(p, e - p), raw.substr(e, two ? 2 : 1)});
    p = e + (two ? 2 : 1);
  }
  return lines;
}

std::string word(const std::string& s, size_t* rest = nullptr) {  // the first word of a line; where the next one begins
  const size_t a = s.find_first_not_of(" \t");
  if (a == std::string::npos) return "";
  size_t e = s.find_first_of(" \t", a);
  if (e == std::string::npos) e = s.size();
  if (rest) {
    const size_t n = s.find_first_not_of(" \t", e);
    *rest = n == std::string::npos ? s.size() : n;
  }
  return s.substr(a, e - a);
}

std::string trim_end(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
  return s;
}

int find(const std::vector<Line>& lines, const std::string& name) {
  for (size_t i = 0; i < lines.size(); ++i)
    if (word(lines[i].text) == name) return static_cast<int>(i);
  return -1;
}

std::string get(const std::string& name) {
  const int i = find(g.lines, name);
  if (i < 0) return "";
  size_t rest = 0;
  word(g.lines[i].text, &rest);
  return trim_end(g.lines[i].text.substr(rest));
}

bool has(const char* name) { return find(g.lines, name) >= 0; }

// "bind <action> <key>" -> the action and the key; false: not such a line.
bool bind_of(const std::string& line, Bind& b) {
  size_t rest = 0;
  if (word(line, &rest) != "bind") return false;
  const std::string tail = line.substr(rest);
  size_t rest2 = 0;
  b.action = word(tail, &rest2);
  b.key = word(tail.substr(rest2));
  return !b.action.empty() && !b.key.empty();
}

const Action* known(const std::string& action) {
  for (const Action& a : kActions)
    if (action == a.name) return &a;
  return nullptr;
}

bool on(const std::string& v) { return v == "on" || v == "1" || v == "true" || v == "yes"; }

// A number as the game writes one: "50.625", "0.4", and "1." for a whole one.
std::string number(float v) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(v));
  std::string s = buf;
  while (!s.empty() && s.back() == '0') s.pop_back();
  return s;
}

const Slide* slide_of(Row r) {
  for (const Slide& s : kSlides)
    if (s.row == r) return &s;
  return nullptr;
}

std::wstring user_file() {
  if (!g_user_dir.empty()) return core::join(g_user_dir, kFile);
  if (core::g_scripted) return L"";  // (a test never reads the player's own file)
  PWSTR local = nullptr;
  std::wstring dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) dir = local;
  CoTaskMemFree(local);
  return dir.empty() ? dir : core::join(core::join(dir, L"4A Games\\Metro 2033"), kFile);
}

void make_modes() {
  std::vector<Mode> modes;
  if (g.canned) {
    modes = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}};
  } else {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); ++i)
      if (dm.dmBitsPerPel == 32 && dm.dmPelsWidth >= 800 && dm.dmPelsHeight >= 600)
        modes.push_back({static_cast<int>(dm.dmPelsWidth), static_cast<int>(dm.dmPelsHeight)});
  }
  // What the file has is always in the list, and what it had when the launcher first read it: a size of the
  // player's own that no monitor offers can be come back to.
  auto own = [&](int w, int h) {
    if (w >= 320 && h >= 200 && w <= 16384 && h <= 16384) modes.push_back({w, h});
  };
  own(atoi(get("r_res_hor").c_str()), atoi(get("r_res_vert").c_str()));
  if (g.first_read && g.first_path == g.path && g.first.count("r_res_hor") && g.first.count("r_res_vert"))
    own(atoi(g.first["r_res_hor"].c_str()), atoi(g.first["r_res_vert"].c_str()));
  std::sort(modes.begin(), modes.end(), [](const Mode& a, const Mode& b) {
    return a.w * a.h < b.w * b.h || (a.w * a.h == b.w * b.h && a.w < b.w);
  });
  modes.erase(std::unique(modes.begin(), modes.end(), [](const Mode& a, const Mode& b) { return a.w == b.w && a.h == b.h; }), modes.end());
  g.modes = modes;
}

void make_pages() {
  g.pages.clear();
  for (int group = 0; group < 4; ++group) {
    std::vector<int> rows;
    for (const Action& a : kActions)  // (the known ones in the launcher's order)
      if (a.group == group)
        for (size_t i = 0; i < g.binds.size(); ++i)
          if (g.binds[i].action == a.name) rows.push_back(static_cast<int>(i));
    if (group == 3)  // (and what the launcher has never heard of, as the file has it)
      for (size_t i = 0; i < g.binds.size(); ++i)
        if (!known(g.binds[i].action)) rows.push_back(static_cast<int>(i));
    for (size_t at = 0, n = 1; at < rows.size(); at += kPageRows, ++n) {
      Page p;
      p.name = kGroups[group] + (n > 1 ? L" · " + std::to_wstring(n) : std::wstring());
      p.rows.assign(rows.begin() + at, rows.begin() + std::min(rows.size(), at + kPageRows));
      g.pages.push_back(p);
    }
  }
}

// The lines are in g.lines: the keys, the list of screens, the pages; the first reading is remembered.
void parse() {
  g.binds.clear();
  for (const Line& l : g.lines) {
    Bind b;
    if (bind_of(l.text, b)) g.binds.push_back(b);
  }
  make_modes();
  make_pages();
  if (!g.first_read || g.first_path != g.path) {
    g.first.clear();
    for (const Line& l : g.lines) {
      Bind b;
      size_t rest = 0;
      const std::string name = word(l.text, &rest);
      if (bind_of(l.text, b)) g.first["bind " + b.action] = b.key;
      else if (!name.empty()) g.first[name] = trim_end(l.text.substr(rest));
    }
    g.first_read = true;
    g.first_path = g.path;
  }
}

void read() {
  g.lines.clear();
  g.present = false;
  if (g.canned) {
    g.lines = split(kCanned);
    g.present = true;
  } else if (!g.path.empty() && core::exists(g.path)) {
    g.lines = split(core::read_all(g.path));
    g.present = !g.lines.empty();
  }
  if (!g.present) {
    g.lines.clear();
    g.binds.clear();
    g.modes.clear();
    g.pages.clear();
    return;
  }
  parse();
}

bool write_file(const std::wstring& path, const std::vector<Line>& lines) {
  std::string bytes;
  for (const Line& l : lines) bytes += l.text + l.eol;
  const std::wstring tmp = path + L".metronet-tmp";  // (written beside and put in its place whole)
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD put = 0;
  const bool ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &put, nullptr) && put == bytes.size();
  CloseHandle(h);
  if (ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
  const DWORD err = GetLastError();
  DeleteFileW(tmp.c_str());
  SetLastError(err);
  return false;
}

// The n-th "bind <action> ..." line of the lines (-1: not there).
int bind_line(const std::vector<Line>& lines, const std::string& action, int nth) {
  for (size_t i = 0; i < lines.size(); ++i) {
    Bind b;
    if (bind_of(lines[i].text, b) && b.action == action && nth-- == 0) return static_cast<int>(i);
  }
  return -1;
}

using Values = std::vector<std::pair<std::string, std::string>>;  // name -> value
using Keys = std::vector<std::pair<int, std::string>>;            // a key's row -> its new key

// The change goes into the file as it is on the disk at this moment: only the lines named are rewritten.
bool apply(const Values& values, const Keys& keys) {
  g.error.clear();
  if (!g.present) {
    g.error = L"Сначала один раз запустите игру: файла её настроек (user.cfg) ещё нет.";
    return false;
  }
  // (which line of the file each key's row is: the n-th line of its action)
  std::vector<std::pair<std::pair<std::string, int>, std::string>> rebinds;
  for (const auto& k : keys) {
    int nth = 0;
    for (int i = 0; i < k.first; ++i) nth += g.binds[i].action == g.binds[k.first].action;
    rebinds.push_back({{g.binds[k.first].action, nth}, k.second});
  }
  std::vector<Line> lines;
  if (g.canned) {
    lines = g.lines;
  } else {
    if (core::game_process_running(g.dir)) {
      g.error = L"Игра запущена: пока она открыта, её настройки не меняются — при выходе она запишет user.cfg заново. Закройте игру.";
      return false;
    }
    if (!core::exists(g.path)) {
      read();
      g.error = L"Сначала один раз запустите игру: файла её настроек (user.cfg) ещё нет.";
      return false;
    }
    lines = split(core::read_all(g.path));
  }
  bool any = false;
  for (const auto& v : values) {
    const int i = find(lines, v.first);
    if (i < 0) continue;  // (a line the file does not have is not added)
    const std::string text = v.first + " " + v.second;
    any = any || lines[i].text != text;
    lines[i].text = text;
  }
  for (const auto& r : rebinds) {
    const int i = bind_line(lines, r.first.first, r.first.second);
    if (i < 0) continue;
    const std::string text = "bind " + r.first.first + " " + r.second;
    any = any || lines[i].text != text;
    lines[i].text = text;
  }
  if (!g.canned && any) {
    const std::wstring backup = g.path.substr(0, g.path.size() - wcslen(kFile)) + kBackup;
    CopyFileW(g.path.c_str(), backup.c_str(), TRUE);  // (once: a copy that is there already is never written over)
    if (!write_file(g.path, lines)) {
      const DWORD err = GetLastError();
      read();
      g.error = L"user.cfg не записался (ошибка " + std::to_wstring(err) + L").";
      return false;
    }
  }
  if (g.canned) {
    g.lines = lines;
    parse();
  } else {
    read();
  }
  return true;
}

}  // namespace

void load(const std::wstring& game_dir) {
  g.canned = false;
  g.dir = game_dir;
  g.path = user_file();
  g.error.clear();
  read();
}

void load_canned() {
  g = State();
  g.canned = true;
  read();
}

bool has_file() { return g.present; }
std::wstring file() { return g.path; }
const std::wstring& error() { return g.error; }

Kind kind(Row r) { return kInfo[r].kind; }
const wchar_t* label(Row r) { return kInfo[r].label; }
const char* key(Row r) { return kInfo[r].key; }

bool usable(Row r) {
  if (!g.present || !has(kInfo[r].name)) return false;
  return r != kResolution || has(kInfo[r].also);
}

bool changed(Row r) {
  if (!usable(r)) return false;
  for (const char* name : {kInfo[r].name, kInfo[r].also}) {
    if (!name || !has(name)) continue;
    auto was = g.first.find(name);
    if (was != g.first.end() && was->second != get(name)) return true;
  }
  return false;
}

std::string raw(Row r) {
  std::string out = std::string(kInfo[r].name) + " " + get(kInfo[r].name);
  if (kInfo[r].also && has(kInfo[r].also)) out += std::string(" | ") + kInfo[r].also + " " + get(kInfo[r].also);
  return out;
}

int count(Row r) {
  switch (r) {
    case kResolution: return static_cast<int>(g.modes.size());
    case kQuality: return 4;
    case kSsaa: return 5 + (atoi(get("r_enum_ssaa").c_str()) == 255 ? 1 : 0);  // ("auto" only when the file has it)
    case kAf: return 2;
    case kTess: return 4;
    case kBlur: return 2;
    case kVibration: return 4;
    case kFullscreen: return 3;
    default: return kInfo[r].kind == Kind::Switch ? 2 : 0;
  }
}

int value(Row r) {
  if (!usable(r)) return 0;
  const std::string v = get(kInfo[r].name);
  switch (r) {
    case kResolution: {
      const int w = atoi(v.c_str()), h = atoi(get("r_res_vert").c_str());
      for (size_t i = 0; i < g.modes.size(); ++i)
        if (g.modes[i].w == w && g.modes[i].h == h) return static_cast<int>(i);
      return 0;
    }
    case kSsaa: {
      if (atoi(get("r_enum_ssaa").c_str()) == 255) return 5;
      const double f = atof(v.c_str());
      int at = 0;
      for (int i = 1; i < 5; ++i)
        if (std::fabs(atof(kSsaaList[i].factor) - f) < std::fabs(atof(kSsaaList[at].factor) - f)) at = i;
      return at;
    }
    case kTess:
      return atoi(v.c_str()) == 0 ? 0 : 1 + std::clamp(atoi(get("r_tess_ss").c_str()), 0, 2);
    case kFullscreen:  // the game's framed window, the mod's frameless one over the monitor, the game's full screen
      return on(v) ? 2 : (mset::get().borderless ? 1 : 0);
    default:
      if (kInfo[r].kind == Kind::Switch) return on(v) ? 1 : 0;
      return std::clamp(atoi(v.c_str()), 0, std::max(count(r) - 1, 0));
  }
}

std::wstring name(Row r, int v) {
  if (v < 0 || v >= count(r)) return L"";
  static const wchar_t* const quality[4] = {L"Низкое", L"Среднее", L"Высокое", L"Очень высокое"};
  static const wchar_t* const tess[4] = {L"Выкл", L"Обычная", L"Высокая", L"Очень высокая"};
  static const wchar_t* const vibration[4] = {L"Выкл", L"Слабая", L"Средняя", L"Сильная"};
  switch (r) {
    case kResolution: return std::to_wstring(g.modes[v].w) + L" × " + std::to_wstring(g.modes[v].h);
    case kQuality: return quality[v];
    case kSsaa: return v == 5 ? L"Авто" : kSsaaList[v].label;
    case kAf: return v ? L"AF 16x" : L"AF 4x";
    case kTess: return tess[v];
    case kBlur: return v ? L"Обычное" : L"Выкл";
    case kVibration: return vibration[v];
    case kFullscreen: return v == 0 ? L"В окне" : v == 1 ? L"Без рамки" : L"Во весь экран";
    default: return v ? L"ВКЛ" : L"ВЫКЛ";
  }
}

bool set(Row r, int v) {
  const int n = count(r);
  if (!usable(r) || n <= 0) {
    g.error = g.present ? L"В user.cfg нет этой строки: игра её не записала." : L"Сначала один раз запустите игру: файла её настроек (user.cfg) ещё нет.";
    return false;
  }
  v = ((v % n) + n) % n;
  Values out;
  switch (r) {
    case kResolution:
      out = {{"r_res_hor", std::to_string(g.modes[v].w)}, {"r_res_vert", std::to_string(g.modes[v].h)}};
      break;
    case kSsaa:
      if (v == 5) return true;  // ("auto" is the game's to choose: the launcher only leaves it)
      out = {{"r_enum_ssaa", std::to_string(kSsaaList[v].tenths)}, {"r_supersample", kSsaaList[v].factor}};
      break;
    case kTess:
      out = {{"r_dx11_tess", v ? "1" : "0"}, {"r_tess_ss", std::to_string(v > 1 ? v - 1 : 0)}};
      break;
    case kFullscreen:
      if (mset::has_ini() && !mset::set_borderless(v == 1)) {
        g.error = mset::error();
        return false;
      }
      out = {{"r_fullscreen", v == 2 ? "on" : "off"}};
      break;
    default:
      if (kInfo[r].kind == Kind::Switch) out = {{kInfo[r].name, v ? "on" : "off"}};
      else out = {{kInfo[r].name, std::to_string(v)}};
      break;
  }
  return apply(out, {});
}

float fraction(Row r) {
  const Slide* s = slide_of(r);
  if (!s || !usable(r)) return 0;
  return std::clamp((static_cast<float>(atof(get(s->name).c_str())) - s->lo) / (s->hi - s->lo), 0.0f, 1.0f);
}

float default_fraction(Row r) {
  const Slide* s = slide_of(r);
  return s ? (s->def - s->lo) / (s->hi - s->lo) : -1;
}

std::wstring text(Row r) {
  const Slide* s = slide_of(r);
  if (!s || !usable(r)) return L"—";
  wchar_t buf[32];
  const double v = atof(get(s->name).c_str());
  if (r == kFov) swprintf(buf, 32, std::fabs(v - std::round(v)) < 0.0005 ? L"%.0f" : L"%.3f", v);
  else swprintf(buf, 32, L"%.2f", v);
  std::wstring out = buf;
  if (r == kFov && out.find(L'.') != std::wstring::npos)
    while (out.back() == L'0') out.pop_back();
  for (wchar_t& ch : out)
    if (ch == L'.') ch = L',';
  return out;
}

bool slide(Row r, float f) {
  const Slide* s = slide_of(r);
  if (!s || !usable(r)) {
    g.error = g.present ? L"В user.cfg нет этой строки: игра её не записала." : L"Сначала один раз запустите игру: файла её настроек (user.cfg) ещё нет.";
    return false;
  }
  const int steps = static_cast<int>(std::lround((s->hi - s->lo) / s->step));
  const int at = static_cast<int>(std::lround(std::clamp(f, 0.0f, 1.0f) * steps));
  const std::string v = number(s->lo + at * s->step);
  if (v == get(s->name)) return true;
  return apply({{s->name, v}}, {});
}

// --- the keys -----------------------------------------------------------------------------------------

int binds() { return static_cast<int>(g.binds.size()); }
const std::string& action(int i) { return g.binds[i].action; }
const std::string& bound(int i) { return g.binds[i].key; }

std::wstring action_label(int i, bool* raw_name) {
  const Action* a = known(g.binds[i].action);
  if (raw_name) *raw_name = !(a && a->label);
  return a && a->label ? std::wstring(a->label) : core::wide(g.binds[i].action);
}

std::wstring key_label(const std::string& token) {
  for (const Key& k : kKeys)
    if (token == k.token) return k.label;
  for (const auto& k : kOther)
    if (token == k.token) return k.label;
  return core::wide(token);  // (a name the launcher does not know: as the file has it)
}

bool bind_changed(int i) {
  auto was = g.first.find("bind " + g.binds[i].action);
  return was != g.first.end() && was->second != g.binds[i].key;
}

bool bind_is_default(int i) {
  const Action* a = known(g.binds[i].action);
  return !a || g.binds[i].key == a->key;
}

bool bind(int i, const std::string& token) {
  if (i < 0 || i >= binds() || token.empty()) return false;
  const std::string old = g.binds[i].key;
  if (old == token) {
    g.error.clear();
    return true;
  }
  Keys keys = {{i, token}};
  for (int j = 0; j < binds(); ++j)  // (a key does one thing: the action that had it takes the old one)
    if (j != i && g.binds[j].key == token) keys.push_back({j, old});
  return apply({}, keys);
}

bool bind_reset(int i) {
  const Action* a = i >= 0 && i < binds() ? known(g.binds[i].action) : nullptr;
  return a ? bind(i, a->key) : false;
}

std::string token_of_key(int dik) {
  for (const Key& k : kKeys)
    if (k.dik == dik) return k.token;
  return "";
}

std::string token_of_mouse(int button) { return button >= 0 && button <= 4 ? "mouse" + std::to_string(button) : std::string(); }

int pages() { return static_cast<int>(g.pages.size()); }
std::wstring page_name(int p) { return p >= 0 && p < pages() ? g.pages[p].name : std::wstring(); }
const std::vector<int>& page(int p) {
  static const std::vector<int> none;
  return p >= 0 && p < pages() ? g.pages[p].rows : none;
}

}  // namespace gcfg
}  // namespace lz
