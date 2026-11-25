// metronet-launcher.exe: the window, the pointer and the keys, what each press does, the work in the
// background (asking GitHub, downloading, installing), and the modes without a window:
//   /install <game folder> [/quiet] [/package <launcher file>]  puts the mod in (from this launcher, or from
//                                                               another one's inside), exit code 0 or 2
//   /uninstall <game folder> [/quiet]                           takes it out
//   --shot <page> <file.png> [--scale N] [--steam 0|1] [--hover <name>] [--press <name>]
//                                                            a page of the design as a picture (no window):
//       main, install, update, downloading, checking, nogame, wrongbuild, offline, norelease, running, problem,
//       selfupdate, off, net, net-ip, net-nomod, graphics, graphics-nofile, controls, controls-wait,
//       controls-weapons, launcher, launcher-update
//   --check [--out file]                                     ask for the newest release, write what came
//   --where [--out file]                                     where the game is found from here
//   --after-update <version>                                 this launcher has just taken that one's place and was
//                                                            started by it: it says so and updates itself no further
// Tests: --settings <ini>, --cache <dir>, --repo <url> (the repository served from elsewhere), --pick <dir>
// (the answer of the folder dialog), --steam 0|1 (Steam "runs" or not), --userdir <dir> (the folder that
// stands for the game's own user folder: its user.cfg), --game-running 0|1 (the game "runs" or not),
// --drive "<script>" (the window stays hidden, nothing of the PC is asked for or shown: wait:ms, click:name,
// mouse:x,y, until:screen:ms, type:text, key:enter, key:esc, keydown:vk:scan:extended, rdown, mdown, xdown:n,
// slide:row:0..1, file:name, shot:file, state, cfg, quit), --out <file> (a log of what happened),
// --drive-next "<script>" (the script of the launcher this one starts in its place: without it a launcher
// run by a script starts no other).
// A launcher run by a script never asks the real GitHub unless --repo says so, never reads the player's own
// user.cfg (only the one of --userdir), and never starts the game.
#include "launcher/launcher.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <memory>
#include <thread>

#include "launcher/gamecfg.h"
#include "launcher/modset.h"
#include "launcher/res.h"

