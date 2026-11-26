#include "mod/config.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>

namespace {

// The value as written (no lower-casing: console commands and paths keep their letters).
std::wstring raw(const std::wstring& path, const wchar_t* section, const wchar_t* key) {
  std::wstring buf(8192, L'\0');
  DWORD n = GetPrivateProfileStringW(section, key, L"", buf.data(), static_cast<DWORD>(buf.size()), path.c_str());
  buf.resize(n);
  return buf;
}

std::string narrow(const std::wstring& w) {
  std::string s;
  for (wchar_t c : w) s += static_cast<char>(c < 128 ? c : '?');
  return s;
}

std::string get(const std::wstring& path, const wchar_t* section, const wchar_t* key, const char* fallback) {
  std::string s = narrow(raw(path, section, key));
  if (!s.empty() && s[0] == ';') s.clear();  // "key =      ; a comment": no value
  size_t c = s.find(" ;");
  if (c != std::string::npos) s.erase(c);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
  return s.empty() ? fallback : s;
}

// A value that may hold any letters, as UTF-8. Notepad saves the file as UTF-8 or in the system's code page;
// the bytes are taken as they are when they are valid UTF-8 and converted from the code page otherwise.
std::string get_text(const std::wstring& path, const char* section, const char* key) {
  char buf[512] = {};
  char file[MAX_PATH * 3] = {};
  WideCharToMultiByte(CP_ACP, 0, path.c_str(), -1, file, sizeof(file), nullptr, nullptr);
  GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), file);
  std::string s = buf;
  if (!s.empty() && s[0] == ';') s.clear();
  size_t c = s.find(" ;");
  if (c != std::string::npos) s.erase(c);
  while (!s.empty() && (s.back() == ' ' || s.back() == '	')) s.pop_back();
  if (s.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), -1, nullptr, 0) > 0) return s;
  wchar_t wide[512] = {};
  MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, wide, 512);
  char utf8[1024] = {};
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, sizeof(utf8), nullptr, nullptr);
  return utf8;
}

int get_int(const std::wstring& path, const wchar_t* section, const wchar_t* key, int fallback) {
  std::string s = get(path, section, key, "");
  return s.empty() ? fallback : static_cast<int>(strtol(s.c_str(), nullptr, 0));
}

bool get_bool(const std::wstring& path, const wchar_t* section, const wchar_t* key, bool fallback) {
  std::string s = get(path, section, key, "");
  if (s.empty()) return fallback;
  return s == "1" || s == "true" || s == "yes" || s == "on";
}

std::string trim(std::string s) {
  size_t a = s.find_first_not_of(" \t");
  size_t b = s.find_last_not_of(" \t");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

}  // namespace

std::wstring module_dir() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring s = path;
  size_t slash = s.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L"" : s.substr(0, slash + 1);
}

