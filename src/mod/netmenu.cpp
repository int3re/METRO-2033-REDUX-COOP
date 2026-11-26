#include "mod/netmenu.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "common/log.h"
#include "mod/engine.h"
#include "mod/game.h"
#include "mod/hud.h"
#include "mod/menu.h"
#include "mod/patch.h"
#include "mod/session.h"
#include "mod/steamnet.h"

namespace netmenu {
namespace {

constexpr uintptr_t kResolve = 0x707770;       // the game's text: (Text*, out*, int) -> the words
constexpr uintptr_t kBoardRequest = 0x6B9540;  // the leaderboard service's request: (service*, int mode, ?, delegate*)
constexpr uintptr_t kBoardRows = 0x664CA0;     // its answer: (Results*, Text* message) fills the page's list
constexpr uintptr_t kBoardStats = 0x6649E0;    // menu handler "leaderboard_stats" (str** "my" on activation / "Y"; else Enter)
constexpr uintptr_t kBoardActivate = 0x661B60;  // handler "leaderboard_on_activate": the list anew, as the page opens
constexpr uintptr_t kMenuManager = 0xD01E88;
constexpr uintptr_t kMessageTable = 0x6A3720;  // () -> the table of the menus' names
constexpr size_t kListSelected = 0x168, kListRows = 0xC0, kListCount = 0xCA;  // the page's list box

// Where the label stands: over the gramophone at the left front of the desk (world dump of 2033\000: mafon_0002).
constexpr float kDeskX = -1.44f, kDeskY = 1.07f, kDeskZ = -6.35f;

struct Text {
  uint8_t* string;
  uint32_t kind;
  uint32_t found;
};
struct Row {
  uint8_t* name;
  int32_t a, b;
};
struct Results {
  Row* data;
  uint16_t cap, count;
  uint32_t pad;
};

Config g_cfg;
std::atomic<bool> g_page{false};  // the leaderboard page is open as the network page
std::atomic<bool> g_fill{false};  // its list is to be filled at the next tick
std::atomic<uint64_t> g_key_at{0};
uint64_t g_filled_at = 0;
bool g_key_was = false, g_mouse_was = false;
std::mutex g_lock;
std::vector<steamnet::Offer> g_listed;  // the rows on the page now
std::vector<uint8_t*> g_row_names;      // their counted strings (the page holds them; kept)
std::map<std::string, uint8_t*> g_counted;

uint8_t* counted(const std::string& text) {  // made once per text, never let go (the page keeps pointers)
  auto it = g_counted.find(text);
  if (it != g_counted.end()) return it->second;
  uint8_t* c = hud::counted_text(text);
  if (c) g_counted[text] = c;
  return c;
}

std::string text_of(const uint8_t* s) {
  if (!s) return "";
  uint16_t n = *reinterpret_cast<const uint16_t*>(s + 12);
  return std::string(reinterpret_cast<const char*>(s) + 20, n);
}

bool ru() { return hud::cyrillic(); }

struct Swap {
  const char* key;
  const char* ru;
  const char* en;
};
const Swap kWords[] = {
    {"leaderboard", "СЕТЕВАЯ ИГРА", "NETWORK GAME"},
    {"tower_mode", "ДРУЗЬЯ В STEAM, КОТОРЫЕ ИГРАЮТ СЕЙЧАС", "STEAM FRIENDS PLAYING NOW"},
    {"top_players", "Войти", "Join"},
    {"my_stats", "Обновить", "Refresh"},
};
// The table's head: the game's words (as the fonts take them) -> the page's.
const Swap kColumns[] = {
    {"СЕТЕВОЙ ID", "ИГРОК И ГЛАВА", "PLAYER AND CHAPTER"}, {"СЧЕТ", " ", " "}, {"ВРЕМЯ", " ", " "},
    {"NETWORK ID", "ИГРОК И ГЛАВА", "PLAYER AND CHAPTER"}, {"SCORE", " ", " "},  {"TIME", " ", " "},
};

const char* chapter(const std::string& level) {
  static const struct {
    const char* level;
    const char* name;
  } kChapters[] = {
      {"l00_intro", "Пролог"},          {"l01_hunter", "Охотник"},       {"l02_exhibition", "Выставка"},
      {"l03_chase", "Погоня"},          {"l04_riga", "Рижская"},         {"l05_lost_tunnel", "Заброшенный туннель"},
      {"l08_market", "Рынок"},          {"l09_dead_city_1", "Мёртвый город"}, {"l11_dry", "Сухаревская"},
      {"l12_ghosts", "Призраки"},       {"l14_cursed", "Проклятая"},     {"l15_armory", "Арсенал"},
      {"l16_frontline", "Передовая"},   {"l17_trolley_combat", "Бой на дрезинах"}, {"l18_depot", "Депо"},
      {"l19_defence", "Оборона"},       {"l21_nazi_outpost", "Аванпост"}, {"l22_black", "Чёрная станция"},
      {"l23_polis", "Полис"},           {"l24_alley", "Аллея"},          {"l26_depository", "Хранилище"},
      {"l27_archives", "Архив"},        {"l28_driving", "Поездка"},      {"l30_darkstar", "Тёмная звезда"},
      {"l32_cave", "Пещера"},           {"l33_d6", "Д6"},                {"l36_ostankino", "Останкино"},
      {"final", "Финал"},
  };
  for (const auto& c : kChapters)
    if (level.size() >= strlen(c.level) && level.compare(level.size() - strlen(c.level), std::string::npos, c.level) == 0) return c.name;
  return nullptr;
}

// --- the page's words ------------------------------------------------------------------------------------------
using ResolveFn = int64_t (*)(uint64_t*, void*, int);
ResolveFn g_resolve = nullptr;

int64_t on_resolve(uint64_t* text, void* out, int which) {
  if (!g_page.load() || !text || !*text) return g_resolve(text, out, which);
  const uint8_t* s = reinterpret_cast<const uint8_t*>(*text);
  uint32_t kind = static_cast<uint32_t>(text[1] & 7);
  const char* put = nullptr;
  bool row = false;
  {
    std::lock_guard<std::mutex> hold(g_lock);
    std::string key = text_of(s);
    if (kind <= 1)
      for (const Swap& w : kWords)
        if (key == w.key) put = ru() ? w.ru : w.en;
    if (!put && kind == 6)
      for (const Swap& c : kColumns) {
        uint8_t* game_words = counted(c.key);
        if (game_words && key == text_of(game_words)) put = ru() ? c.ru : c.en;
      }
    for (uint8_t* n : g_row_names)
      if (n == s) row = true;
  }
  if (!put && !row) return g_resolve(text, out, which);
  uint64_t was0 = text[0], was1 = text[1];
  if (put) {
    std::lock_guard<std::mutex> hold(g_lock);
    uint8_t* mine = counted(put);
    if (mine) text[0] = reinterpret_cast<uint64_t>(mine);
  }
  text[1] = (text[1] & ~7ull) | 6;  // the words as they are (already in the fonts' codes)
  int64_t r = g_resolve(text, out, which);
  text[0] = was0, text[1] = was1;
  return r;
}

// --- the page's rows --------------------------------------------------------------------------------------------
// The answer's list is the service's: the game lets it go at the end of exe+664CA0 (exe+17300: each name's hold
// dropped, the buffer given back to the game's heap). The mod's own list (its buffer is the mod's) is left alone.
using DropListFn = void (*)(void*);
DropListFn g_drop_list = nullptr;
std::atomic<void*> g_own_list{nullptr};

void on_drop_list(void* list) {
  if (list && list == g_own_list.load()) {
    *reinterpret_cast<void**>(list) = nullptr;
    *reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(list) + 10) = 0;
  }
  g_drop_list(list);
}

using RequestFn = int64_t (*)(void*, int64_t, int64_t, void*);
RequestFn g_request = nullptr;

int64_t on_request(void* service, int64_t mode, int64_t x, void* delegate) {
  if (g_page.load()) {
    g_fill.store(true);  // (the answer is given at the next tick, as a service's answer comes)
    return 0;
  }
  return g_request(service, mode, x, delegate);
}

uint8_t* list_box();

void fill() {
  std::vector<steamnet::Offer> games = steamnet::games();
  std::vector<Row> rows;
  {
    std::lock_guard<std::mutex> hold(g_lock);
    g_listed = games;
    g_row_names.clear();
    for (const steamnet::Offer& g : games) {
      // one text a row, its columns parted by "|" (the page's list draws "N|name|score|time" so)
      const char* ch = chapter(g.level);
      std::string line = std::to_string(rows.size() + 1) + "|" + (g.name.empty() ? std::string(ru() ? "Друг" : "A friend") : g.name) +
                         "  —  " + (ch && ru() ? std::string(ch) : g.level.substr(g.level.find_last_of('\\') + 1)) + "||";
      uint8_t* n = counted(line);
      g_row_names.push_back(n);
      rows.push_back({n, 0, 0});
    }
  }
  Results res = {rows.data(), static_cast<uint16_t>(rows.size()), static_cast<uint16_t>(rows.size()), 0};
  Text message = {nullptr, 6, 0};
  if (rows.empty()) {
    std::lock_guard<std::mutex> hold(g_lock);
    message.string = counted(ru() ? "Сейчас никто из ваших друзей в Steam не играет в Metro 2033 Redux с этим модом. Начните игру сами - "
                                    "друзья увидят её здесь и смогут войти."
                                  : "None of your Steam friends plays Metro 2033 Redux with this mod now. Start a game yourself - your "
                                    "friends will see it here and can come in.");
  }
  g_own_list.store(&res);
  reinterpret_cast<void (*)(Results*, Text*)>(game::at(kBoardRows))(&res, &message);
  g_own_list.store(nullptr);
  g_filled_at = GetTickCount64();
  LOGI("netmenu: the network page lists %zu friends' games", rows.size());
}

// The page's list box: the game's own way to it (exe+6649E0): the page by its name in the menus' table, its item
// whose name is the list's, that item's list (vtbl + 0x2A8).
uint8_t* list_box() {
  uint8_t* mgr = game::ref<uint8_t*>(kMenuManager);
  if (!mgr) return nullptr;
  uint8_t* table = reinterpret_cast<uint8_t* (*)()>(game::at(kMessageTable))();
  using PageFn = uint8_t* (*)(uint8_t*, void*);
  uint8_t* page = (*reinterpret_cast<PageFn**>(mgr))[96 / 8](mgr, table + 80);
  if (!page) return nullptr;
  uint8_t** items = *reinterpret_cast<uint8_t***>(page + 440);
  uint16_t n = *reinterpret_cast<uint16_t*>(page + 450);
  void* want = *reinterpret_cast<void**>(table + 280);
  for (uint16_t i = 0; i < n && items; ++i) {
    uint8_t* item = items[i];
    if (item && *reinterpret_cast<void**>(item + 8) == want)
      return reinterpret_cast<uint8_t* (*)(uint8_t*)>((*reinterpret_cast<void***>(item))[0x2A8 / 8])(item);
  }
  return nullptr;
}

void join_chosen() {
  uint8_t* box = list_box();
  int chosen = -1;
  uint8_t* name = nullptr;
  if (box) {
    int sel = *reinterpret_cast<int*>(box + kListSelected);
    uint16_t count = *reinterpret_cast<uint16_t*>(box + kListCount);
    uint8_t** rows = *reinterpret_cast<uint8_t***>(box + kListRows);
    if (rows && sel >= 0 && sel < count && rows[sel]) name = *reinterpret_cast<uint8_t**>(rows[sel]);
    LOGI("netmenu: Enter on the network page: row %d of %u", sel, count);
  }
  uint64_t lobby = 0;
  std::string who;
  {
    std::lock_guard<std::mutex> hold(g_lock);
    for (size_t i = 0; i < g_row_names.size() && i < g_listed.size(); ++i)  // (the first ones are the names, in order)
      if (name && g_row_names[i] == name) chosen = static_cast<int>(i);
    if (chosen < 0 && g_listed.size() == 1) chosen = 0;
    if (chosen >= 0) lobby = g_listed[chosen].lobby, who = g_listed[chosen].name;
  }
  if (!lobby) {
    LOGI("netmenu: no game chosen on the network page");
    return;
  }
  LOGI("netmenu: going into %s's game", who.c_str());
  hud::notice((who.empty() ? std::string("Друг") : who) + ": вход в игру...", "Joining " + (who.empty() ? std::string("a friend") : who) + "'s game...", 6);
  session::join_game(lobby);
}

// --- the page's keys --------------------------------------------------------------------------------------------
using StatsFn = int64_t (*)(void*, uint8_t**);
StatsFn g_stats = nullptr;

int64_t on_stats(void* a1, uint8_t** param) {
  if (!g_page.load()) return g_stats(a1, param);
  std::string p = param && *param ? text_of(*param) : "";
  if (p == "my") return g_stats(a1, param);  // the page opens, or "Обновить": the list anew
  join_chosen();                             // Enter: "Войти"
  return 0;
}

void on_log(const char* line) {
  if (strstr(line, "Deactivate menu [leaderboard]")) {
    if (g_page.exchange(false)) LOGI("netmenu: the network page is closed");
  }
}

void open_page() {
  if (g_page.load()) return;
  g_page.store(true);
  LOGI("netmenu: the network page opens");
  engine::command("mainmenu leaderboard", true);
}

}  // namespace

