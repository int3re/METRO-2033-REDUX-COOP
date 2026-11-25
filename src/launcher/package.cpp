// The mod as it travels: inside a launcher exe, as resources. This launcher carries one version of the mod;
// a newer one comes as a newer launcher file, which is opened as data (never run) and installed from.
//   resource RES_PKG_INDEX: a text list of what the package holds (below)
//   resource RES_PKG_INFO:  release.json of that version (title, notes, protocols)
//   resources RES_PKG_FILES..: the files
// Installing puts the files next to metro.exe: dinput8.dll (the loader), metronet.asi (the mod),
// metronet_steam_api64.dll (Steam's library under the mod's own name), metronet-README.md, and metronet.ini
// when the folder has none. metro.exe, the game's data and the game's own steam_api64.dll are never touched.
// Every installed file and every backup is listed in metronet-install.txt in the game folder; removing the
// mod removes exactly those (and metronet.log) and puts the backups back.
#include "launcher/launcher.h"

#include <cstring>
#include <fstream>
#include <sstream>

#include "launcher/res.h"

namespace lz {
namespace pkg {
namespace {

const wchar_t kList[] = L"metronet-install.txt";
const wchar_t kBackupDir[] = L"metronet-backup";
const wchar_t kExe[] = L"metro.exe";
const wchar_t kLoader[] = L"dinput8.dll";
// The mod's own names: a file of such a name in the game folder is the mod's, whoever put it there.
const wchar_t* const kOwn[] = {L"metronet.asi", L"metronet_steam_api64.dll", L"metronet-README.md", L"metronet.ini"};

using core::exists;
using core::join;
using core::read_all;

std::wstring error_text(DWORD err) {
  wchar_t* buf = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0,
                 reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
  std::wstring s = buf ? buf : L"";
  if (buf) LocalFree(buf);
  while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L'.')) s.pop_back();
  return s + L" (" + std::to_wstring(err) + L")";
}

bool write_file(const std::wstring& path, const void* data, DWORD size, DWORD& err) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    err = GetLastError();
    return false;
  }
  DWORD written = 0;
  bool ok = WriteFile(f, data, size, &written, nullptr) && written == size;
  err = ok ? 0 : GetLastError();
  CloseHandle(f);
  return ok;
}

bool plain_name(const std::wstring& n) {  // a file's own name, nothing of a path in it
  if (n.empty() || n.size() > 64 || n[0] == L'.') return false;
  for (wchar_t ch : n)
    if (!((ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || ch == L'.' || ch == L'-' ||
          ch == L'_'))
      return false;
  return true;
}

// The game's own files that sit where the mod's do: no package may name them, no list may have them removed.
bool games_own(const std::wstring& n) { return _wcsicmp(n.c_str(), L"steam_api64.dll") == 0 || _wcsicmp(n.c_str(), kExe) == 0; }

bool mods_own(const std::wstring& n) {
  for (const wchar_t* own : kOwn)
    if (_wcsicmp(n.c_str(), own) == 0) return true;
  return false;
}

// A dinput8.dll that is metronet's loader of any version: it names the file it loads.
bool our_loader(const std::wstring& path) {
  const std::string have = read_all(path);
  const wchar_t mark[] = L"metronet.asi";
  return have.find(std::string(reinterpret_cast<const char*>(mark), sizeof(mark) - sizeof(wchar_t))) != std::string::npos;
}

// What the launcher put into a game folder, and what it moved aside to do so.
struct List {
  std::vector<std::wstring> files;
  std::vector<std::pair<std::wstring, std::wstring>> backups;  // original -> backup
  std::wstring version;  // of the mod that was put there (empty: not known)

  bool load(const std::wstring& dir) {
    std::ifstream in(join(dir, kList), std::ios::binary);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
      if (line.rfind("# metronet ", 0) == 0) version = core::wide(line.substr(11, line.find(' ', 11) - 11));
      if (line.rfind("file\t", 0) == 0) files.push_back(core::wide(line.substr(5)));
      if (line.rfind("backup\t", 0) == 0) {
        size_t tab = line.find('\t', 7);
        if (tab != std::string::npos) backups.emplace_back(core::wide(line.substr(7, tab - 7)), core::wide(line.substr(tab + 1)));
      }
    }
    return true;
  }
  bool save(const std::wstring& dir, const Package& p) const {
    std::ofstream out(join(dir, kList), std::ios::binary);
    out << "# metronet " << core::narrow(p.version) << " (protocol " << p.protocol
        << "). metronet-launcher.exe removes exactly these files.\n";
    for (auto& f : files) out << "file\t" << core::narrow(f) << "\n";
    for (auto& b : backups) out << "backup\t" << core::narrow(b.first) << "\t" << core::narrow(b.second) << "\n";
    return static_cast<bool>(out);
  }
  bool has(const std::wstring& name) const {
    for (auto& f : files)
      if (_wcsicmp(f.c_str(), name.c_str()) == 0) return true;
    return false;
  }
};