bool load_config(const std::wstring& path, Config& cfg) {
  bool have = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
  cfg.log_level = tlog::parse_level(get(path, L"log", L"level", "info"), tlog::Info);
  cfg.log_engine = get_bool(path, L"log", L"engine", true);

  // No file, no line: the game sorts the roles out by itself. A test run of the lab without a mode is a plain
  // single-player one.
  cfg.lab = get_bool(path, L"lab", L"enabled", false);
  std::string mode = get(path, L"net", L"mode", cfg.lab ? "off" : "auto");
  cfg.net_mode = mode == "host" ? Config::NetMode::Host
                 : mode == "join" ? Config::NetMode::Join
                 : mode == "off" ? Config::NetMode::Off : Config::NetMode::Auto;
  cfg.net_address = get(path, L"net", L"address", "");
  cfg.net_spawn_delay = get_int(path, L"net", L"body_delay", 2);
  cfg.net_timeout = get_int(path, L"net", L"timeout", 45);
  cfg.net_tree_lock = get_int(path, L"net", L"tree_lock", 1) != 0;
  cfg.net_friendly_fire = get_int(path, L"net", L"friendly_fire", 0) != 0;
  cfg.net_ride = get_int(path, L"net", L"ride", 1) != 0;
  cfg.net_zones = get_int(path, L"net", L"zones", 1) != 0;
  cfg.screenshot = get_int(path, L"video", L"print_screen", 1) != 0;
  cfg.net_menu = get_int(path, L"net", L"menu", 1) != 0;
  cfg.borderless = get_int(path, L"net", L"borderless", 0) != 0;
  cfg.net_zone_own = get_int(path, L"net", L"zone_own", 1) != 0;
  cfg.net_use_scene_own = get_int(path, L"net", L"use_scene", 1) != 0;
  cfg.net_drive = get_int(path, L"net", L"drive", 1) != 0;
  cfg.net_use_on_joined = get_int(path, L"net", L"use_on_joined", 0) != 0;
  cfg.net_use_on_host = get_int(path, L"net", L"use_on_host2", 1) != 0;
  cfg.net_actors = get_int(path, L"net", L"actors", 1) != 0;
  cfg.net_use = get_int(path, L"net", L"use", 1) != 0;
  cfg.net_missing_target = get_int(path, L"net", L"missing_target", 1) != 0;
  cfg.net_steam = get_int(path, L"net", L"steam", 1) != 0;
  cfg.net_names = get_int(path, L"net", L"names", 1) != 0;
  cfg.net_revive = get_int(path, L"net", L"revive", 1) != 0;
  cfg.net_motions = get_int(path, L"net", L"motions", 1) != 0;
  cfg.net_motions_back = get_int(path, L"net", L"motions_back2", 1) != 0;
  cfg.net_loot = get_int(path, L"net", L"loot", 1) != 0;
  // (0.4.3: the keys of 0.4.1 - "hide_hands = 1", "scene_hands = 1" stayed in the players' metronet.ini - switch
  // nothing on any more: both took a player's hands away and nothing could be used or opened)
  cfg.net_hide_hands = get_int(path, L"net", L"hide_hands_lab", 0);
  cfg.net_scene_hands = get_int(path, L"net", L"scene_hands_lab", 0) != 0;
  cfg.net_physics = get_int(path, L"net", L"physics", 1) != 0;
  cfg.net_own_gear = get_int(path, L"net", L"own_gear", 1) != 0;
  cfg.net_name = get_text(path, "net", "name");
  if (cfg.net_name.size() > 48) cfg.net_name.resize(48);
  // No address: a Steam friend's game (before 0.2.0 the mode was simply off).
  if (cfg.net_mode == Config::NetMode::Join && cfg.net_address.empty()) cfg.net_address = "steam";

  cfg.lab = get_bool(path, L"lab", L"enabled", false);
  cfg.lab_userdir = raw(path, L"lab", L"userdir");
  cfg.lab_quit_after = get_int(path, L"lab", L"quit_after", 0);
  cfg.lab_steam = get_int(path, L"lab", L"steam", 0);
  cfg.lab_bind_port = get_int(path, L"lab", L"bind_port", -1);
  cfg.lab_autostart = get_bool(path, L"lab", L"autostart", true);
  cfg.lab_fps = get_int(path, L"lab", L"fps", 30);
  cfg.lab_desk = get_int(path, L"lab", L"desk", 0) != 0;
  cfg.lab_cores = get_int(path, L"lab", L"cores", 4);
  // script = 2: map 2033\l00_intro | 30: quit
  std::string script = get(path, L"lab", L"script", "");
  size_t pos = 0;
  while (pos < script.size()) {
    size_t bar = script.find('|', pos);
    std::string part = trim(script.substr(pos, bar == std::string::npos ? std::string::npos : bar - pos));
    pos = bar == std::string::npos ? script.size() : bar + 1;
    Config::Step st;
    size_t colon = part.find(':');
    std::string when = colon == std::string::npos ? "" : trim(part.substr(0, colon));
    bool timed = !when.empty() && when.find_first_not_of("+0123456789.") == std::string::npos;
    if (timed) {
      st.relative = when[0] == '+';
      st.at = strtod(when.c_str() + (st.relative ? 1 : 0), nullptr);
      st.cmd = trim(part.substr(colon + 1));
    } else {  // no time: right after the step before
      st.relative = true;
      st.cmd = part;
    }
    if (!st.cmd.empty()) cfg.script.push_back(st);
  }
  return have;
}
