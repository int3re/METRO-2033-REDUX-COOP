#include "mod/menu.h"

#include <windows.h>

#include <atomic>
#include <cstring>

#include "common/log.h"
#include "mod/engine.h"
#include "mod/game.h"
#include "mod/hud.h"

namespace menu {
namespace {

// The game asks "Return to the main menu?" like this (exe+666D08 .. 666D8E, the handler "main_menu_handler"):
//   mgr = [exe+D01E88], made by exe+168E0() when there is none yet
//   mgr->vtbl[0x68](mgr, Text* message, Delegate* on_ok, str* parameter, int 3, char 0, void* 0)
// Text is what the game's localisation takes (exe+707770): {counted string*, u32 kind, u32 found}; kind 0 = a key
// of the language table, kind 6 = the string as it is, already in the fonts' codes (hud::counted_text makes one).
// Delegate = {function, exe+C2C0}: on "OK" the handler "confirmation_ok_handler" (exe+667BC0) closes the window
// and calls function(0, &parameter). "Back" only closes it.
constexpr uintptr_t kMenuManager = 0xD01E88;
constexpr uintptr_t kMakeMenuManager = 0x168E0;
constexpr uintptr_t kDelegateThunk = 0xC2C0;
constexpr uintptr_t kEmptyString = 0xD03FC0;

struct Text {
  uint8_t* string;
  uint32_t kind;
  uint32_t found;
};
struct Delegate {
  void* function;
  void* thunk;
};

void (*g_yes)() = nullptr;
std::atomic<int> g_open{0};

void on_ok(void*, void*) {
  void (*yes)() = g_yes;
  g_yes = nullptr;
  LOGI("menu: the player said yes");
  if (yes) yes();
}

void on_log(const char* line) {
  if (strstr(line, "Deactivate menu [")) {
    if (g_open.load() > 0) --g_open;
  } else if (strstr(line, "Activate menu [")) {
    ++g_open;
  }
}

}  // namespace

bool page_open() { return g_open.load() > 0; }

bool ask(const std::string& text, const std::string& english, void (*yes)()) {
  uint8_t* mgr = game::ref<uint8_t*>(kMenuManager);
  if (!mgr) {
    mgr = reinterpret_cast<uint8_t* (*)()>(game::at(kMakeMenuManager))();
    game::ref<uint8_t*>(kMenuManager) = mgr;
  }
  uint8_t* counted = hud::counted_text(hud::cyrillic() ? text : english);
  if (!mgr || !counted) {
    LOGW("menu: the game's question window cannot be opened now");
    return false;
  }
  g_yes = yes;
  Text message = {counted, 6, 0};
  Delegate ok = {reinterpret_cast<void*>(&on_ok), reinterpret_cast<void*>(game::at(kDelegateThunk))};
  using Fn = void (*)(uint8_t*, Text*, Delegate*, void*, int, char, void*);
  Fn show = *reinterpret_cast<Fn*>(*reinterpret_cast<uint8_t**>(mgr) + 0x68);
  show(mgr, &message, &ok, reinterpret_cast<void*>(game::at(kEmptyString)), 3, 0, nullptr);
  InterlockedDecrement(reinterpret_cast<volatile LONG*>(counted + 8));  // the window keeps its own hold
  LOGI("menu: asked: %s", text.c_str());
  return true;
}

void install(const Config& cfg) {
  if (cfg.net_mode == Config::NetMode::Off && !cfg.lab) return;
  engine::on_log(&on_log);
}

}  // namespace menu