bool same_bytes(const std::wstring& path, const void* data, DWORD size) {
  if (!data) return false;
  std::string have = read_all(path);
  return have.size() == size && std::memcmp(have.data(), data, size) == 0;
}

// Puts one file into the folder. A file of that name that is not the mod's is moved to metronet-backup\ first.
Result put(const std::wstring& dir, const std::wstring& name, const void* data, DWORD size, List& list, std::wstring& log) {
  std::wstring dst = join(dir, name);
  bool ours = list.has(name) || mods_own(name) || same_bytes(dst, data, size) ||
              (_wcsicmp(name.c_str(), kLoader) == 0 && exists(dst) && our_loader(dst));
  if (exists(dst) && !ours) {
    std::wstring bdir = join(dir, kBackupDir);
    CreateDirectoryW(bdir.c_str(), nullptr);
    if (!MoveFileExW(dst.c_str(), join(bdir, name).c_str(), MOVEFILE_REPLACE_EXISTING)) {
      DWORD e = GetLastError();
      log += L"не удалось сохранить прежний " + name + L": " + error_text(e) + L"\n";
      return e == ERROR_ACCESS_DENIED ? Result::Denied : Result::Failed;
    }
    list.backups.emplace_back(name, std::wstring(kBackupDir) + L"\\" + name);
    log += L"прежний " + name + L" сохранён в " + kBackupDir + L"\\\n";
  }
  DWORD err = 0;
  if (!write_file(dst, data, size, err)) {
    log += L"не удалось записать " + name + L": " + error_text(err) + L"\n";
    return err == ERROR_ACCESS_DENIED ? Result::Denied : Result::Failed;
  }
  if (!list.has(name)) list.files.push_back(name);
  log += name + L"\n";
  return Result::Ok;
}

// Removes one of the mod's files. True: it is not there any more.
bool drop(const std::wstring& dir, const std::wstring& name, std::wstring& log, Result& r) {
  if (!plain_name(name) || games_own(name)) return false;
  std::wstring p = join(dir, name);
  if (!exists(p)) return true;
  if (!DeleteFileW(p.c_str())) {
    DWORD e = GetLastError();
    log += L"не удалось удалить " + name + L": " + error_text(e) + L"\n";
    r = e == ERROR_ACCESS_DENIED ? Result::Denied : Result::Failed;
    return false;
  }
  log += L"удалён " + name + L"\n";
  return true;
}

}  // namespace

Package::~Package() {
  if (mod_ && owned_) FreeLibrary(mod_);
}

bool Package::bytes(int id, const void*& data, DWORD& size) const {
  HRSRC r = FindResourceW(mod_, MAKEINTRESOURCEW(id), RT_RCDATA);
  HGLOBAL g = r ? LoadResource(mod_, r) : nullptr;
  data = g ? LockResource(g) : nullptr;
  size = r ? SizeofResource(mod_, r) : 0;
  return data && size;
}

bool Package::holds(const std::wstring& name, const std::wstring& path) const {
  for (const Entry& e : entries) {
    if (_wcsicmp(e.name.c_str(), name.c_str()) != 0) continue;
    const void* data = nullptr;
    DWORD size = 0;
    return bytes(e.id, data, size) && same_bytes(path, data, size);
  }
  return false;
}