namespace lz {
namespace {

const wchar_t kClass[] = L"metronet-launcher";
const wchar_t kOnce[] = L"metronet-launcher-running";
const UINT WM_APP_CHECK = WM_APP + 1, WM_APP_STAGE = WM_APP + 2, WM_APP_PROGRESS = WM_APP + 3, WM_APP_GAME = WM_APP + 4,
           WM_APP_DONE = WM_APP + 5;

Model g;
HWND g_hwnd = nullptr;
ID2D1HwndRenderTarget* g_rt = nullptr;
std::unique_ptr<gfx::Canvas> g_canvas;
std::vector<Hit> g_hits;
std::atomic<bool> g_cancel{false};
std::atomic<int> g_gen{0};       // checks: only the newest one's answer counts
bool g_job = false;              // a download / install / removal runs
bool g_startup_check = false;    // the check of the start (it may install by itself)
bool g_self_auto = false;        // the job that runs is the launcher taking a newer one's place by itself, at the start
std::wstring g_after_update;     // the version of the launcher this one has just replaced (it started this one)
HANDLE g_once = nullptr;         // one launcher at a time
ULONGLONG g_drop_old_until = 0;  // the file of the launcher this one replaced is removed once that one has closed
net::Route g_route;              // the way to GitHub that worked last
Pending g_last_job = Pending::None;
ULONGLONG g_start = 0, g_confirm_until = 0, g_launch_until = 0, g_quit_at = 0, g_steam_at = 0;
bool g_tracking = false, g_dirty = true;

// tests
bool g_drive = false;
std::wstring g_out, g_pick, g_drive_next;
std::vector<std::wstring> g_script;
size_t g_script_at = 0;
ULONGLONG g_script_wait = 0, g_script_deadline = 0;

const wchar_t* screen_name(Screen s) {
  switch (s) {
    case Screen::Checking: return L"checking";
    case Screen::Update: return L"update";
    case Screen::Downloading: return L"downloading";
    case Screen::Ready: return L"ready";
    case Screen::Install: return L"install";
    case Screen::NoGame: return L"nogame";
    case Screen::WrongBuild: return L"wrongbuild";
    case Screen::Offline: return L"offline";
    case Screen::Running: return L"running";
    default: return L"problem";
  }
}

const wchar_t* mode_name(mset::Mode m) {
  return m == mset::Mode::Off ? L"off" : m == mset::Mode::Host ? L"host" : m == mset::Mode::Join ? L"join" : L"auto";
}

void logf(const std::wstring& line) {
  if (g_out.empty()) return;
  FILE* f = nullptr;
  if (_wfopen_s(&f, g_out.c_str(), L"ab") || !f) return;
  std::string s = core::narrow(L"[" + std::to_wstring(GetTickCount64() - g_start) + L"] " + line + L"\n");
  fwrite(s.data(), 1, s.size(), f);
  fclose(f);
}

void dirty() {
  g_dirty = true;
  if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

void set_screen(Screen s) {
  if (g.screen != s) logf(std::wstring(L"screen ") + screen_name(g.screen) + L" -> " + screen_name(s));
  g.screen = s;
  dirty();
}

void flash(const std::wstring& text, int ms = 4500) {
  g.flash = text;
  g.flash_until = GetTickCount64() + ms;
  logf(L"note: " + text);
  dirty();
}

// The settings of the mod and of the game, as their files have them now.
void load_settings_files() {
  mset::load(g.has_game ? g.set.game_dir : std::wstring());
  gcfg::load(g.has_game ? g.set.game_dir : std::wstring());
  g.capture = -1;
}

void refresh_installed() {
  g.has_game = core::has_game(g.set.game_dir);
  g.exe_ok = !g.has_game || core::exe_supported(g.set.game_dir);
  g.inst = g.has_game ? core::read_installed(g.set.game_dir) : Installed{};
  if (g.inst.present && !g.inst.protocol) g.inst.protocol = core::known_protocol(g.rel, g.inst.version);
  load_settings_files();
}

// What there is to install: GitHub's version if it is newer than the one inside this launcher (and its
// file can be had), else the one inside.
const Release& offer() { return core::target(g.rel, g.assets_blocked); }

Screen decide() {
  if (!g.has_game) return Screen::NoGame;
  if (!g.exe_ok) return Screen::WrongBuild;  // (another build of the game: nothing is installed there)
  if (!g.inst.present) return Screen::Install;
  if (g.inst.version.empty() || core::compare_versions(offer().version, g.inst.version) > 0) return Screen::Update;
  // Nothing newer: GitHub said so, or was not asked, or has no release at all yet - all of them fine.
  if ((g.rel.known && !g.assets_blocked) || g.unchecked || g.no_release) return Screen::Ready;
  return Screen::Offline;  // (nothing newer inside, and GitHub did not say)
}

void go_decided() { set_screen(decide()); }

// --- the work in the background -----------------------------------------------------------------

void start_check(bool quiet) {
  if (!core::has_repo()) {  // (nothing to ask: the mod inside this launcher is all there is)
    g.unchecked = true;
    g.rechecking = false;
    return;
  }
  int gen = ++g_gen;
  g.unchecked = false;
  if (quiet || !g.has_game || !g.exe_ok || !g.inst.present) g.rechecking = true;
  else set_screen(Screen::Checking);
  logf(L"check: asking");
  dirty();
  Settings s = g.set;
  HWND hwnd = g_hwnd;
  std::thread([s, hwnd, gen]() {
    static std::atomic<bool> never{false};
    auto* r = new core::CheckResult(core::check(s, never));
    if (!PostMessageW(hwnd, WM_APP_CHECK, static_cast<WPARAM>(gen), reinterpret_cast<LPARAM>(r))) delete r;
  }).detach();
}

struct Done {
  core::Step step;
  Pending kind;
  std::wstring log;      // what the install or the removal said, line by line
  std::wstring package;  // the newer launcher file it came from (empty: this launcher)
  Release rel;           // what was being installed
};

void start_job(Pending kind) {
  if (g_job) return;
  Release rel = kind == Pending::Launcher ? g.rel : offer();
  bool download = kind == Pending::Launcher || (kind == Pending::Install && core::newer_on_github(g.rel, g.assets_blocked));
  if (kind == Pending::Install && !rel.known) {
    flash(L"В этом лаунчере нет мода: скачайте лаунчер заново");
    return;
  }
  if (kind == Pending::Install && !g.exe_ok) {
    flash(L"В этой папке другая сборка игры: мод сюда не ставится");
    return;
  }
  g_job = true;
  g_last_job = kind;
  g_cancel = false;
  g.pending = kind;
  g.stage = kind == Pending::Uninstall ? 2 : download ? 0 : 1;
  g.got = 0;
  g.total = download ? rel.pkg_size : 0;
  g.confirm_uninstall = false;
  g.tab = Tab::Game;  // (the work is shown there)
  set_screen(Screen::Downloading);
  logf(kind == Pending::Uninstall ? std::wstring(L"job: uninstall")
       : kind == Pending::Launcher ? L"job: a newer launcher, " + rel.launcher_version
       : L"job: install " + rel.version + (download ? L" from GitHub" : L" from this launcher"));
  Settings s = g.set;
  net::Route route = g_route;
  HWND hwnd = g_hwnd;
  std::thread([rel, download, s, route, hwnd, kind]() {
    core::UpdateHooks hooks;
    hooks.stage = [hwnd](int stage) { PostMessageW(hwnd, WM_APP_STAGE, static_cast<WPARAM>(stage), 0); };
    hooks.progress = [hwnd](uint64_t got, uint64_t total) {
      static ULONGLONG last = 0;
      ULONGLONG now = GetTickCount64();
      if (got == total || now - last >= 40) {
        last = now;
        PostMessageW(hwnd, WM_APP_PROGRESS, static_cast<WPARAM>(got), static_cast<LPARAM>(total));
      }
    };
    hooks.game_open = [hwnd](bool waiting) { PostMessageW(hwnd, WM_APP_GAME, waiting ? 1 : 0, 0); };
    auto* d = new Done{core::Step::Ok, kind, L"", L"", rel};
    if (kind == Pending::Uninstall) d->step = core::uninstall(s, hooks, g_cancel, &d->log);
    else if (kind == Pending::Launcher) d->step = core::fetch_launcher(rel, s, route, hooks, g_cancel, &d->log, &d->package);
    else d->step = core::install(rel, download, s, route, hooks, g_cancel, &d->log, &d->package);
    if (!PostMessageW(hwnd, WM_APP_DONE, 0, reinterpret_cast<LPARAM>(d))) delete d;
  }).detach();
}

void problem(const wchar_t* title, const std::wstring& text, const wchar_t* primary, Fix fix) {
  g.problem_title = title;
  g.problem_text = text;
  g.problem_primary = primary;
  g.problem_secondary = L"НАЗАД";
  g.fix = fix;
  g.fix2 = Fix::Back;
  g.tab = Tab::Game;
  set_screen(Screen::Problem);
  logf(std::wstring(L"problem: ") + title);
}

// The line of a log that says what went wrong: the last one that begins "не удалось" or has an error in it.
std::wstring reason(const std::wstring& log) {
  std::wstring last, why;
  for (size_t p = 0; p < log.size();) {
    size_t e = log.find(L'\n', p);
    if (e == std::wstring::npos) e = log.size();
    std::wstring line = log.substr(p, e - p);
    if (!line.empty()) last = line;
    if (line.rfind(L"не удалось", 0) == 0 || line.find(L"не уда") != std::wstring::npos || line.find(L"нет ") != std::wstring::npos ||
        line.find(L"запущена") != std::wstring::npos || line.find(L"другая сборка") != std::wstring::npos)
      why = line;
    p = e + 1;
  }
  std::wstring out = why.empty() ? last : why;
  if (!out.empty()) out[0] = static_cast<wchar_t>(towupper(out[0]));
  return out.empty() ? L"Причина не названа" : out;
}

// A newer launcher file is at hand (downloaded and checked): it takes this one's place. True: it has.
bool take_newer_launcher(const std::wstring& file, bool say) {
  pkg::Package p;
  if (file.empty() || !p.open(file) || core::compare_versions(p.launcher_version, kLauncherVersion) <= 0) return false;
  if (core::replace_self(file)) {
    g.swapped = true;
    logf(L"launcher replaced by " + p.launcher_version);
    if (say) flash(L"Лаунчер обновлён до " + p.launcher_version + L": новая версия откроется при следующем запуске", 7000);
    return true;
  }
  logf(L"launcher NOT replaced");
  if (say) flash(L"Лаунчер не смог заменить свой файл: скачайте новый со страницы релиза", 7000);
  if (say && g.set.self_skip != p.launcher_version) {  // (a folder closed to its user: not tried by itself at every start)
    g.set.self_skip = p.launcher_version;
    core::save_settings(g.set);
  }
  return false;
}

// The newer launcher is in this one's place: it is started, and this one closes. A launcher run by a
// script starts another only when the script says what that one is to do (--drive-next).
void restart_self() {
  if (g_drive && g_drive_next.empty()) {
    logf(L"restart: not in a script");
    return;
  }
  wchar_t self[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  std::wstring cmd = core::quoted(self) + L" --after-update " + kLauncherVersion;
  if (!core::g_settings_path.empty()) cmd += L" --settings " + core::quoted(core::g_settings_path);
  if (!core::g_cache_dir.empty()) cmd += L" --cache " + core::quoted(core::g_cache_dir);
  if (!core::g_repo_url.empty()) cmd += L" --repo " + core::quoted(core::g_repo_url);
  if (core::g_steam >= 0) cmd += core::g_steam ? L" --steam 1" : L" --steam 0";
  if (core::g_game >= 0) cmd += core::g_game ? L" --game-running 1" : L" --game-running 0";
  if (!gcfg::g_user_dir.empty()) cmd += L" --userdir " + core::quoted(gcfg::g_user_dir);
  if (!g_pick.empty()) cmd += L" --pick " + core::quoted(g_pick);
  if (!g_out.empty()) cmd += L" --out " + core::quoted(g_out);
  if (g_drive) cmd += L" --drive " + core::quoted(g_drive_next);
  if (g_once) {  // (the one that starts must not find this one and step back)
    ReleaseMutex(g_once);
    CloseHandle(g_once);
    g_once = nullptr;
  }
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (CreateProcessW(self, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    logf(L"restart: the newer launcher is started");
    g_cancel = true;
    DestroyWindow(g_hwnd);
    return;
  }
  logf(L"restart: FAILED, error " + std::to_wstring(GetLastError()));
  if (!g_drive) g_once = CreateMutexW(nullptr, TRUE, kOnce);
}

// A newer launcher on GitHub takes this one's place by itself: at the start, when nothing else goes on.
bool wants_self_update() {
  return g.set.auto_self && g_after_update.empty() && !g_job && !g.swapped && !g.launching && g.tab == Tab::Game && g.rel.full &&
         !g.assets_blocked && !g.rel.launcher_version.empty() && g.rel.launcher_version != g.set.self_skip &&
         core::compare_versions(g.rel.launcher_version, kLauncherVersion) > 0;
}

// What the start goes on with once the launcher itself is settled: a newer mod installed without a question.
void after_self_update() {
  if (g.set.auto_install && g.screen == Screen::Update && !g_job) start_job(Pending::Install);
}

void on_done(Done* d) {
  g_job = false;
  g.pending = Pending::None;
  const bool self = d->kind == Pending::Launcher && g_self_auto;
  g_self_auto = false;
  logf(L"job done: step " + std::to_wstring(static_cast<int>(d->step)));
  for (size_t p = 0; p < d->log.size();) {
    size_t e = d->log.find(L'\n', p);
    if (e == std::wstring::npos) e = d->log.size();
    if (e > p) logf(L"  " + d->log.substr(p, e - p));
    p = e + 1;
  }
  if (self && d->step != core::Step::Ok) {  // (done by itself: no screen about it, the start goes on as it would)
    go_decided();
    if (d->step != core::Step::Cancelled) {
      flash(d->step == core::Step::NetFail ? std::wstring(L"Лаунчер не обновился: загрузка не удалась. Играть можно как раньше")
            : d->step == core::Step::BadSum ? std::wstring(L"Лаунчер не обновился: файл с GitHub повреждён. Играть можно как раньше")
            : L"Лаунчер не обновился: " + reason(d->log), 7000);
      after_self_update();
    }
    delete d;
    return;
  }
  switch (d->step) {
    case core::Step::Ok:
      refresh_installed();
      if (d->kind == Pending::Install) {
        if (!d->rel.notes.empty()) {  // the notes of what is installed now
          g.set.notes_version = d->rel.version;
          g.set.notes_title = d->rel.title;
          g.set.notes_date = d->rel.date;
          g.set.notes = d->rel.notes;
          core::save_settings(g.set);
        }
        go_decided();
        flash(L"Мод установлен: версия " + g.inst.version);
        take_newer_launcher(d->package, false);
      } else if (d->kind == Pending::Launcher) {
        go_decided();
        if (take_newer_launcher(d->package, true)) restart_self();
        if (g_hwnd && self) after_self_update();  // (this launcher goes on: its file could not be replaced, or a script runs it)
      } else {
        go_decided();
        flash(L"Мод удалён из папки игры");
      }
      break;
    case core::Step::Cancelled:
      go_decided();
      break;
    case core::Step::NetFail:
      problem(L"Загрузка не удалась",
              L"Сервер загрузок GitHub не ответил, или связь оборвалась. Проверьте интернет и прокси на станции ЛАУНЧЕР.",
              L"ПОВТОРИТЬ", Fix::Retry);
      break;
    case core::Step::BadSum:
      problem(L"Файл повреждён",
              L"Контрольная сумма скачанного файла не совпала с той, что указана в релизе. Файл удалён — скачайте его ещё раз.",
              L"СКАЧАТЬ ЗАНОВО", Fix::Retry);
      break;
    case core::Step::BadPackage:
      problem(L"В релизе не тот файл", reason(d->log) + L". Попробуйте позже или скачайте лаунчер со страницы релиза.",
              L"ПОВТОРИТЬ", Fix::Retry);
      break;
    case core::Step::Denied:
      problem(L"Нужно разрешение Windows",
              L"Игра лежит в папке, куда обычной программе писать нельзя. Windows спросит разрешение — без него мод не поставить и не убрать.",
              L"ПОВТОРИТЬ", Fix::Retry);
      break;
    default:
      problem(d->kind == Pending::Uninstall ? L"Удаление не удалось" : L"Установка не удалась", reason(d->log) + L".", L"ПОВТОРИТЬ",
              Fix::Retry);
      break;
  }
  delete d;
}

void on_check(int gen, core::CheckResult* r) {
  if (gen != g_gen.load()) {
    delete r;
    return;
  }
  logf(L"check: " + (r->rel.known ? L"version " + r->rel.version + (r->rel.full ? L" (described)" : L" (the version only)")
                     : r->no_release ? std::wstring(L"no release yet") : std::wstring(L"no answer")) +
       (r->assets_blocked ? L", files blocked" : L"") + (r->route.label.empty() ? L"" : L", " + r->route.label));
  for (size_t p = 0; p < r->log.size();) {
    size_t e = r->log.find(L'\n', p);
    logf(L"  " + r->log.substr(p, e - p));
    p = e == std::wstring::npos ? r->log.size() : e + 1;
  }
  g.rel = r->rel;
  g.assets_blocked = r->assets_blocked;
  g.no_release = r->no_release;
  if (r->rel.known) g_route = r->route;
  g.rechecking = false;
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t at[16];
  swprintf(at, 16, L"%02d:%02d", t.wHour, t.wMinute);
  g.checked_at = r->rel.known ? at : L"";
  g.via = r->rel.known && r->route.kind == 2 ? r->route.label : L"";
  const int waits = g.capture;
  refresh_installed();
  g.capture = waits;  // (an answer of GitHub does not end a wait for a key)
  bool startup = g_startup_check;
  g_startup_check = false;
  delete r;
  if (g.screen == Screen::Downloading || g.screen == Screen::Running || g.screen == Screen::Problem) return;
  go_decided();
  if (startup && wants_self_update()) {
    g_self_auto = true;
    start_job(Pending::Launcher);
    return;
  }
  if (startup && g.set.auto_install && g.screen == Screen::Update) start_job(Pending::Install);
}

// --- what a press does -------------------------------------------------------------------------

void open_url(const std::wstring& url) {
  logf(L"open " + url);
  if (!g_drive) ShellExecuteW(g_hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// A text file of the mod (its log, its description), in Notepad: such a file may have no program of its own.
void open_text(const std::wstring& file) {
  logf(L"open " + file);
  if (!g_drive) ShellExecuteW(g_hwnd, L"open", L"notepad.exe", core::quoted(file).c_str(), nullptr, SW_SHOWNORMAL);
}

void play() {
  if (!g.has_game || !g.exe_ok || g.launching) return;
  logf(L"play");
  if (!g_drive && !core::launch_game(g.set.game_dir)) {  // (a test never starts the game)
    flash(L"Не удалось запустить metro.exe");
    return;
  }
  g.launching = true;
  g_launch_until = GetTickCount64() + 5000;
  if (g.set.close_on_play && !g_drive) g_quit_at = GetTickCount64() + 2500;
  dirty();
}

void choose_folder() {
  std::wstring dir;
  if (g_drive) dir = g_pick;
  else if (!core::pick_folder(g_hwnd, g.set.game_dir, dir)) return;
  if (dir.empty()) return;
  if (!core::has_game(dir)) {
    flash(L"В этой папке нет metro.exe");
    return;
  }
  logf(L"folder chosen");
  g.set.game_dir = core::trimmed(dir);
  core::save_settings(g.set);
  refresh_installed();
  go_decided();
  if (!g.rel.known && !g.no_release && !g.rechecking) start_check(true);
}

void fix(Fix f) {
  switch (f) {
    case Fix::Retry:
      start_job(g_last_job == Pending::None ? Pending::Install : g_last_job);
      break;
    case Fix::Recheck:
      start_check(false);
      break;
    default:
      go_decided();
      break;
  }
}

// A line of metronet.ini was to be written: what the file has now, and why not if it was not written.
void wrote(const wchar_t* what, bool ok) {
  const mset::Values& v = mset::get();
  logf(std::wstring(L"set ") + what + L": mode " + mode_name(v.mode) + L" names " + (v.names ? L"1" : L"0") + L" ff " +
       (v.friendly_fire ? L"1" : L"0") + L" name \"" + v.name + L"\" address \"" + v.address + L"\"" + (ok ? L"" : L": NOT WRITTEN"));
  if (!ok) flash(mset::error().empty() ? std::wstring(L"Настройка не записалась") : mset::error(), 8000);
}

// The same for a line of the game's user.cfg.
void wrote_cfg(const std::wstring& what, bool ok) {
  logf(L"cfg " + what + (ok ? L"" : L": NOT WRITTEN"));
  if (!ok) flash(gcfg::error().empty() ? std::wstring(L"Настройка не записалась") : gcfg::error(), 9000);
}

// The field that is typed into is left: what it holds is written (`keep` false: thrown away).
void blur(bool keep = true) {
  const int was = g.focus;
  g.focus = F_None;
  if (was == F_Proxy) {
    core::save_settings(g.set);
  } else if (was == F_Name && keep) {
    if (g.edit != mset::get().name) wrote(L"name", mset::set_name(g.edit));
  } else if (was == F_Address && keep) {
    if (g.edit != mset::get().address) wrote(L"address", mset::set_address(g.edit));
  }
  g.edit.clear();
  if (was) dirty();
}

// A row of the game's settings: "<", ">" or its switch was pressed.
void act_row(int id) {
  const gcfg::Row r = static_cast<gcfg::Row>((id - H_Row) / kRowParts);
  const int part = (id - H_Row) % kRowParts;
  if (part == P_Slide) return;  // (the knob follows the pointer: slide_to)
  const bool ok = gcfg::set(r, gcfg::value(r) + (part == P_Prev ? -1 : 1));
  wrote_cfg(core::wide(gcfg::key(r)) + L" = " + core::wide(gcfg::raw(r)), ok);
}

// The knob of a slider is put where the pointer is.
void slide_to(gcfg::Row r, float fraction) {
  const std::string was = gcfg::raw(r);
  const bool ok = gcfg::slide(r, fraction);
  if (!ok || gcfg::raw(r) != was) wrote_cfg(core::wide(gcfg::key(r)) + L" = " + core::wide(gcfg::raw(r)), ok);
  dirty();
}

// УПРАВЛЕНИЕ: the key (or mouse button) for the row that waits for one. An empty name: the game has none for it.
void bind_key(const std::string& token) {
  const int i = g.capture;
  if (i < 0 || i >= gcfg::binds()) return;
  if (token.empty()) {  // (the row goes on waiting)
    flash(L"Эту клавишу игре не назначить: выберите другую");
    return;
  }
  g.capture = -1;
  const std::string action = gcfg::action(i);
  const bool ok = gcfg::bind(i, token);
  wrote_cfg(L"bind " + core::wide(action) + L" " + core::wide(token), ok);
  dirty();
}

void act(int id) {
  logf(L"press " + std::to_wstring(id));
  const int field_of = id == H_ProxyField ? F_Proxy : id == H_NameField ? F_Name : id == H_AddressField ? F_Address : F_None;
  if (g.focus && g.focus != field_of) blur();
  if (id != H_Uninstall) g.confirm_uninstall = false;
  if (id != H_Bind + g.capture) g.capture = -1;  // (a press elsewhere: the row waits no more)
  if (id >= H_Row && id < H_Row + gcfg::kRows * kRowParts) {
    act_row(id);
    dirty();
    return;
  }
  if (id >= H_Bind && id < H_Bind + gcfg::binds()) {
    if (g.capture == id - H_Bind) bind_key(gcfg::token_of_mouse(0));  // (a second press on the waiting row: the mouse's own button)
    else g.capture = id - H_Bind;
    dirty();
    return;
  }
  if (id >= H_BindReset && id < H_BindReset + gcfg::binds()) {
    const std::string action = gcfg::action(id - H_BindReset);
    wrote_cfg(L"bind " + core::wide(action) + L" back to the game's own", gcfg::bind_reset(id - H_BindReset));
    dirty();
    return;
  }
  switch (id) {
    case H_Close:
      g_cancel = true;
      DestroyWindow(g_hwnd);
      return;
    case H_Min:
      if (!g_drive) ShowWindow(g_hwnd, SW_MINIMIZE);  // (a script's window is never on the desktop, minimized or not)
      return;
    case H_Tab0:
    case H_Tab1:
    case H_Tab2:
    case H_Tab3:
    case H_Tab4:
      g.tab = static_cast<Tab>(id - H_Tab0);
      load_settings_files();  // (the files may have been changed: by hand, by the game)
      break;
    case H_Primary:
      if (!view::primary_enabled(g)) break;
      switch (g.screen) {
        case Screen::Checking:
        case Screen::Ready:
        case Screen::Offline: play(); break;
        case Screen::Update:
        case Screen::Install: start_job(Pending::Install); break;
        case Screen::NoGame:
        case Screen::WrongBuild: choose_folder(); break;
        case Screen::Running:
          if (core::close_game(g.set.game_dir)) flash(L"Игру попросили закрыться");
          else flash(L"Окно игры не найдено: закройте её сами");
          break;
        case Screen::Problem: fix(g.fix); break;
        default: break;
      }
      break;
    case H_Secondary:
      switch (g.screen) {
        case Screen::Update: play(); break;
        case Screen::Running: g_cancel = true; break;
        case Screen::Problem: fix(g.fix2); break;
        default: break;
      }
      break;
    case H_Cancel:
      g_cancel = true;
      break;
    // СЕТЬ: each press writes its line of metronet.ini at once
    case H_Coop:
      wrote(L"mode", mset::set_mode(mset::get().mode == mset::Mode::Off ? mset::Mode::Auto : mset::Mode::Off));
      break;
    case H_Names:
      wrote(L"names", mset::set_names(!mset::get().names));
      break;
    case H_FriendlyFire:
      wrote(L"friendly_fire", mset::set_friendly_fire(!mset::get().friendly_fire));
      break;
    case H_IpPrev:
    case H_IpNext: {  // not needed (through Steam) - I am the host - I come in to a host
      const mset::Mode ways[3] = {mset::Mode::Auto, mset::Mode::Host, mset::Mode::Join};
      const mset::Mode now = mset::get().mode;
      const int at = now == mset::Mode::Host ? 1 : now == mset::Mode::Join ? 2 : 0;
      wrote(L"mode", mset::set_mode(ways[(at + (id == H_IpNext ? 1 : 2)) % 3]));
      break;
    }
    case H_NameField:
      if (g.focus != F_Name) {
        g.focus = F_Name;
        g.edit = mset::get().name;
      }
      break;
    case H_AddressField:
      if (g.focus != F_Address) {
        g.focus = F_Address;
        g.edit = mset::get().address;
      }
      break;
    // УПРАВЛЕНИЕ: the page of the keys
    case H_KeysPrev:
    case H_KeysNext:
      if (gcfg::pages()) g.keys_page = (std::min(g.keys_page, gcfg::pages() - 1) + (id == H_KeysNext ? 1 : gcfg::pages() - 1)) % gcfg::pages();
      break;
    // ЛАУНЧЕР
    case H_Folder:
      if (!g_job) choose_folder();
      break;
    case H_Recheck:
      if (!g.rechecking && g.screen != Screen::Checking) start_check(true);
      break;
    case H_Repo:
      if (core::has_repo()) open_url(core::repo());
      break;
    case H_NewLauncher:
      if (g.rel.full && !g_job && !g.swapped) start_job(Pending::Launcher);
      break;
    case H_AutoCheck:
      g.set.auto_check = !g.set.auto_check;
      core::save_settings(g.set);
      break;
    case H_AutoInstall:
      g.set.auto_install = !g.set.auto_install;
      core::save_settings(g.set);
      break;
    case H_AutoSelf:
      g.set.auto_self = !g.set.auto_self;
      g.set.self_skip.clear();  // (asked for again: tried again)
      core::save_settings(g.set);
      break;
    case H_ClosePlay:
      g.set.close_on_play = !g.set.close_on_play;
      core::save_settings(g.set);
      break;
    case H_ProxyPrev:
    case H_ProxyNext:
      g.set.proxy = (g.set.proxy + (id == H_ProxyNext ? 1 : 2)) % 3;
      g.focus = g.set.proxy == 2 && g.set.proxy_addr.empty() ? F_Proxy : F_None;
      core::save_settings(g.set);
      break;
    case H_ProxyField:
      g.focus = F_Proxy;
      break;
    case H_OpenLog: {
      std::wstring log = core::join(g.set.game_dir, L"metronet.log");
      if (g.has_game && core::exists(log)) open_text(log);
      else flash(L"Журнала пока нет: игра с модом ещё не запускалась");
      break;
    }
    case H_OpenReadme: {
      std::wstring readme = core::join(g.set.game_dir, L"metronet-README.md");
      if (g.has_game && core::exists(readme)) open_text(readme);
      else flash(L"Описание лежит в папке игры вместе с модом: сначала установите мод");
      break;
    }
    case H_Reinstall:
      if (!g.has_game) flash(L"Сначала укажите папку игры");
      else if (!g_job) start_job(Pending::Install);
      break;
    case H_Uninstall:
      if (!g.inst.present) {
        flash(L"Мод не установлен");
      } else if (!g.confirm_uninstall) {
        g.confirm_uninstall = true;
        g_confirm_until = GetTickCount64() + 5000;
      } else if (!g_job) {
        start_job(Pending::Uninstall);
      }
      break;
    default:
      break;
  }
  dirty();
}

// --- the window -----------------------------------------------------------------------------

float scale_of(HWND hwnd) { return GetDpiForWindow(hwnd) / 96.0f; }

void discard_target() {
  g_canvas.reset();
  if (g_rt) g_rt->Release();
  g_rt = nullptr;
}

void paint(HWND hwnd) {
  const ULONGLONG began = GetTickCount64();
  const bool first = !g_rt;
  if (!g_rt) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    float dpi = static_cast<float>(GetDpiForWindow(hwnd));
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpi, dpi);
    if (FAILED(gfx::d2d()->CreateHwndRenderTarget(
            props, D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(rc.right, rc.bottom)), &g_rt)))
      return;
    g_canvas = std::make_unique<gfx::Canvas>(g_rt);
  }
  const ULONGLONG made = GetTickCount64();
  g.time = (GetTickCount64() - g_start) / 1000.0;
  g_rt->BeginDraw();
  g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
  view::draw(*g_canvas, g, &g_hits);
  HRESULT hr = g_rt->EndDraw();
  if (g_drive) {
    static bool said = false;
    if (!said || FAILED(hr)) logf(FAILED(hr) ? L"window: drawing FAILED" : L"window: drawn");
    if (first || !said)  // (how long the first picture takes: the target, then the map and the page)
      logf(L"window: target " + std::to_wstring(made - began) + L" ms, picture " + std::to_wstring(GetTickCount64() - made) + L" ms");
    said = true;
  }
  if (hr == D2DERR_RECREATE_TARGET) discard_target();
  g_dirty = false;
}

// A key of the keyboard as DirectInput numbers it (the game names its keys by that): its scan code, with
// 0x80 for the keys of the extended set. Pause and Num Lock come the other way round.
int dik_of(WPARAM vk, LPARAM lp) {
  if (vk == VK_PAUSE) return 0xC5;
  if (vk == VK_NUMLOCK) return 0x45;
  int scan = static_cast<int>((lp >> 16) & 0xFF);
  if (!scan) scan = static_cast<int>(MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC)) & 0xFF;
  return scan | ((lp >> 24) & 1 ? 0x80 : 0);
}

int hit_at(float x, float y) {
  for (size_t i = g_hits.size(); i-- > 0;) {
    const Hit& h = g_hits[i];
    D2D1_POINT_2F p = h.inv.TransformPoint(D2D1::Point2F(x, y));
    if (p.x >= h.r.left && p.x < h.r.right && p.y >= h.r.top && p.y < h.r.bottom) return h.id;
  }
  return H_None;
}

bool is_slider(int id) { return id >= H_Row && id < H_Row + gcfg::kRows * kRowParts && (id - H_Row) % kRowParts == P_Slide; }

// The pointer is at x, y of the window over (or dragging) a slider: where along its line that is.
void slide_at(int id, float x, float y) {
  for (const Hit& h : g_hits) {
    if (h.id != id) continue;
    const D2D1_POINT_2F p = h.inv.TransformPoint(D2D1::Point2F(x, y));
    slide_to(static_cast<gcfg::Row>((id - H_Row) / kRowParts), (p.x - (h.r.left + 8)) / (h.r.right - h.r.left - 16));  // (the line's ends are 8 inside)
    return;
  }
}

bool moving() { return g.screen == Screen::Checking || g.screen == Screen::Downloading || g.rechecking || g.focus != F_None; }

int hit_by_name(const std::wstring& n) {
  static const struct {
    const wchar_t* name;
    int id;
  } names[] = {{L"min", H_Min},                {L"close", H_Close},             {L"tab.game", H_Tab0},
               {L"tab.net", H_Tab1},           {L"tab.graphics", H_Tab2},       {L"tab.controls", H_Tab3},
               {L"tab.launcher", H_Tab4},      {L"primary", H_Primary},         {L"secondary", H_Secondary},
               {L"cancel", H_Cancel},          {L"coop", H_Coop},               {L"names", H_Names},
               {L"ff", H_FriendlyFire},        {L"name", H_NameField},          {L"ip.prev", H_IpPrev},
               {L"ip.next", H_IpNext},         {L"address", H_AddressField},    {L"keys.prev", H_KeysPrev},
               {L"keys.next", H_KeysNext},     {L"folder", H_Folder},           {L"reinstall", H_Reinstall},
               {L"uninstall", H_Uninstall},    {L"recheck", H_Recheck},         {L"newlauncher", H_NewLauncher},
               {L"autocheck", H_AutoCheck},    {L"autoself", H_AutoSelf},       {L"autoinstall", H_AutoInstall},
               {L"closeplay", H_ClosePlay},    {L"proxy.prev", H_ProxyPrev},    {L"proxy.next", H_ProxyNext},
               {L"proxy_field", H_ProxyField}, {L"log", H_OpenLog},             {L"readme", H_OpenReadme},
               {L"repo", H_Repo}};
  for (auto& x : names)
    if (n == x.name) return x.id;
  // the game's settings: g.quality.next, g.vsync (a switch), g.fov (a slider); the keys: bind.jump, bind.jump.reset
  if (n.rfind(L"g.", 0) == 0) {
    const size_t dot = n.find(L'.', 2);
    const std::wstring key = n.substr(2, dot == std::wstring::npos ? dot : dot - 2), part = dot == std::wstring::npos ? L"" : n.substr(dot + 1);
    for (int r = 0; r < gcfg::kRows; ++r) {
      const gcfg::Row row = static_cast<gcfg::Row>(r);
      if (key != core::wide(gcfg::key(row))) continue;
      const int base = H_Row + r * kRowParts;
      if (gcfg::kind(row) == gcfg::Kind::Slider) return base + P_Slide;
      return part == L"prev" ? base + P_Prev : base + P_Next;
    }
  } else if (n.rfind(L"bind.", 0) == 0) {
    const bool reset = n.size() > 11 && n.compare(n.size() - 6, 6, L".reset") == 0;
    const std::wstring action = n.substr(5, n.size() - 5 - (reset ? 6 : 0));
    for (int i = 0; i < gcfg::binds(); ++i)
      if (action == core::wide(gcfg::action(i))) return (reset ? H_BindReset : H_Bind) + i;
  }
  return H_None;
}

// Where things stand, for the log of a script.
void log_state() {
  const mset::Values& v = mset::get();
  logf(std::wstring(L"state: screen ") + screen_name(g.screen) + L" tab " + std::to_wstring(static_cast<int>(g.tab)) + L" game " +
       (g.has_game ? L"1" : L"0") + L" exe " + (g.exe_ok ? L"1" : L"0") + L" mod " +
       (g.inst.present ? (g.inst.version.empty() ? std::wstring(L"?") : g.inst.version) : std::wstring(L"none")) + L" steam " +
       (g.steam ? L"1" : L"0") + L" norelease " + (g.no_release ? L"1" : L"0") + L" github " + (g.rel.known ? g.rel.version : L"-"));
  logf(std::wstring(L"state: ini ") + (mset::has_ini() ? L"1" : L"0") + L" mode " + mode_name(v.mode) + L" names " + (v.names ? L"1" : L"0") +
       L" ff " + (v.friendly_fire ? L"1" : L"0") + L" name \"" + v.name + L"\" address \"" + v.address + L"\" focus " +
       std::to_wstring(g.focus));
  logf(L"state: file " + mset::ini_file());
}

// What the launcher has of the game's user.cfg, for the log of a script: every row, every key.
void log_cfg() {
  logf(std::wstring(L"cfg: file ") + (gcfg::has_file() ? L"1" : L"0") + L" page " + std::to_wstring(g.keys_page) + L"/" +
       std::to_wstring(gcfg::pages()) + L" waits " + std::to_wstring(g.capture) + L" path " + gcfg::file());
  if (!gcfg::has_file()) return;
  for (int r = 0; r < gcfg::kRows; ++r) {
    const gcfg::Row row = static_cast<gcfg::Row>(r);
    const bool slider = gcfg::kind(row) == gcfg::Kind::Slider;
    logf(L"cfg: row " + core::wide(gcfg::key(row)) + L" " +
         (slider ? L"at " + std::to_wstring(static_cast<int>(gcfg::fraction(row) * 1000 + 0.5f))
                 : std::to_wstring(gcfg::value(row)) + L"/" + std::to_wstring(gcfg::count(row))) +
         L" changed " + (gcfg::changed(row) ? L"1" : L"0") + L" | " + core::wide(gcfg::raw(row)) + L" | " +
         (slider ? gcfg::text(row) : gcfg::name(row, gcfg::value(row))));
  }
  for (int i = 0; i < gcfg::binds(); ++i)
    logf(L"cfg: bind " + core::wide(gcfg::action(i)) + L" " + core::wide(gcfg::bound(i)) + L" changed " + (gcfg::bind_changed(i) ? L"1" : L"0") +
         L" own " + (gcfg::bind_is_default(i) ? L"1" : L"0") + L" | " + gcfg::action_label(i) + L" | " + gcfg::key_label(gcfg::bound(i)));
}

// The line of user.cfg that begins with these words, read from the disk now and by nothing of the launcher's
// own reading of it: what a change really left in the file.
void log_file_line(const std::wstring& begins) {
  const std::string text = core::read_all(gcfg::file()), want = core::narrow(begins) + " ";
  for (size_t p = 0; p < text.size();) {
    size_t e = text.find('\n', p);
    if (e == std::string::npos) e = text.size();
    std::string line = text.substr(p, e - p);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.compare(0, want.size(), want) == 0) {
      logf(L"file: " + core::wide(line));
      return;
    }
    p = e + 1;
  }
  logf(L"file: (no line " + begins + L")");
}

bool save_shot(const std::wstring& file, float scale) {
  g.time = (GetTickCount64() - g_start) / 1000.0;
  return gfx::save_png(file.c_str(), kW, kH, scale, [](gfx::Canvas& c) { view::draw(c, g, nullptr); });
}

std::vector<std::wstring> split(const std::wstring& s, wchar_t sep) {
  std::vector<std::wstring> out;
  size_t p = 0;
  while (p <= s.size()) {
    size_t e = s.find(sep, p);
    if (e == std::wstring::npos) e = s.size();
    if (e > p) out.push_back(s.substr(p, e - p));
    p = e + 1;
  }
  return out;
}

// The test script: one step at a time, from the timer.
void drive() {
  ULONGLONG now = GetTickCount64();
  while (g_script_at < g_script.size()) {
    if (now < g_script_wait) return;
    const std::wstring& cmd = g_script[g_script_at];
    size_t colon = cmd.find(L':');
    std::wstring op = cmd.substr(0, colon), arg = colon == std::wstring::npos ? L"" : cmd.substr(colon + 1);
    if (op == L"wait") {
      g_script_wait = now + _wtoi(arg.c_str());
      ++g_script_at;
      return;
    }
    if (op == L"until") {  // until:<screen>:<ms>
      size_t c2 = arg.find(L':');
      std::wstring want = arg.substr(0, c2);
      if (!g_script_deadline) g_script_deadline = now + (c2 == std::wstring::npos ? 20000 : _wtoi(arg.c_str() + c2 + 1));
      if (want == screen_name(g.screen) && !g.rechecking) {
        logf(L"until " + want + L": reached");
        g_script_deadline = 0;
        ++g_script_at;
        continue;
      }
      if (now > g_script_deadline) {
        logf(L"until " + want + L": TIMEOUT on " + screen_name(g.screen));
        g_script_deadline = 0;
        ++g_script_at;
        continue;
      }
      return;
    }
    ++g_script_at;
    if (op == L"click") {
      int id = hit_by_name(arg);
      bool there = false;
      std::vector<Hit> hits;  // (what is on the screen now: drawn aside)
      gfx::save_png(nullptr, kW, kH, 1, [&](gfx::Canvas& c) { view::draw(c, g, &hits); });
      for (auto& h : hits) there = there || h.id == id;
      logf(L"click " + arg + (there && id ? L"" : L": NOT ON THIS SCREEN"));
      if (there && id) act(id);
      if (!g_hwnd) return;
    } else if (op == L"mouse") {  // mouse:x,y - a press at that place, as the window gets it (the real pointer is not moved)
      paint(g_hwnd);
      float s = scale_of(g_hwnd);
      int mx = static_cast<int>(_wtof(arg.c_str()) * s), my = static_cast<int>(_wtof(arg.c_str() + arg.find(L',') + 1) * s);
      LPARAM at = MAKELPARAM(mx, my);
      SendMessageW(g_hwnd, WM_MOUSEMOVE, 0, at);
      logf(L"mouse " + arg + L": over " + std::to_wstring(g.hover) + L" of " + std::to_wstring(g_hits.size()));
      SendMessageW(g_hwnd, WM_LBUTTONDOWN, MK_LBUTTON, at);
      SendMessageW(g_hwnd, WM_LBUTTONUP, 0, at);
      if (!g_hwnd) return;
    } else if (op == L"slide") {  // slide:<row>:<0..1> - the knob of a slider put there
      const size_t c2 = arg.find(L':');
      const int id = hit_by_name(L"g." + arg.substr(0, c2));
      if (is_slider(id) && c2 != std::wstring::npos) slide_to(static_cast<gcfg::Row>((id - H_Row) / kRowParts), static_cast<float>(_wtof(arg.c_str() + c2 + 1)));
      else logf(L"slide " + arg + L": NOT A SLIDER");
    } else if (op == L"type") {
      for (wchar_t ch : arg) SendMessageW(g_hwnd, WM_CHAR, ch, 0);
    } else if (op == L"key") {  // key:enter, key:esc, key:tab, key:back
      const WPARAM vk = arg == L"enter" ? VK_RETURN : arg == L"esc" ? VK_ESCAPE : arg == L"tab" ? VK_TAB : 0;
      if (arg == L"back") SendMessageW(g_hwnd, WM_CHAR, 8, 0);
      else if (vk) SendMessageW(g_hwnd, WM_KEYDOWN, vk, 1);
      if (!g_hwnd) return;
    } else if (op == L"keydown") {  // keydown:<virtual key>:<scan code>:<extended 0 | 1>, as the keyboard sends it
      std::vector<std::wstring> f = split(arg, L':');
      if (f.size() == 3) {
        LPARAM lp = 1 | (static_cast<LPARAM>(wcstol(f[1].c_str(), nullptr, 0) & 0xFF) << 16) | (static_cast<LPARAM>(_wtoi(f[2].c_str()) & 1) << 24);
        SendMessageW(g_hwnd, WM_KEYDOWN, static_cast<WPARAM>(wcstol(f[0].c_str(), nullptr, 0)), lp);
      }
    } else if (op == L"rdown") {
      SendMessageW(g_hwnd, WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(4, 60));
    } else if (op == L"mdown") {
      SendMessageW(g_hwnd, WM_MBUTTONDOWN, MK_MBUTTON, MAKELPARAM(4, 60));
    } else if (op == L"xdown") {
      SendMessageW(g_hwnd, WM_XBUTTONDOWN, MAKEWPARAM(0, _wtoi(arg.c_str()) == 2 ? XBUTTON2 : XBUTTON1), MAKELPARAM(4, 60));
    } else if (op == L"state") {
      log_state();
    } else if (op == L"cfg") {
      log_cfg();
    } else if (op == L"file") {
      log_file_line(arg);
    } else if (op == L"shot") {
      logf(L"shot " + arg + (save_shot(arg, 1) ? L"" : L": FAILED"));
    } else if (op == L"quit") {
      logf(L"quit");
      DestroyWindow(g_hwnd);
      return;
    }
  }
}

void tick(HWND hwnd) {
  ULONGLONG now = GetTickCount64();
  if (!g.flash.empty() && now > g.flash_until) g.flash.clear(), dirty();
  if (g.confirm_uninstall && now > g_confirm_until) g.confirm_uninstall = false, dirty();
  if (g.launching && now > g_launch_until) g.launching = false, dirty();
  if (g_quit_at && now > g_quit_at) {
    DestroyWindow(hwnd);
    return;
  }
  if (now > g_steam_at) {  // Steam may be started or closed while the launcher is open
    g_steam_at = now + 2000;
    const bool steam = core::steam_running();
    if (steam != g.steam) {
      g.steam = steam;
      logf(steam ? L"steam: running" : L"steam: not running");
      dirty();
    }
  }
  if (g_drop_old_until && (core::drop_old_self() || now > g_drop_old_until)) g_drop_old_until = 0;
  if (g_drive) drive();
  if (g_hwnd && (moving() || g_dirty) && !IsIconic(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
}

// A letter for the field that is typed into.
void type_char(HWND hwnd, wchar_t ch) {
  std::wstring& text = g.focus == F_Proxy ? g.set.proxy_addr : g.edit;
  const size_t max = g.focus == F_Name ? mset::kNameMax : 64;
  auto fits = [&](wchar_t c) {
    return g.focus == F_Name ? mset::name_char(c) : g.focus == F_Address ? mset::address_char(c) : (c > 0x20 && c < 0x7F);
  };
  if (ch == 8) {
    if (!text.empty()) text.pop_back();
  } else if (ch == 22) {  // Ctrl+V
    if (OpenClipboard(hwnd)) {
      if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* t = static_cast<const wchar_t*>(GlobalLock(h))) {
          for (; *t && text.size() < max; ++t)
            if (fits(*t)) text += *t;
          GlobalUnlock(h);
        }
      }
      CloseClipboard();
    }
  } else if (fits(ch) && text.size() < max) {
    text += ch;
  }
  dirty();
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(hwnd, &ps);
      paint(hwnd);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      if (g_rt) g_rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
      return 0;
    case WM_DPICHANGED: {
      const RECT* r = reinterpret_cast<const RECT*>(lp);
      discard_target();
      SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_TIMER:
      tick(hwnd);
      return 0;
    case WM_NCHITTEST: {
      POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ScreenToClient(hwnd, &p);
      float s = scale_of(hwnd);
      if (p.y / s < 52 && p.x / s < 870) return HTCAPTION;  // the top of the window drags it
      return HTCLIENT;
    }
    case WM_NCLBUTTONDBLCLK:
      return 0;  // (no maximizing)
    case WM_SYSCOMMAND:
      if ((wp & 0xFFF0) == SC_MAXIMIZE) return 0;
      if ((wp & 0xFFF0) == SC_KEYMENU) return 0;  // (Alt opens no menu: it is a key a row of УПРАВЛЕНИЕ may take)
      break;
    case WM_MOUSEMOVE: {
      float s = scale_of(hwnd);
      const float mx = GET_X_LPARAM(lp) / s, my = GET_Y_LPARAM(lp) / s;
      if (is_slider(g.pressed) && (wp & MK_LBUTTON)) {  // the knob follows the pointer
        slide_at(g.pressed, mx, my);
        return 0;
      }
      int id = hit_at(mx, my);
      if (id != g.hover) {
        g.hover = id;
        dirty();
      }
      if (!g_tracking) {
        TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
        g_tracking = TrackMouseEvent(&t) != FALSE;
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      g_tracking = false;
      if (g.hover) g.hover = 0, dirty();
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) {
        const bool field = g.hover == H_ProxyField || g.hover == H_NameField || g.hover == H_AddressField;
        SetCursor(LoadCursorW(nullptr, field ? IDC_IBEAM : g.hover ? IDC_HAND : IDC_ARROW));
        return TRUE;
      }
      break;
    case WM_LBUTTONDOWN: {
      float s = scale_of(hwnd);
      const float mx = GET_X_LPARAM(lp) / s, my = GET_Y_LPARAM(lp) / s;
      g.pressed = hit_at(mx, my);
      if (g.focus && !g.pressed) blur();  // (a press beside the field: what was typed is written)
      if (g.capture >= 0 && !g.pressed) g.capture = -1;
      if (!g_drive) SetCapture(hwnd);  // (a script's press takes nothing of the real mouse)
      if (is_slider(g.pressed)) slide_at(g.pressed, mx, my);
      dirty();
      return 0;
    }
    case WM_LBUTTONUP: {
      float s = scale_of(hwnd);
      int id = hit_at(GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s), was = g.pressed;
      g.pressed = 0;
      ReleaseCapture();
      dirty();
      if (was && id == was) act(id);
      return 0;
    }
    case WM_KILLFOCUS:
      if (g.focus) blur();
      if (g.capture >= 0) g.capture = -1, dirty();
      break;
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
      if (g.capture < 0) break;  // a row waits for a key: a button of the mouse is one
      bind_key(gcfg::token_of_mouse(msg == WM_RBUTTONDOWN ? 1 : msg == WM_MBUTTONDOWN ? 2 : HIWORD(wp) == XBUTTON1 ? 3 : 4));
      return msg == WM_XBUTTONDOWN ? TRUE : 0;
    case WM_SYSKEYDOWN:
      if (g.capture < 0) break;  // (Alt and F10 are keys like any other for a row that waits)
      [[fallthrough]];
    case WM_KEYDOWN:
      if (g.capture >= 0) {
        if (wp == VK_ESCAPE) {
          g.capture = -1;
          dirty();
        } else if (!(lp & (1 << 30))) {  // (not a held key's repeat)
          bind_key(gcfg::token_of_key(dik_of(wp, lp)));
        }
        return 0;
      }
      if (g.focus) {
        if (wp == VK_RETURN || wp == VK_TAB) blur();
        else if (wp == VK_ESCAPE) blur(false);
        return 0;
      }
      if (wp == VK_ESCAPE && g.tab != Tab::Game) act(H_Tab0);
      else if (wp == VK_RETURN && g.tab == Tab::Game && g.screen != Screen::Downloading) act(H_Primary);
      return 0;
    case WM_CHAR:
      if (g.focus) type_char(hwnd, static_cast<wchar_t>(wp));
      return 0;
    case WM_APP + 1:
      on_check(static_cast<int>(wp), reinterpret_cast<core::CheckResult*>(lp));
      return 0;
    case WM_APP + 2:
      g.stage = static_cast<int>(wp);
      if (g.screen == Screen::Running && g.stage == 2) set_screen(Screen::Downloading);
      logf(L"stage " + std::to_wstring(g.stage));
      dirty();
      return 0;
    case WM_APP + 3: {
      uint64_t got = static_cast<uint64_t>(wp), total = static_cast<uint64_t>(lp);
      if (g_drive && total && got * 4 / total != g.got * 4 / total) logf(L"downloaded " + std::to_wstring(got * 100 / total) + L"%");
      g.got = got;
      g.total = total;
    }
      dirty();
      return 0;
    case WM_APP + 4:
      if (g_job) set_screen(wp ? Screen::Running : Screen::Downloading);
      return 0;
    case WM_APP + 5:
      on_done(reinterpret_cast<Done*>(lp));
      return 0;
    case WM_DESTROY:
      g_cancel = true;
      discard_target();
      g_hwnd = nullptr;
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

// The mod put into a game folder or taken out of it with no window of the launcher: for scripts, and for
// this launcher asking Windows for administrator rights. quiet: the report goes to the console the
// launcher was started from, if there is one; else it is shown in a box.
int headless(int verb, const std::wstring& dir, bool quiet, const std::wstring& package) {
  std::wstring log;
  pkg::Result r = pkg::Result::Failed;
  if (dir.empty()) {
    log = L"не указана папка игры\n";
  } else if (verb == 1) {
    pkg::Package p;
    if (p.open(package)) {
      r = pkg::install(p, dir, log);
      if (r == pkg::Result::Ok) log = L"Мод metronet " + p.version + L" установлен в:\n" + dir + L"\n\n" + log;
    } else {
      log = package.empty() ? L"в этом лаунчере нет мода\n" : L"в файле " + package + L" нет мода\n";
    }
  } else {
    r = pkg::uninstall(dir, log);
    if (r == pkg::Result::Ok) log = L"Мод metronet удалён из:\n" + dir + L"\n\n" + log;
  }
  if (r != pkg::Result::Ok) log = L"Не получилось.\n\n" + log;
  if (!quiet) {
    MessageBoxW(nullptr, log.c_str(), L"metronet", MB_OK | (r == pkg::Result::Ok ? MB_ICONINFORMATION : MB_ICONERROR));
  } else if (AttachConsole(ATTACH_PARENT_PROCESS)) {
    HANDLE out = GetStdHandle(r == pkg::Result::Ok ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
    DWORD n = 0;
    std::wstring text = L"\n" + log;
    if (out && out != INVALID_HANDLE_VALUE) WriteConsoleW(out, text.c_str(), static_cast<DWORD>(text.size()), &n, nullptr);
  }
  if (!g_out.empty()) logf(log);
  return r == pkg::Result::Ok ? 0 : 2;
}

int run(int argc, wchar_t** argv) {
  std::wstring shot_screen, shot_file, shot_hover, shot_press;
  float shot_scale = 1;
  bool check_only = false, where_only = false, quiet = false;
  int verb = 0;  // 1 /install, 2 /uninstall
  std::wstring verb_dir, package;
  for (int i = 1; i < argc; ++i) {
    std::wstring a = argv[i];
    auto next = [&]() { return i + 1 < argc ? std::wstring(argv[++i]) : std::wstring(); };
    if (a == L"--shot") {
      shot_screen = next();
      shot_file = next();
    } else if (a == L"--scale") {
      shot_scale = static_cast<float>(_wtof(next().c_str()));
    } else if (a == L"/install" || a == L"/uninstall") {
      verb = a == L"/install" ? 1 : 2;
    } else if (a == L"/quiet") {
      quiet = true;
    } else if (a == L"/package") {
      package = next();
    } else if (verb && verb_dir.empty() && !a.empty() && a[0] != L'-' && a[0] != L'/') {
      verb_dir = a;
    } else if (a == L"--check") {
      check_only = true;
    } else if (a == L"--where") {
      where_only = true;
    } else if (a == L"--out") {
      g_out = next();
    } else if (a == L"--settings") {
      core::g_settings_path = next();
    } else if (a == L"--cache") {
      core::g_cache_dir = next();
    } else if (a == L"--repo") {
      core::g_repo_url = next();
    } else if (a == L"--steam") {
      core::g_steam = _wtoi(next().c_str()) ? 1 : 0;
    } else if (a == L"--game-running") {
      core::g_game = _wtoi(next().c_str()) ? 1 : 0;
    } else if (a == L"--userdir") {
      gcfg::g_user_dir = next();
    } else if (a == L"--hover") {
      shot_hover = next();
    } else if (a == L"--press") {
      shot_press = next();
    } else if (a == L"--pick") {
      g_pick = next();
    } else if (a == L"--drive") {
      g_drive = true;
      g_script = split(next(), L';');
    } else if (a == L"--drive-next") {
      g_drive_next = next();
    } else if (a == L"--after-update") {
      g_after_update = next();
    }
  }
  g_start = GetTickCount64();
  if (g_drive || !shot_file.empty()) core::g_scripted = true;  // (nothing of the player's own is read, no prompt is raised)
  // (a script that names no repository gets one that is not there: the real GitHub is never asked by accident)
  if (g_drive && core::g_repo_url.empty()) core::g_repo_url = L"http://127.0.0.1:9/no-repository";
  if (verb) return headless(verb, core::trimmed(verb_dir), quiet, package);
  if (!gfx::init()) {
    if (g_drive || !shot_file.empty()) logf(L"Direct2D could not be started");
    else MessageBoxW(nullptr, L"Не удалось включить Direct2D: лаунчеру нужна Windows 10 или новее.", L"metronet", MB_ICONERROR);
    return 3;
  }

  if (!shot_file.empty()) {  // a page of the design as a picture
    if (!view::demo(shot_screen, g)) return 2;
    if (core::g_steam >= 0) g.steam = core::g_steam != 0;
    g.hover = hit_by_name(shot_hover);  // (the pointer over something, or pressing it: how that looks)
    g.pressed = hit_by_name(shot_press);
    bool ok = gfx::save_png(shot_file.c_str(), kW, kH, shot_scale <= 0 ? 1 : shot_scale,
                            [](gfx::Canvas& c) { view::draw(c, g, nullptr); });
    if (!ok) {
      wchar_t why[80];
      swprintf(why, 80, L"shot failed at step %d, hr %08lX", gfx::g_last_step, static_cast<unsigned long>(gfx::g_last_hr));
      logf(why);
    }
    gfx::shutdown();
    return ok ? 0 : 1;
  }

  if (where_only) {  // where the game would be found from here, and what is in it
    std::wstring trace, dir = core::find_game_dir(&trace);
    Installed in = core::read_installed(dir);
    for (size_t p = 0; p < trace.size();) {
      const size_t e = trace.find(L'\n', p);
      logf(L"looked: " + trace.substr(p, e - p));
      p = e == std::wstring::npos ? trace.size() : e + 1;
    }
    logf(L"game: " + (dir.empty() ? std::wstring(L"(not found)") : dir));
    logf(std::wstring(L"mod: ") + (in.present ? (in.version.empty() ? L"of an unknown version" : in.version.c_str()) : L"not installed") +
         (dir.empty() ? L"" : core::exe_supported(dir) ? L", the exe is the supported build" : L", ANOTHER build of the exe"));
    gfx::shutdown();
    return dir.empty() ? 1 : 0;
  }
  core::load_settings(g.set);
  if (check_only) {
    std::atomic<bool> never{false};
    core::CheckResult r = core::check(g.set, never);
    logf(std::wstring(L"known ") + (r.rel.known ? L"1" : L"0") + L" full " + (r.rel.full ? L"1" : L"0") + L" blocked " +
         (r.assets_blocked ? L"1" : L"0") + L" norelease " + (r.no_release ? L"1" : L"0") + L" version " + r.rel.version + L" protocol " +
         std::to_wstring(r.rel.protocol) + L" route " + r.route.label);
    logf(L"date " + r.rel.date + L" title " + r.rel.title + L" notes " + std::to_wstring(r.rel.notes.size()) + L" file " +
         r.rel.pkg_name + L" " + std::to_wstring(r.rel.pkg_size) + L" " + core::wide(r.rel.pkg_sha256) + L" launcher " +
         r.rel.launcher_version);
    logf(L"inside this launcher: mod " + core::bundled().version + L", protocol " + std::to_wstring(core::bundled().protocol));
    logf(r.log);
    gfx::shutdown();
    return r.rel.known ? 0 : r.no_release ? 4 : 1;
  }

  // One launcher at a time: a second start shows the first one.
  g_once = g_drive ? nullptr : CreateMutexW(nullptr, TRUE, kOnce);
  if (g_once && GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND other = FindWindowW(kClass, nullptr)) {
      ShowWindow(other, SW_RESTORE);
      SetForegroundWindow(other);
    }
    return 0;
  }

  HINSTANCE inst = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_DROPSHADOW;
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = inst;
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
  wc.hIconSm = wc.hIcon;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = kClass;
  RegisterClassExW(&wc);

  // In the middle of the screen the pointer is on, at that screen's scale.
  POINT cursor{};
  GetCursorPos(&cursor);
  HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  GetMonitorInfoW(mon, &mi);
  UINT dpi = 96;
  if (HDC dc = GetDC(nullptr)) {
    dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
  }
  int w = MulDiv(kW, dpi, 96), h = MulDiv(kH, dpi, 96);
  int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2, y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
  g_hwnd = CreateWindowExW(0, kClass, L"Metro 2033 Redux — совместная игра", WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX, x, y, w, h, nullptr,
                           nullptr, inst, nullptr);
  if (!g_hwnd) return 3;
  UINT real = GetDpiForWindow(g_hwnd);  // (the screen it landed on may have another scale)
  if (real != dpi) SetWindowPos(g_hwnd, nullptr, 0, 0, MulDiv(kW, real, 96), MulDiv(kH, real, 96), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  int round = 2;  // DWMWCP_ROUND: round corners where Windows has them (11)
  DwmSetWindowAttribute(g_hwnd, 33, &round, sizeof(round));

  // Where things stand at the start.
  if (!core::has_game(g.set.game_dir) && !g_drive) {  // (a test never goes looking for the real game)
    std::wstring found = core::trimmed(core::find_game_dir());
    if (!found.empty()) {
      g.set.game_dir = found;
      core::save_settings(g.set);
    }
  }
  g.pack = core::bundled();
  g.repo = core::has_repo();
  g.steam = core::steam_running();
  g_steam_at = GetTickCount64() + 2000;
  // What an update of the launcher itself left behind. The launcher this one has just replaced may still be
  // closing: its file is removed as soon as it lets go of it.
  if (!core::drop_old_self()) g_drop_old_until = GetTickCount64() + 15000;
  if (!g_after_update.empty()) {
    logf(L"started by launcher " + g_after_update + L" in its place");
    flash(L"Лаунчер обновлён: " + g_after_update + L" → " + kLauncherVersion, 7000);
  }
  refresh_installed();
  logf(std::wstring(L"start: launcher ") + kLauncherVersion + L", game " + (g.has_game ? (g.exe_ok ? L"found" : L"found, ANOTHER build") : L"not found") +
       L", mod " + (g.inst.present ? (g.inst.version.empty() ? L"of an unknown version" : g.inst.version) : L"not installed") +
       L"; inside the launcher: " + (g.pack.known ? g.pack.version : L"NOTHING") + L"; steam " + (g.steam ? L"running" : L"not running") +
       L"; user.cfg " + (gcfg::has_file() ? L"read" : L"not there"));
  if (!g.has_game) {
    g.screen = Screen::NoGame;
    start_check(true);  // (the newest version is known by the time a folder is picked)
  } else if (!g.exe_ok) {
    g.screen = Screen::WrongBuild;
    start_check(true);
  } else if (!g.inst.present) {
    g.screen = Screen::Install;  // (the mod is inside: it can go in at once; GitHub may still name a newer one)
    g_startup_check = true;
    start_check(true);
  } else if (g.set.auto_check && core::has_repo()) {
    g.screen = Screen::Checking;
    g_startup_check = true;
    start_check(false);
  } else {
    g.unchecked = true;
    g.screen = decide();
  }

  SetTimer(g_hwnd, 1, 33, nullptr);
  if (!g_drive) {
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
  }
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  logf(L"exit");
  gfx::shutdown();
  if (g_once) CloseHandle(g_once);
  return 0;
}

}  // namespace
}  // namespace lz

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  int rc = lz::run(argc, argv);
  LocalFree(argv);
  CoUninitialize();
  return rc;
}
