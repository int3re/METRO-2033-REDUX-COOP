// The launcher's work behind the picture: settings, the game folder and the mod in it, the newest release
// (metronet-latest.json of the GitHub release), getting a newer launcher file and installing the mod from it.
#include "launcher/launcher.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <cstring>
#include <fstream>
#include <sstream>

#include "launcher/sha256.h"
#include "mod/game.h"  // the build of metro.exe the mod knows: kTimeDateStamp, kSizeOfImage

namespace lz {
namespace core {

std::wstring g_repo_url, g_settings_path, g_cache_dir;
int g_steam = -1, g_game = -1;
bool g_scripted = false;

std::wstring repo() { return g_repo_url.empty() ? std::wstring(kRepoUrl) : g_repo_url; }
bool has_repo() { return !repo().empty(); }

namespace {

const wchar_t kExe[] = L"metro.exe";
const wchar_t kAsi[] = L"metronet.asi";
const wchar_t kInstallList[] = L"metronet-install.txt";
// Metro 2033 Redux in GOG's own record of installed games (the 32-bit view of HKLM is where it is written).
const wchar_t kGogKey[] = L"SOFTWARE\\GOG.com\\Games\\1436434037";

// Versions of the mod and the protocol each speaks (src/mod/link.h). A release's own description
// (Release::protocols) comes first; this is for an installed version it does not name.
const struct {
  const wchar_t *from, *to;
  int protocol;
} kProtocols[] = {
    {L"0.1.0", L"0.1.4", 1},
    {L"0.1.5", L"0.3.0", 2},
};

std::wstring folder(int csidl) {
  wchar_t buf[MAX_PATH] = {};
  SHGetFolderPathW(nullptr, csidl, nullptr, 0, buf);
  return buf;
}

std::wstring settings_file() {
  if (!g_settings_path.empty()) return g_settings_path;
  std::wstring dir = join(folder(CSIDL_APPDATA), L"metronet");
  CreateDirectoryW(dir.c_str(), nullptr);
  return join(dir, L"launcher.ini");
}

std::wstring cache_dir() {
  std::wstring dir = g_cache_dir;
  if (dir.empty()) {
    dir = join(folder(CSIDL_LOCAL_APPDATA), L"metronet");
    CreateDirectoryW(dir.c_str(), nullptr);
    dir = join(dir, L"cache");
  }
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

std::wstring ini_get(const std::wstring& file, const wchar_t* section, const wchar_t* key, const wchar_t* def = L"") {
  wchar_t buf[2048] = {};
  GetPrivateProfileStringW(section, key, def, buf, 2048, file.c_str());
  return buf;
}

void ini_set(const std::wstring& file, const wchar_t* section, const wchar_t* key, const std::wstring& value) {
  WritePrivateProfileStringW(section, key, value.c_str(), file.c_str());
}

bool version_text(const std::wstring& v) {
  if (v.empty() || v.size() > 20) return false;
  for (wchar_t ch : v)
    if (!((ch >= L'0' && ch <= L'9') || ch == L'.')) return false;
  return v[0] != L'.' && v.back() != L'.';
}

// --- a small JSON reader ---------------------------------------------------------------------

struct Json {
  enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<Json> a;
  std::vector<std::pair<std::string, Json>> o;
  const Json* get(const char* key) const {
    for (auto& kv : o)
      if (kv.first == key) return &kv.second;
    return nullptr;
  }
  std::string str(const char* key) const {
    const Json* j = get(key);
    return j && j->type == Str ? j->s : std::string();
  }
  double num(const char* key) const {
    const Json* j = get(key);
    return j && j->type == Num ? j->n : 0;
  }
};

struct JsonReader {
  const char* p;
  const char* end;
  void ws() {
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
  }
  static void utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  bool hex4(unsigned& v) {
    if (end - p < 4) return false;
    v = 0;
    for (int i = 0; i < 4; ++i) {
      char ch = *p++;
      v <<= 4;
      if (ch >= '0' && ch <= '9') v |= ch - '0';
      else if (ch >= 'a' && ch <= 'f') v |= ch - 'a' + 10;
      else if (ch >= 'A' && ch <= 'F') v |= ch - 'A' + 10;
      else return false;
    }
    return true;
  }
  bool string(std::string& out) {
    if (p >= end || *p != '"') return false;
    ++p;
    while (p < end && *p != '"') {
      if (*p != '\\') {
        out += *p++;
        continue;
      }
      if (++p >= end) return false;
      char e = *p++;
      switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u': {
          unsigned cp = 0, low = 0;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
            p += 2;
            if (!hex4(low)) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          }
          utf8(out, cp);
          break;
        }
        default: out += e; break;
      }
    }
    if (p >= end) return false;
    ++p;
    return true;
  }
  bool value(Json& j, int depth) {
    if (depth > 16) return false;
    ws();
    if (p >= end) return false;
    if (*p == '{') {
      ++p;
      j.type = Json::Obj;
      ws();
      if (p < end && *p == '}') return ++p, true;
      for (;;) {
        ws();
        std::string key;
        if (!string(key)) return false;
        ws();
        if (p >= end || *p++ != ':') return false;
        Json v;
        if (!value(v, depth + 1)) return false;
        j.o.emplace_back(std::move(key), std::move(v));
        ws();
        if (p < end && *p == ',') {
          ++p;
          continue;
        }
        if (p < end && *p == '}') return ++p, true;
        return false;
      }
    }
    if (*p == '[') {
      ++p;
      j.type = Json::Arr;
      ws();
      if (p < end && *p == ']') return ++p, true;
      for (;;) {
        Json v;
        if (!value(v, depth + 1)) return false;
        j.a.push_back(std::move(v));
        ws();
        if (p < end && *p == ',') {
          ++p;
          continue;
        }
        if (p < end && *p == ']') return ++p, true;
        return false;
      }
    }
    if (*p == '"') {
      j.type = Json::Str;
      return string(j.s);
    }
    if (end - p >= 4 && !std::strncmp(p, "true", 4)) return p += 4, j.type = Json::Bool, j.b = true, true;
    if (end - p >= 5 && !std::strncmp(p, "false", 5)) return p += 5, j.type = Json::Bool, true;
    if (end - p >= 4 && !std::strncmp(p, "null", 4)) return p += 4, true;
    char buf[40];
    int n = 0;
    while (p < end && n < 39 && ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E'))
      buf[n++] = *p++;
    if (!n) return false;
    buf[n] = 0;
    j.type = Json::Num;
    j.n = atof(buf);
    return true;
  }
};

std::wstring clipped(const std::string& utf8, size_t max_chars) {
  std::wstring w = wide(utf8);
  for (wchar_t& ch : w)
    if (ch < 0x20) ch = L' ';
  if (w.size() > max_chars) w = w.substr(0, max_chars - 1) + L"…";
  return w;
}

std::wstring date_text(const std::string& iso) {  // 2026-11-20 -> 20 ноября 2026
  static const wchar_t* months[12] = {L"января", L"февраля", L"марта",    L"апреля",  L"мая",    L"июня",
                                      L"июля",   L"августа", L"сентября", L"октября", L"ноября", L"декабря"};
  int y = 0, mo = 0, d = 0;
  if (sscanf_s(iso.c_str(), "%d-%d-%d", &y, &mo, &d) != 3 || mo < 1 || mo > 12 || d < 1 || d > 31 || y < 2000 || y > 2999)
    return L"";
  return std::to_wstring(d) + L" " + months[mo - 1] + L" " + std::to_wstring(y);
}

bool writable(const std::wstring& dir) {
  std::wstring probe = join(dir, L"metronet-write-test.tmp");
  HANDLE f = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                         nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  CloseHandle(f);
  return true;
}

// This launcher again, with the rights of an administrator (the Windows prompt), to put the mod into a
// folder this user may not write to or take it out of there. Waits for it; its exit code.
int run_elevated(const std::wstring& args, bool* denied) {
  *denied = false;
  if (g_scripted) {  // (a test raises no prompt of Windows)
    *denied = true;
    return 99;
  }
  wchar_t self[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  SHELLEXECUTEINFOW sei{};
  sei.cbSize = sizeof(sei);
  sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
  sei.lpVerb = L"runas";
  sei.lpFile = self;
  sei.lpParameters = args.c_str();
  sei.nShow = SW_HIDE;
  if (!ShellExecuteExW(&sei) || !sei.hProcess) {
    *denied = true;
    return 99;
  }
  DWORD code = 99;
  WaitForSingleObject(sei.hProcess, INFINITE);
  GetExitCodeProcess(sei.hProcess, &code);
  CloseHandle(sei.hProcess);
  return static_cast<int>(code);
}

std::vector<net::Route> routes(const Settings& s, const net::Route* first) {
  std::vector<net::Route> out;
  auto add = [&](const net::Route& r) {
    for (auto& o : out)
      if (o.kind == r.kind && o.proxy == r.proxy) return;
    out.push_back(r);
  };
  if (first && !first->label.empty()) add(*first);
  if (s.proxy == 1) {
    add({1, L"", L"напрямую"});
  } else if (s.proxy == 2 && !s.proxy_addr.empty()) {
    add({2, s.proxy_addr, L"через прокси " + s.proxy_addr});
  } else {
    // Automatic: as Windows is set up, then the HTTP port of a VPN client on this PC, if one listens.
    add({0, L"", L"напрямую"});
    for (int port : {10809, 7890, 7897, 2080, 2081, 8889})
      if (net::port_open(L"127.0.0.1", port, 150)) {
        std::wstring addr = L"127.0.0.1:" + std::to_wstring(port);
        add({2, addr, L"через прокси " + addr});
      }
  }
  return out;
}

std::string hex_of_file(const std::wstring& path) {
  uint8_t h[32] = {};
  return sha256_file(path, h) ? to_hex(h, 32) : std::string();
}

// The launcher file of a release, in the cache: there already (and whole), or downloaded now.
Step fetch_package(const Release& rel, const Settings& s, const net::Route& first, const UpdateHooks& hooks,
                   const std::atomic<bool>& cancel, std::wstring* path) {
  std::wstring file = join(cache_dir(), L"metronet-" + rel.version + L"-" +
                                            (rel.pkg_sha256.empty() ? std::wstring(L"unchecked") : wide(rel.pkg_sha256.substr(0, 12))) +
                                            L".exe");
  *path = file;
  if (hooks.stage) hooks.stage(0);
  bool have = !rel.pkg_sha256.empty() && exists(file) && hex_of_file(file) == rel.pkg_sha256;
  if (!have) {
    std::wstring name = rel.pkg_name.empty() ? L"metronet-launcher.exe" : rel.pkg_name;
    std::wstring url = repo() + L"/releases/download/v" + rel.version + L"/" + name;
    std::wstring part = file + L".part";
    bool ok = false;
    for (const net::Route& r : routes(s, &first)) {
      if (cancel.load()) return Step::Cancelled;
      net::Response resp = net::get(url, r, true, part, hooks.progress, &cancel);
      if (resp.error == ERROR_CANCELLED) return Step::Cancelled;
      if (resp.status == 200 && !resp.error) {
        ok = true;
        break;
      }
    }
    if (!ok) return Step::NetFail;
    DeleteFileW(file.c_str());
    if (!MoveFileW(part.c_str(), file.c_str())) return Step::NetFail;
  } else if (hooks.progress) {
    hooks.progress(rel.pkg_size ? rel.pkg_size : 1, rel.pkg_size ? rel.pkg_size : 1);
  }
  if (hooks.stage) hooks.stage(1);
  if (!rel.pkg_sha256.empty() && hex_of_file(file) != rel.pkg_sha256) {
    DeleteFileW(file.c_str());
    return Step::BadSum;
  }
  Sleep(350);  // (the step is seen)
  return Step::Ok;
}

// What a release's description says, into `out`. want_file: the launcher file's name, size and SHA-256
// must be there (GitHub's description); without it only the words are read (the one inside a launcher).
bool read_release(const std::string& json, Release& rel, bool want_file) {
  Json j;
  JsonReader r{json.data(), json.data() + json.size()};
  if (json.size() >= 3 && !std::memcmp(json.data(), "\xEF\xBB\xBF", 3)) r.p += 3;
  if (!r.value(j, 0) || j.type != Json::Obj) return false;
  Release out;
  out.version = wide(j.str("version"));
  if (!version_text(out.version)) return false;
  out.known = true;
  out.protocol = static_cast<int>(j.num("protocol"));
  out.date = date_text(j.str("date"));
  out.title = clipped(j.str("title"), 90);
  if (const Json* notes = j.get("notes"))
    for (const Json& n : notes->a)
      if (n.type == Json::Str && out.notes.size() < 6) out.notes.push_back(clipped(n.s, 220));
  // The file to download: the launcher itself.
  if (const Json* f = j.get("launcher")) {
    std::wstring name = wide(f->str("name"));
    bool fine = !name.empty() && name.size() <= 64 && name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0;
    for (wchar_t ch : name)
      if (!((ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || ch == L'.' || ch == L'-' ||
            ch == L'_'))
        fine = false;
    std::string sha = f->str("sha256");
    for (char& ch : sha)
      if (ch >= 'A' && ch <= 'F') ch = static_cast<char>(ch - 'A' + 'a');
    bool hex = sha.size() == 64;
    for (char ch : sha)
      if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) hex = false;
    double size = f->num("size");
    if (fine && hex && size > 0 && size < 512.0 * 1024 * 1024) {
      out.pkg_name = name;
      out.pkg_sha256 = sha;
      out.pkg_size = static_cast<uint64_t>(size);
      out.full = true;
    }
    std::wstring v = wide(f->str("version"));
    if (version_text(v)) out.launcher_version = v;
  }
  if (const Json* p = j.get("protocols"))
    for (auto& kv : p->o) {
      std::wstring v = wide(kv.first);
      if (version_text(v) && kv.second.type == Json::Num && out.protocols.size() < 200)
        out.protocols.emplace_back(v, static_cast<int>(kv.second.n));
    }
  if (want_file && !out.full) return false;
  rel = out;
  return true;
}

// The game holds the mod's files while it runs: wait for it to close.
Step wait_for_game(const std::wstring& dir, const UpdateHooks& hooks, const std::atomic<bool>& cancel) {
  if (!game_running(dir)) return Step::Ok;
  if (hooks.game_open) hooks.game_open(true);
  while (game_running(dir)) {
    if (cancel.load()) return Step::Cancelled;
    Sleep(300);
  }
  Sleep(400);  // (its process lets go of the folder)
  if (hooks.game_open) hooks.game_open(false);
  return Step::Ok;
}

std::wstring reg_text(HKEY root, REGSAM view, const wchar_t* key, const wchar_t* value) {
  HKEY k;
  if (RegOpenKeyExW(root, key, 0, KEY_QUERY_VALUE | view, &k) != ERROR_SUCCESS) return L"";
  wchar_t buf[MAX_PATH * 2] = {};
  DWORD size = sizeof(buf) - sizeof(wchar_t), type = 0;
  const bool ok = RegQueryValueExW(k, value, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS &&
                  (type == REG_SZ || type == REG_EXPAND_SZ);
  RegCloseKey(k);
  return ok ? std::wstring(buf) : std::wstring();
}

std::wstring parent_of(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

}  // namespace

std::wstring downloads_dir() { return cache_dir(); }

std::string read_all(const std::wstring& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// A path as one argument of a command line (a backslash before the closing quote would swallow it).
std::wstring quoted(std::wstring path) {
  size_t tail = 0;
  while (tail < path.size() && path[path.size() - 1 - tail] == L'\\') ++tail;
  return L"\"" + path + std::wstring(tail, L'\\') + L"\"";
}

std::wstring join(const std::wstring& dir, const std::wstring& name) {
  if (dir.empty()) return name;
  wchar_t last = dir.back();
  return (last == L'\\' || last == L'/') ? dir + name : dir + L"\\" + name;
}

std::wstring trimmed(std::wstring dir) {
  while (dir.size() > 3 && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
  return dir;
}

bool exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string narrow(const std::wstring& s) {
  if (s.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
  std::string out(n, 0);
  WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
  return out;
}

std::wstring wide(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

int compare_versions(const std::wstring& a, const std::wstring& b) {
  size_t i = 0, j = 0;
  while (i < a.size() || j < b.size()) {
    unsigned long x = 0, y = 0;
    while (i < a.size() && a[i] != L'.') x = x * 10 + (a[i++] - L'0');
    while (j < b.size() && b[j] != L'.') y = y * 10 + (b[j++] - L'0');
    if (x != y) return x < y ? -1 : 1;
    if (i < a.size()) ++i;
    if (j < b.size()) ++j;
  }
  return 0;
}

int known_protocol(const Release& rel, const std::wstring& version) {
  if (!version_text(version)) return 0;
  if (rel.known && rel.version == version && rel.protocol) return rel.protocol;
  for (auto& p : rel.protocols)
    if (p.first == version) return p.second;
  for (auto& k : kProtocols)
    if (compare_versions(version, k.from) >= 0 && compare_versions(version, k.to) <= 0) return k.protocol;
  return 0;
}

void load_settings(Settings& s) {
  std::wstring f = settings_file();
  s.game_dir = ini_get(f, L"launcher", L"game_dir");
  s.auto_check = ini_get(f, L"launcher", L"auto_check", L"1") != L"0";
  s.auto_install = ini_get(f, L"launcher", L"auto_install", L"1") == L"1";
  // 1.0.0 wrote auto_install = 0 for everybody (nobody chose it): since 1.1.0 starting the launcher updates
  // everything by itself, so that old "0" becomes "1" once; a choice made in 1.1.0 or later stays
  if (_wtoi(ini_get(f, L"launcher", L"settings", L"1").c_str()) < 2) s.auto_install = true;
  s.auto_self = ini_get(f, L"launcher", L"auto_self", L"1") != L"0";
  s.self_skip = ini_get(f, L"launcher", L"self_skip");
  s.close_on_play = ini_get(f, L"launcher", L"close_on_play", L"1") != L"0";
  s.proxy = _wtoi(ini_get(f, L"launcher", L"proxy", L"0").c_str());
  if (s.proxy < 0 || s.proxy > 2) s.proxy = 0;
  s.proxy_addr = ini_get(f, L"launcher", L"proxy_addr");
  s.notes_version = ini_get(f, L"notes", L"version");
  s.notes_title = ini_get(f, L"notes", L"title");
  s.notes_date = ini_get(f, L"notes", L"date");
  s.notes.clear();
  int n = _wtoi(ini_get(f, L"notes", L"count", L"0").c_str());
  for (int i = 0; i < n && i < 8; ++i) {
    std::wstring line = ini_get(f, L"notes", (L"n" + std::to_wstring(i)).c_str());
    if (!line.empty()) s.notes.push_back(line);
  }
}

void save_settings(const Settings& s) {
  std::wstring f = settings_file();
  if (!exists(f)) {  // UTF-16 with its mark: the folder and the notes keep their letters
    HANDLE h = CreateFileW(f.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      const wchar_t head[] = L"\xFEFF; metronet-launcher\r\n";
      DWORD w = 0;
      WriteFile(h, head, static_cast<DWORD>(sizeof(head) - sizeof(wchar_t)), &w, nullptr);
      CloseHandle(h);
    }
  }
  ini_set(f, L"launcher", L"game_dir", s.game_dir);
  ini_set(f, L"launcher", L"auto_check", s.auto_check ? L"1" : L"0");
  ini_set(f, L"launcher", L"settings", L"2");
  ini_set(f, L"launcher", L"auto_install", s.auto_install ? L"1" : L"0");
  ini_set(f, L"launcher", L"auto_self", s.auto_self ? L"1" : L"0");
  ini_set(f, L"launcher", L"self_skip", s.self_skip);
  ini_set(f, L"launcher", L"close_on_play", s.close_on_play ? L"1" : L"0");
  ini_set(f, L"launcher", L"proxy", std::to_wstring(s.proxy));
  ini_set(f, L"launcher", L"proxy_addr", s.proxy_addr);
  ini_set(f, L"notes", L"version", s.notes_version);
  ini_set(f, L"notes", L"title", s.notes_title);
  ini_set(f, L"notes", L"date", s.notes_date);
  ini_set(f, L"notes", L"count", std::to_wstring(s.notes.size()));
  for (size_t i = 0; i < s.notes.size() && i < 8; ++i) ini_set(f, L"notes", (L"n" + std::to_wstring(i)).c_str(), s.notes[i]);
}

bool has_game(const std::wstring& dir) { return !dir.empty() && exists(join(dir, kExe)); }

// The build the mod knows is told by two numbers of metro.exe's PE header (as the mod itself tells it,
// src/mod/game.h): the link time and the size of the image.
bool exe_supported(const std::wstring& dir) {
  HANDLE f = CreateFileW(join(dir, kExe).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  uint8_t head[4096] = {};
  DWORD got = 0;
  ReadFile(f, head, sizeof(head), &got, nullptr);
  CloseHandle(f);
  if (got < 0x40 || head[0] != 'M' || head[1] != 'Z') return false;
  uint32_t pe = 0;
  std::memcpy(&pe, head + 0x3C, 4);
  if (pe > sizeof(head) - 0x60 || pe + 0x60 > got || std::memcmp(head + pe, "PE\0\0", 4) != 0) return false;
  uint32_t stamp = 0, image = 0;
  uint16_t magic = 0;
  std::memcpy(&stamp, head + pe + 8, 4);        // IMAGE_FILE_HEADER.TimeDateStamp
  std::memcpy(&magic, head + pe + 24, 2);       // IMAGE_OPTIONAL_HEADER64.Magic
  std::memcpy(&image, head + pe + 24 + 56, 4);  // ... .SizeOfImage
  return magic == 0x20B && stamp == game::kTimeDateStamp && image == game::kSizeOfImage;
}

std::wstring find_game_dir(std::wstring* trace) {
  // Where a metro.exe is, in the order of trust. `named`: a place that names this very game - it is told
  // even when its metro.exe is another build (the launcher then says so); other places only with the right one.
  std::vector<std::pair<std::wstring, bool>> found;
  auto add = [&](std::wstring dir, bool named, const wchar_t* from = nullptr) {
    dir = trimmed(dir);
    if (trace && from && !dir.empty())
      *trace += std::wstring(from) + L": " + dir +
                (!has_game(dir) ? L" (no metro.exe there)" : exe_supported(dir) ? L" (the game)" : L" (another build)") + L"\n";
    if (!has_game(dir)) return;
    for (auto& f : found)
      if (_wcsicmp(f.first.c_str(), dir.c_str()) == 0) return;
    found.emplace_back(dir, named);
  };
  // GOG's record of the game.
  for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER})
    for (REGSAM view : {KEY_WOW64_32KEY, KEY_WOW64_64KEY}) {
      add(reg_text(root, view, kGogKey, L"path"), true, L"GOG record, path");
      add(reg_text(root, view, kGogKey, L"workingDir"), true, L"GOG record, workingDir");
      add(parent_of(reg_text(root, view, kGogKey, L"exe")), true, L"GOG record, exe");
    }
  // The launcher's own folder.
  wchar_t self[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  const std::wstring here = parent_of(self);
  add(here, true, L"the launcher's folder");
  // The game's uninstall entry (the usual installers write where it is).
  const wchar_t* base = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
  for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
    for (REGSAM view : {KEY_WOW64_32KEY, KEY_WOW64_64KEY}) {
      HKEY k;
      if (RegOpenKeyExW(root, base, 0, KEY_READ | view, &k) != ERROR_SUCCESS) continue;
      wchar_t name[256];
      for (DWORD i = 0;; ++i) {
        DWORD len = 256;
        if (RegEnumKeyExW(k, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        const std::wstring sub = std::wstring(base) + L"\\" + name;
        for (const wchar_t* value : {L"InstallLocation", L"Inno Setup: App Path"}) add(reg_text(root, view, sub.c_str(), value), false);
      }
      RegCloseKey(k);
    }
  }
  // Beside the launcher: the folder above its own, or one next to its own (the mod's kit unpacked by the
  // game's folder). Several copies of the game there: the one the mod was installed into, if it is the
  // only such - else the player says which.
  const std::wstring parent = parent_of(here);
  if (parent.size() > 2) {
    add(parent, false);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(join(parent, L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      std::vector<std::wstring> copies, with_mod;
      int seen = 0;
      do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        std::wstring dir = join(parent, fd.cFileName);
        if (!has_game(dir) || !exe_supported(dir)) continue;
        copies.push_back(dir);
        if (exists(join(dir, kInstallList))) with_mod.push_back(dir);
      } while (++seen < 2000 && FindNextFileW(h, &fd));
      FindClose(h);
      if (copies.size() == 1) add(copies[0], false, L"beside the launcher");
      else if (with_mod.size() == 1) add(with_mod[0], false, L"beside the launcher, with the mod");
      else if (trace && !copies.empty())
        *trace += L"beside the launcher: " + std::to_wstring(copies.size()) + L" copies of the game, none chosen\n";
    }
  }
  for (auto& f : found)
    if (exe_supported(f.first)) return f.first;
  for (auto& f : found)
    if (f.second) return f.first;
  return L"";
}

Installed read_installed(const std::wstring& dir) {
  Installed in;
  if (!has_game(dir) || !exists(join(dir, kAsi))) return in;
  in.present = true;
  // The launcher's list begins "# metronet 0.3.0 (protocol 2). ...".
  std::string list = read_all(join(dir, kInstallList));
  const char* tag = "# metronet ";
  if (list.compare(0, std::strlen(tag), tag) == 0) {
    size_t p = std::strlen(tag), e = p;
    while (e < list.size() && ((list[e] >= '0' && list[e] <= '9') || list[e] == '.')) ++e;
    std::string v = list.substr(p, e - p);
    while (!v.empty() && v.back() == '.') v.pop_back();
    in.version = wide(v);
    if (!version_text(in.version)) in.version.clear();
    size_t line_end = list.find('\n');
    size_t pr = list.find("protocol ");
    if (pr != std::string::npos && pr < line_end) in.protocol = atoi(list.c_str() + pr + 9);
  } else {
    // Put there by hand: the version is written nowhere. It is known all the same when the file is, byte
    // for byte, the one inside this launcher.
    pkg::Package p;
    if (p.open(L"") && p.holds(kAsi, join(dir, kAsi))) {
      in.version = p.version;
      in.protocol = p.protocol;
    }
  }
  return in;
}

// A running metro.exe from this folder keeps metronet.asi open.
bool game_running(const std::wstring& dir) {
  std::wstring asi = join(dir, kAsi);
  if (!exists(asi)) return false;
  HANDLE f = CreateFileW(asi.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_SHARING_VIOLATION;
  CloseHandle(f);
  return false;
}

namespace {
struct CloseAsk {
  std::wstring exe;
  int asked = 0;
};
BOOL CALLBACK close_window(HWND hwnd, LPARAM lp) {
  auto* ask = reinterpret_cast<CloseAsk*>(lp);
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (!pid || pid == GetCurrentProcessId() || GetWindow(hwnd, GW_OWNER)) return TRUE;
  HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!proc) return TRUE;
  wchar_t path[MAX_PATH * 2] = {};
  DWORD size = MAX_PATH * 2;
  if (QueryFullProcessImageNameW(proc, 0, path, &size) && _wcsicmp(path, ask->exe.c_str()) == 0) {
    PostMessageW(hwnd, WM_CLOSE, 0, 0);  // (as the window's own cross)
    ++ask->asked;
  }
  CloseHandle(proc);
  return TRUE;
}
}  // namespace

int close_game(const std::wstring& dir) {
  CloseAsk ask{join(dir, kExe)};
  EnumWindows(close_window, reinterpret_cast<LPARAM>(&ask));
  return ask.asked;
}

bool launch_game(const std::wstring& dir) {
  std::wstring exe = join(dir, kExe);
  SHELLEXECUTEINFOW sei{};
  sei.cbSize = sizeof(sei);
  sei.lpFile = exe.c_str();
  sei.lpDirectory = dir.c_str();
  sei.nShow = SW_SHOWNORMAL;
  return ShellExecuteExW(&sei) != FALSE;
}

bool steam_running() {
  if (g_steam >= 0) return g_steam != 0;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  bool found = false;
  for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
    found = _wcsicmp(pe.szExeFile, L"steam.exe") == 0;
  CloseHandle(snap);
  return found;
}

// The game writes its settings when it ends: while a metro.exe of this folder runs they are not to be changed.
bool game_process_running(const std::wstring& dir) {
  if (g_game >= 0) return g_game != 0;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  const std::wstring exe = dir.empty() ? std::wstring() : join(dir, kExe);
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  bool found = false;
  for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) {
    if (_wcsicmp(pe.szExeFile, kExe) != 0) continue;
    wchar_t path[MAX_PATH * 2] = {};
    DWORD size = MAX_PATH * 2;
    HANDLE proc = exe.empty() ? nullptr : OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
    // (a copy of the game in another folder keeps its own settings; one that cannot be asked counts as ours)
    found = !proc || !QueryFullProcessImageNameW(proc, 0, path, &size) || _wcsicmp(path, exe.c_str()) == 0;
    if (proc) CloseHandle(proc);
  }
  CloseHandle(snap);
  return found;
}

bool pick_folder(HWND owner, const std::wstring& initial, std::wstring& out) {
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  dlg->SetTitle(L"Папка игры Metro 2033 Redux: где лежит metro.exe");
  IShellItem* init = nullptr;
  if (!initial.empty() && SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&init)))) {
    dlg->SetFolder(init);
    init->Release();
  }
  bool ok = false;
  if (SUCCEEDED(dlg->Show(owner))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        out = path;
        CoTaskMemFree(path);
        ok = true;
      }
      item->Release();
    }
  }
  dlg->Release();
  return ok;
}

bool parse_manifest(const std::string& json, Release& rel) { return read_release(json, rel, true); }

// The mod inside this launcher: its version from the package itself, the words from its release.json.
const Release& bundled() {
  static Release mine;
  static bool read = false;
  if (!read) {
    read = true;
    pkg::Package p;
    if (p.open(L"")) {
      Release words;
      if (read_release(p.info, words, false) && words.version == p.version) mine = words;
      mine.known = true;
      mine.full = false;
      mine.version = p.version;
      mine.protocol = p.protocol;
      mine.launcher_version = kLauncherVersion;
    }
  }
  return mine;
}

bool newer_on_github(const Release& rel, bool assets_blocked) {
  const Release& mine = bundled();
  return rel.known && !assets_blocked && (!mine.known || compare_versions(rel.version, mine.version) > 0);
}

const Release& target(const Release& rel, bool assets_blocked) { return newer_on_github(rel, assets_blocked) ? rel : bundled(); }

CheckResult check(const Settings& s, const std::atomic<bool>& cancel) {
  CheckResult out;
  if (!has_repo()) {
    out.log = L"у мода пока нет репозитория: проверять нечего\n";
    return out;
  }
  std::wstring base = repo() + L"/releases/latest/download";
  std::vector<net::Route> rs = routes(s, nullptr);
  bool no_description = false;  // the newest release has no metronet-latest.json, or there is no release
  for (const net::Route& r : rs) {
    if (cancel.load()) return out;
    net::Response resp = net::get(base + L"/metronet-latest.json", r, true);
    out.log += L"описание релиза, " + r.label + L": " +
               (resp.sent ? L"HTTP " + std::to_wstring(resp.status) : L"нет ответа (" + std::to_wstring(resp.error) + L")") + L"\n";
    if (resp.status == 200 && !resp.error && parse_manifest(resp.body, out.rel)) {
      out.answered = true;
      out.route = r;
      return out;
    }
    if (resp.sent) out.answered = true;
    if (resp.sent && resp.status == 404) {  // (answered, and clearly: no other way would say otherwise)
      no_description = true;
      out.route = r;
      break;
    }
  }
  // No description came. Where "latest" points tells the version at least (github.com alone answers
  // that; the files of a release are on another host) - or that nothing has been released yet.
  for (const net::Route& r : rs) {
    if (cancel.load()) return out;
    net::Response resp = net::get(repo() + L"/releases/latest", r, false);
    out.log += L"последний релиз, " + r.label + L": " +
               (resp.sent ? L"HTTP " + std::to_wstring(resp.status) : L"нет ответа (" + std::to_wstring(resp.error) + L")") + L"\n";
    size_t tag = resp.location.find(L"/releases/tag/v");
    if (resp.sent && (resp.status == 301 || resp.status == 302) && tag != std::wstring::npos) {
      std::wstring v = resp.location.substr(tag + 15);
      if (version_text(v)) {
        out.answered = true;
        out.rel = Release{};
        out.rel.known = true;
        out.rel.version = v;
        out.rel.protocol = known_protocol(out.rel, v);
        out.rel.pkg_name = L"metronet-launcher.exe";
        out.assets_blocked = !no_description;
        out.route = r;
        return out;
      }
    }
    // No such page (the repository is not there or not open yet), or "latest" leads back to an empty
    // list of releases: nothing is published. That is no fault of the network.
    if (resp.sent && no_description && (resp.status == 404 || ((resp.status == 301 || resp.status == 302) && tag == std::wstring::npos))) {
      out.answered = true;
      out.no_release = true;
      out.route = r;
      return out;
    }
  }
  return out;
}

// Puts the mod into the game folder: from the newer launcher file of the release (downloaded first and
// checked) or from this launcher itself.
Step install(const Release& rel, bool download, const Settings& s, const net::Route& first, const UpdateHooks& hooks,
             const std::atomic<bool>& cancel, std::wstring* log, std::wstring* package_file) {
  std::wstring file;  // (empty: this launcher)
  if (download) {
    Step st = fetch_package(rel, s, first, hooks, cancel, &file);
    if (st != Step::Ok) return st;
  } else {
    if (hooks.stage) hooks.stage(1);  // (nothing to download: the mod is in this launcher)
    Sleep(450);
  }
  pkg::Package p;
  if (!p.open(file) || (download && p.version != rel.version)) {
    *log += download ? L"в скачанном файле нет мода версии " + rel.version + L"\n" : L"в этом лаунчере нет мода\n";
    if (download) DeleteFileW(file.c_str());
    return Step::BadPackage;
  }
  if (package_file) *package_file = file;
  if (hooks.stage) hooks.stage(2);
  Step st = wait_for_game(s.game_dir, hooks, cancel);
  if (st != Step::Ok) return st;
  if (cancel.load()) return Step::Cancelled;
  if (!writable(s.game_dir)) {
    bool denied = false;
    int code = run_elevated(L"/install " + quoted(s.game_dir) + L" /quiet" + (file.empty() ? L"" : L" /package " + quoted(file)), &denied);
    if (denied) return Step::Denied;
    Installed now = read_installed(s.game_dir);
    if (code == 0 || (now.present && now.version == p.version)) return Step::Ok;
    *log += L"установка с правами администратора не удалась (код " + std::to_wstring(code) + L")\n";
    return Step::Failed;
  }
  pkg::Result r = pkg::install(p, s.game_dir, *log);
  return r == pkg::Result::Ok ? Step::Ok : r == pkg::Result::Denied ? Step::Denied : Step::Failed;
}

Step uninstall(const Settings& s, const UpdateHooks& hooks, const std::atomic<bool>& cancel, std::wstring* log) {
  if (hooks.stage) hooks.stage(2);
  Step st = wait_for_game(s.game_dir, hooks, cancel);
  if (st != Step::Ok) return st;
  if (cancel.load()) return Step::Cancelled;
  if (!writable(s.game_dir)) {
    bool denied = false;
    int code = run_elevated(L"/uninstall " + quoted(s.game_dir) + L" /quiet", &denied);
    if (denied) return Step::Denied;
    if (code == 0 || !read_installed(s.game_dir).present) return Step::Ok;
    *log += L"удаление с правами администратора не удалось (код " + std::to_wstring(code) + L")\n";
    return Step::Failed;
  }
  Sleep(400);  // (the step is seen)
  pkg::Result r = pkg::uninstall(s.game_dir, *log);
  return r == pkg::Result::Ok ? Step::Ok : r == pkg::Result::Denied ? Step::Denied : Step::Failed;
}

// A newer launcher alone (the mod is the same): downloaded, checked, put in this one's place.
Step fetch_launcher(const Release& rel, const Settings& s, const net::Route& first, const UpdateHooks& hooks,
                    const std::atomic<bool>& cancel, std::wstring* log, std::wstring* package_file) {
  std::wstring file;
  Step st = fetch_package(rel, s, first, hooks, cancel, &file);
  if (st != Step::Ok) return st;
  pkg::Package p;
  if (!p.open(file)) {
    *log += L"скачанный файл — не лаунчер metronet\n";
    DeleteFileW(file.c_str());
    return Step::BadPackage;
  }
  if (hooks.stage) hooks.stage(2);
  Sleep(300);
  if (package_file) *package_file = file;
  return Step::Ok;
}

// This launcher's own file becomes `newer`. The running one is renamed aside (Windows lets a running
// program be renamed) and removed at the next start.
bool replace_self(const std::wstring& newer) {
  wchar_t self[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  std::wstring old = std::wstring(self) + L".old";
  DeleteFileW(old.c_str());
  if (!MoveFileExW(self, old.c_str(), MOVEFILE_REPLACE_EXISTING)) return false;
  if (!CopyFileW(newer.c_str(), self, TRUE)) {
    MoveFileExW(old.c_str(), self, MOVEFILE_REPLACE_EXISTING);
    return false;
  }
  return true;
}

bool drop_old_self() {
  wchar_t self[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  std::wstring old = std::wstring(self) + L".old";
  DeleteFileW(old.c_str());
  return !exists(old);  // (false while the launcher it was is still closing)
}

}  // namespace core
}  // namespace lz