void key_seen() { g_key_at.store(GetTickCount64()); }

void install(const Config& cfg) {
  g_cfg = cfg;
  if (cfg.net_mode == Config::NetMode::Off || !cfg.net_menu) return;
  patch::hook("menu: the network page's words", game::at(kResolve), reinterpret_cast<void*>(&on_resolve), reinterpret_cast<void**>(&g_resolve));
  patch::hook("menu: the network page's rows", game::at(kBoardRequest), reinterpret_cast<void*>(&on_request), reinterpret_cast<void**>(&g_request));
  patch::hook("menu: the network page's keys", game::at(kBoardStats), reinterpret_cast<void*>(&on_stats), reinterpret_cast<void**>(&g_stats));
  patch::hook("menu: the network page's list let go", game::at(0x17300), reinterpret_cast<void*>(&on_drop_list), reinterpret_cast<void**>(&g_drop_list));
  engine::on_log(&on_log);
}

void tick() {
  if (g_cfg.net_mode == Config::NetMode::Off || !g_cfg.net_menu || !g_resolve) return;
  if (g_fill.exchange(false)) fill();
  // the open page follows Steam: a friend who starts or ends a game appears or goes within a few seconds
  if (g_page.load() && GetTickCount64() - g_filled_at > 4000) {
    std::vector<steamnet::Offer> now_games = steamnet::games();
    bool changed;
    {
      std::lock_guard<std::mutex> hold(g_lock);
      changed = now_games.size() != g_listed.size();
      for (size_t i = 0; !changed && i < now_games.size(); ++i)
        changed = now_games[i].lobby != g_listed[i].lobby || now_games[i].level != g_listed[i].level;
    }
    g_filled_at = GetTickCount64();
    if (changed) reinterpret_cast<void (*)()>(game::at(kBoardActivate))();  // (the game's own way: list cleared, asked anew)
  }
  bool desk = session::on_main_menu() && !menu::page_open();
  if (!desk) {
    hud::desk_label(false, 0, 0, 0, "", "", "", "", false);
    g_key_was = g_mouse_was = false;
    return;
  }
  // the pointer: over the label (the label is lit), its left button opens the page
  bool hot = false, click = false;
  float cx, cy, w, h;
  HWND window = GetForegroundWindow();
  DWORD pid = 0;
  if (window) GetWindowThreadProcessId(window, &pid);
  if (!g_cfg.lab && pid == GetCurrentProcessId() && hud::desk_label_rect(&cx, &cy, &w, &h)) {
    POINT p;
    RECT r;
    if (GetCursorPos(&p) && ScreenToClient(window, &p) && GetClientRect(window, &r) && r.right > 0 && r.bottom > 0) {
      float mx = 2.f * p.x / r.right - 1.f, my = 2.f * p.y / r.bottom - 1.f;
      hot = mx > cx - w * 0.5f && mx < cx + w * 0.5f && my > cy - h * 0.5f && my < cy + h * 0.5f;
      bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
      click = hot && down && !g_mouse_was;
      g_mouse_was = down;
    }
  }
  hud::desk_label(true, kDeskX, kDeskY, kDeskZ, "СЕТЕВАЯ ИГРА", "NETWORK GAME", "N  /  Y", "N  /  Y", hot);
  uint64_t now = GetTickCount64();
  bool key = now - g_key_at.load() < 150 ||
             (!g_cfg.lab && pid == GetCurrentProcessId() && (GetAsyncKeyState('N') & 0x8000) != 0);
  if ((key && !g_key_was) || click) open_page();
  g_key_was = key;
}

void lab_open() {
  engine::on_main([] { open_page(); });
}

void lab_enter() {
  engine::on_main([] {
    if (g_page.load()) join_chosen();
  });
}

}  // namespace netmenu
