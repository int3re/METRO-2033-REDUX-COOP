// The mod's settings the launcher edits: see modset.h. The file is read and written here line by line,
// with the rules the mod itself reads it by (src/mod/config.cpp): the first [net] section, the first line
// of a key, a value ends at " ;" (a comment), a value that begins with ";" is no value. The player's name
// is UTF-8 in the file (a name left there in the system's code page is read as the mod reads it).
#include "launcher/modset.h"

#include <windows.h>

#include <cstdlib>
#include <vector>

#include "launcher/launcher.h"

namespace lz {
namespace mset {
namespace {

const wchar_t kIni[] = L"metronet.ini";
const char kSection[] = "net";

struct Line {
  std::string text, eol;
};
struct File {
  bool utf16 = false, bom = false;  // how the bytes were written: kept so
  std::vector<Line> lines;
  std::string eol = "\r\n";  // of a line that is added
};

struct State {
  std::wstring dir, ini;
  bool present = false, canned = false;
  Values v, first;
  std::wstring first_ini;
  std::wstring error;
  bool kept = false;  // a copy of the file was put aside before this run's first change
};
State g;

std::string trim(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

bool read_file(const std::wstring& path, File& f) {
  if (!core::exists(path)) return false;
  std::string raw = core::read_all(path);
  if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFF && static_cast<unsigned char>(raw[1]) == 0xFE) {
    f.utf16 = true;
    raw = core::narrow(std::wstring(reinterpret_cast<const wchar_t*>(raw.data() + 2), (raw.size() - 2) / sizeof(wchar_t)));
  } else if (raw.size() >= 3 && raw.compare(0, 3, "\xEF\xBB\xBF") == 0) {
    f.bom = true;
    raw.erase(0, 3);
  }
  size_t p = 0;
  bool crlf = false, lf = false;
  while (p < raw.size()) {
    size_t e = raw.find_first_of("\r\n", p);
    if (e == std::string::npos) {
      f.lines.push_back({raw.substr(p), ""});
      break;
    }
    const bool two = raw[e] == '\r' && e + 1 < raw.size() && raw[e + 1] == '\n';
    f.lines.push_back({raw.substr(p, e - p), raw.substr(e, two ? 2 : 1)});
    (two ? crlf : lf) = true;
    p = e + (two ? 2 : 1);
  }
  if (lf && !crlf) f.eol = "\n";
  return true;
}

bool write_file(const std::wstring& path, const File& f) {
  std::string text;
  for (const Line& l : f.lines) text += l.text + l.eol;
  std::string bytes;
  if (f.utf16) {
    const std::wstring w = core::wide(text);
    bytes = "\xFF\xFE" + std::string(reinterpret_cast<const char*>(w.data()), w.size() * sizeof(wchar_t));
  } else {
    bytes = (f.bom ? "\xEF\xBB\xBF" : "") + text;
  }
  // (written beside and put in its place whole: a launcher closed mid-way leaves the old file)
  const std::wstring tmp = path + L".tmp";
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

// "[net]" -> "net"; empty: the line opens no section.
std::string section_of(const std::string& line) {
  const std::string t = trim(line);
  if (t.size() < 2 || t[0] != '[') return "";
  const size_t close = t.find(']');
  return close == std::string::npos ? std::string() : lower(trim(t.substr(1, close - 1)));
}

// "names = 1 ; ..." -> "names"; empty: a comment, a blank, a line without "=".
std::string key_of(const std::string& line) {
  const std::string t = trim(line);
  if (t.empty() || t[0] == ';' || t[0] == '[') return "";
  const size_t eq = t.find('=');
  return eq == std::string::npos ? std::string() : lower(trim(t.substr(0, eq)));
}

// Where the comment of a line's value begins, counted from just after the "=" (npos: there is none).
size_t comment_at(const std::string& rest) {
  const size_t first = rest.find_first_not_of(" \t");
  if (first != std::string::npos && rest[first] == ';') return first;
  const size_t p = rest.find(" ;");
  return p == std::string::npos ? p : p + 1;
}

struct Where {
  int line = -1;      // the key's line
  int insert = -1;    // where a new line of the section goes (-1: the file has no such section)
};

Where find(const File& f, const char* key) {
  Where w;
  bool in = false, seen = false;
  for (size_t i = 0; i < f.lines.size(); ++i) {
    const std::string sec = section_of(f.lines[i].text);
    if (!sec.empty() || trim(f.lines[i].text).rfind("[", 0) == 0) {
      if (in) break;  // (the first section of the name is the one that counts)
      in = !seen && sec == kSection;
      if (in) {
        seen = true;
        w.insert = static_cast<int>(i) + 1;
      }
      continue;
    }
    if (!in) continue;
    if (!trim(f.lines[i].text).empty()) w.insert = static_cast<int>(i) + 1;
    if (w.line < 0 && key_of(f.lines[i].text) == key) w.line = static_cast<int>(i);
  }
  return w;
}

std::string value_of(const File& f, const char* key) {
  const Where w = find(f, key);
  if (w.line < 0) return "";
  const std::string& line = f.lines[w.line].text;
  std::string rest = line.substr(line.find('=') + 1);
  const size_t c = comment_at(rest);
  if (c != std::string::npos) rest.resize(c);
  return trim(rest);
}

size_t columns(const std::string& utf8) {  // letters, not bytes
  size_t n = 0;
  for (unsigned char ch : utf8) n += (ch & 0xC0) != 0x80;
  return n;
}

void put_value(File& f, const char* key, const std::string& value) {
  const Where w = find(f, key);
  if (w.line >= 0) {
    std::string& line = f.lines[w.line].text;
    const size_t eq = line.find('=');
    const std::string rest = line.substr(eq + 1);
    const size_t c = comment_at(rest);
    std::string out = line.substr(0, eq + 1) + (value.empty() ? "" : " " + value);
    if (c != std::string::npos) {  // the comment stays in its column
      const size_t col = columns(line.substr(0, eq + 1 + c)), have = columns(out);
      out += std::string(col > have + 2 ? col - have : 2, ' ') + rest.substr(c);
    }
    line = out;
    return;
  }
  const std::string text = std::string(key) + " =" + (value.empty() ? "" : " " + value);
  if (w.insert >= 0) {
    if (w.insert > 0 && f.lines[w.insert - 1].eol.empty()) f.lines[w.insert - 1].eol = f.eol;
    f.lines.insert(f.lines.begin() + w.insert, {text, f.eol});
    return;
  }
  if (!f.lines.empty() && f.lines.back().eol.empty()) f.lines.back().eol = f.eol;
  if (!f.lines.empty() && !trim(f.lines.back().text).empty()) f.lines.push_back({"", f.eol});
  f.lines.push_back({std::string("[") + kSection + "]", f.eol});
  f.lines.push_back({text, f.eol});
}

// The bytes of a value as letters: UTF-8 when they are that, else the system's code page.
std::wstring letters(const std::string& s) {
  if (s.empty()) return L"";
  UINT cp = CP_UTF8;
  int n = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) {
    cp = CP_ACP;
    n = MultiByteToWideChar(cp, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
  }
  std::wstring out(n > 0 ? n : 0, 0);
  if (n > 0) MultiByteToWideChar(cp, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

int number(const std::string& s, int fallback) { return s.empty() ? fallback : static_cast<int>(strtol(s.c_str(), nullptr, 0)); }

void read_values() {
  g.v = Values();
  g.present = false;
  if (g.canned || g.ini.empty()) return;
  File f;
  if (!read_file(g.ini, f)) return;
  g.present = true;
  const std::string mode = value_of(f, "mode");
  g.v.mode = mode == "host" ? Mode::Host : mode == "join" ? Mode::Join : mode == "off" ? Mode::Off : Mode::Auto;
  g.v.names = number(value_of(f, "names"), 1) != 0;
  g.v.friendly_fire = number(value_of(f, "friendly_fire"), 0) != 0;
  g.v.borderless = number(value_of(f, "borderless"), 0) != 0;
  g.v.name = letters(value_of(f, "name"));
  g.v.address = letters(value_of(f, "address"));
}

// One line of [net] gets a new value; the page then shows what the file has.
bool put(const char* key, const std::string& value) {
  g.error.clear();
  if (g.canned) return true;
  File f;
  if (g.ini.empty() || !read_file(g.ini, f)) {
    g.error = L"В папке игры нет metronet.ini: сначала установите мод.";
    return false;
  }
  if (!g.kept) {
    g.kept = true;
    CopyFileW(g.ini.c_str(), core::join(core::downloads_dir(), std::wstring(kIni) + L".before-launcher").c_str(), FALSE);
  }
  put_value(f, key, value);
  const bool ok = write_file(g.ini, f);
  if (!ok) {
    const DWORD err = GetLastError();
    g.error = err == ERROR_ACCESS_DENIED ? std::wstring(L"Папка игры закрыта для записи: запустите лаунчер от имени администратора.")
                                         : L"metronet.ini не записался (ошибка " + std::to_wstring(err) + L").";
  }
  const std::wstring why = g.error;
  read_values();
  g.error = why;
  return ok;
}

}  // namespace

void load(const std::wstring& game_dir) {
  if (g.dir != game_dir) g.kept = false;
  g.canned = false;
  g.dir = game_dir;
  g.ini = game_dir.empty() ? std::wstring() : core::join(game_dir, kIni);
  g.error.clear();
  read_values();
  if (g.present && g.first_ini != g.ini) {
    g.first = g.v;
    g.first_ini = g.ini;
  }
}

void load_canned(Mode mode, const wchar_t* name, const wchar_t* address) {
  g = State();
  g.canned = g.present = true;
  g.v.mode = mode;
  g.v.name = name;
  g.v.address = address;
  g.first = g.v;
}

bool has_ini() { return g.present; }
const Values& get() { return g.v; }
const Values& first() { return g.present && (g.canned || g.first_ini == g.ini) ? g.first : g.v; }
std::wstring ini_file() { return g.ini; }
const std::wstring& error() { return g.error; }

bool set_mode(Mode m) {
  if (g.canned) g.v.mode = m;
  return put("mode", m == Mode::Off ? "off" : m == Mode::Host ? "host" : m == Mode::Join ? "join" : "auto");
}

bool set_names(bool on) {
  if (g.canned) g.v.names = on;
  return put("names", on ? "1" : "0");
}

bool set_borderless(bool on) {
  if (g.canned) g.v.borderless = on;
  return put("borderless", on ? "1" : "0");
}

bool set_friendly_fire(bool on) {
  if (g.canned) g.v.friendly_fire = on;
  return put("friendly_fire", on ? "1" : "0");
}

// (";" would begin a comment for the mod; a quote at both ends is taken off by Windows' own reader of ini files)
bool name_char(wchar_t ch) { return ch >= 0x20 && ch != 0x7F && ch != L';' && ch != L'"' && ch != L'\''; }

bool address_char(wchar_t ch) {
  return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || ch == L'.' || ch == L':' ||
         ch == L'-' || ch == L'_';
}

bool set_name(const std::wstring& name) {
  std::wstring clean;
  for (wchar_t ch : name)
    if (name_char(ch) && clean.size() < kNameMax) clean += ch;
  while (!clean.empty() && clean.back() == L' ') clean.pop_back();
  while (!clean.empty() && clean.front() == L' ') clean.erase(0, 1);
  std::string utf8 = core::narrow(clean);
  while (utf8.size() > 48) {  // (the mod keeps 48 bytes of a name: whole letters only)
    clean.pop_back();
    utf8 = core::narrow(clean);
  }
  if (g.canned) g.v.name = clean;
  return put("name", utf8);
}

bool set_address(const std::wstring& address) {
  std::wstring clean;
  for (wchar_t ch : address)
    if (address_char(ch) && clean.size() < kAddressMax) clean += ch;
  if (g.canned) g.v.address = clean;
  return put("address", core::narrow(clean));
}

}  // namespace mset
}  // namespace lz