// The index: lines of words.
//   metronet-package 1
//   version 0.3.0 | protocol 2 | launcher 1.0.0
//   file <name> <resource id> [loader] [keep]   loader: an Ultimate ASI Loader already there is used as it is
//                                               keep: a file already there stays (the player's settings)
bool Package::open(const std::wstring& file) {
  if (mod_ && owned_) FreeLibrary(mod_);
  mod_ = nullptr;
  owned_ = false;
  entries.clear();
  version.clear();
  if (!file.empty()) {
    mod_ = LoadLibraryExW(file.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!mod_) return false;
    owned_ = true;
  }
  const void* data = nullptr;
  DWORD size = 0;
  if (!bytes(RES_PKG_INDEX, data, size) || size > 16384) return false;
  std::istringstream in(std::string(static_cast<const char*>(data), size));
  std::string line;
  bool head = false;
  while (std::getline(in, line)) {
    std::istringstream words(line);
    std::string key, a, b, flag;
    words >> key >> a;
    if (key == "metronet-package") {
      head = a == "1";
    } else if (key == "version") {
      version = core::wide(a);
    } else if (key == "protocol") {
      protocol = atoi(a.c_str());
    } else if (key == "launcher") {
      launcher_version = core::wide(a);
    } else if (key == "file") {
      Entry e;
      e.name = core::wide(a);
      words >> b;
      e.id = atoi(b.c_str());
      while (words >> flag) {
        if (flag == "loader") e.loader = true;
        if (flag == "keep") e.keep = true;
      }
      const void* body = nullptr;
      DWORD body_size = 0;
      if (!plain_name(e.name) || games_own(e.name) || entries.size() >= 64) return false;
      if (e.id < RES_PKG_FILES || e.id > RES_PKG_FILES + 99 || !bytes(e.id, body, body_size)) return false;
      entries.push_back(e);
    }
  }
  if (bytes(RES_PKG_INFO, data, size) && size < 65536) info.assign(static_cast<const char*>(data), size);
  bool has_mod = false;
  for (auto& e : entries) has_mod = has_mod || _wcsicmp(e.name.c_str(), L"metronet.asi") == 0;
  return head && has_mod && !version.empty() && core::compare_versions(version, L"0.0.1") >= 0;
}

Result install(const Package& p, const std::wstring& dir, std::wstring& log) {
  if (!exists(join(dir, kExe))) {
    log += L"в папке нет metro.exe\n";
    return Result::Failed;
  }
  if (!core::exe_supported(dir)) {
    log += L"в этой папке другая сборка игры: мод сделан для GOG-версии Metro 2033 Redux (metro.exe от сентября 2014) и сюда не "
           L"ставится\n";
    return Result::Failed;
  }
  if (core::game_running(dir)) {
    log += L"игра сейчас запущена: закройте её и повторите\n";
    return Result::Failed;
  }
  List list;
  list.load(dir);
  Result r = Result::Ok;
  for (const Entry& e : p.entries) {
    if (r != Result::Ok) break;
    std::wstring dst = join(dir, e.name);
    const void* data = nullptr;
    DWORD size = 0;
    if (!p.bytes(e.id, data, size)) {
      log += L"в лаунчере нет файла " + e.name + L"\n";
      r = Result::Failed;
      break;
    }
    if (e.loader && exists(dst) && !list.has(e.name) && !same_bytes(dst, data, size) && !our_loader(dst) &&
        read_all(dst).find("IsUltimateASILoader") != std::string::npos) {
      log += e.name + L": уже стоит Ultimate ASI Loader, используется он\n";
    } else if (e.keep && exists(dst)) {
      log += e.name + L": оставлены ваши настройки\n";
      if (!list.has(e.name)) list.files.push_back(e.name);
    } else {
      r = put(dir, e.name, data, size, list, log);
    }
  }
  list.save(dir, p);
  return r;
}

Result uninstall(const std::wstring& dir, std::wstring& log) {
  if (core::game_running(dir)) {
    log += L"игра сейчас запущена: закройте её и повторите\n";
    return Result::Failed;
  }
  List list;
  const bool listed = list.load(dir);
  if (!listed) {
    // Put there by hand, without the launcher: the mod's own names, and the loader if it is metronet's.
    for (const wchar_t* own : kOwn)
      if (exists(join(dir, own))) list.files.push_back(own);
    if (list.files.empty()) {
      log += L"в папке нет мода\n";
      return Result::Failed;
    }
    if (exists(join(dir, kLoader))) {
      if (our_loader(join(dir, kLoader))) list.files.push_back(kLoader);
      else log += std::wstring(kLoader) + L" не от мода: оставлен\n";
    }
  }
  Result r = Result::Ok;
  for (auto& f : list.files) drop(dir, f, log, r);
  if (r == Result::Ok) {
    for (auto& b : list.backups)
      if (plain_name(b.first) && !games_own(b.first) &&
          MoveFileExW(join(dir, b.second).c_str(), join(dir, b.first).c_str(), MOVEFILE_REPLACE_EXISTING))
        log += L"возвращён прежний " + b.first + L"\n";
    // What the mod wrote itself while the game ran (the saves of the game are not in this folder at all).
    Result quiet = Result::Ok;
    if (exists(join(dir, L"metronet.log"))) drop(dir, L"metronet.log", log, quiet);
    RemoveDirectoryW(join(dir, kBackupDir).c_str());  // only if empty
    DeleteFileW(join(dir, kList).c_str());
  }
  return r;
}

}  // namespace pkg
}  // namespace lz
