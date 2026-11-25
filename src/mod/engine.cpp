#include "mod/engine.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/log.h"
#include "mod/game.h"
#include "mod/patch.h"

namespace engine {
namespace {

using LogLineFn = void (*)(const char*);
LogLineFn g_log_line = nullptr;
bool g_copy_log = true;
std::atomic<bool> g_ready{false};
LARGE_INTEGER g_ready_at{}, g_freq{};
std::mutex g_lock;
std::unordered_map<std::string, int> g_counts;  // needle -> hits (filled lazily by count_log)
std::string g_seen;                              // every engine line, for count_log (bounded)
std::string g_last_line;                         // the engine line written last
unsigned g_repeats = 0;                          // how many times it came again since

void (*g_log_listeners[16])(const char*) = {};
void (*g_tickers[16])() = {};

void on_log_line(const char* line) {
  if (line) {
    for (auto fn : g_log_listeners)
      if (fn) fn(line);
    {
      std::lock_guard<std::mutex> hold(g_lock);
      if (g_copy_log) {
        // Some levels make the engine say one line every frame: it is written once, then counted.
        if (g_last_line == line) {
          ++g_repeats;
        } else {
          if (g_repeats) LOGI("[game]   (the line above %u more times)", g_repeats);
          g_repeats = 0;
          g_last_line = line;
          LOGI("[game] %s", line);
        }
      }
      if (g_seen.size() < (8u << 20)) {
        g_seen += line;
        g_seen += '\n';
      }
    }
    if (!g_ready.load(std::memory_order_relaxed) && strstr(line, "initialization finished")) {
      QueryPerformanceCounter(&g_ready_at);
      g_ready.store(true);
      LOGI("the engine's start-up is over");
    }
  }
  g_log_line(line);
}

// The window's message pump: the one place of the main thread that turns in every state of the game.
using PeekMessageFn = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
PeekMessageFn g_peek_message = nullptr;
std::mutex g_main_lock;
std::vector<std::function<void()>> g_main_jobs;
std::atomic<bool> g_main_pending{false};

BOOL WINAPI on_peek_message(LPMSG msg, HWND hwnd, UINT lo, UINT hi, UINT remove) {
  if (g_main_pending.load(std::memory_order_relaxed)) {
    std::vector<std::function<void()>> jobs;
    {
      std::lock_guard<std::mutex> hold(g_main_lock);
      jobs.swap(g_main_jobs);
      g_main_pending.store(false);
    }
    for (auto& job : jobs) job();
  }
  for (auto fn : g_tickers)
    if (fn) fn();
  return g_peek_message(msg, hwnd, lo, hi, remove);
}

void* console() {
  using Get = void* (*)();
  return reinterpret_cast<Get>(game::at(game::kConsoleGet))();
}

}  // namespace

void install(const Config& cfg) {
  g_copy_log = cfg.log_engine;
  QueryPerformanceFrequency(&g_freq);
  patch::hook("engine log line", game::at(game::kLogLine), reinterpret_cast<void*>(&on_log_line),
              reinterpret_cast<void**>(&g_log_line));

  patch::import("USER32.dll", "PeekMessageA", reinterpret_cast<void*>(&on_peek_message), reinterpret_cast<void**>(&g_peek_message));
}

void on_main(std::function<void()> fn) {
  std::lock_guard<std::mutex> hold(g_main_lock);
  g_main_jobs.push_back(std::move(fn));
  g_main_pending.store(true);
}

void to_main_menu() {
  using Handler = void (*)(void*, void*);
  // A JOINED game cannot take that way: its "change_map" is "disconnect 1" + a new local game + the map, and the
  // menu then comes up as a console's title ("press START") and after it as a blurred background with nothing to
  // press - the friend's frozen screen of 2026-11-25; neither the mark, a key nor START helped (the lab). A joined
  // game leaves the network game ("disconnect") and loads the menu the way the game itself does at its start
  // ("map 000"): the menu's desk comes up by itself (re/shots/home40-quit).
  bool joined = game::ref<uintptr_t>(game::kNetHost) > 1 && game::ref<int>(game::kNetLocalPlayer) != 0;
  LOGI("engine: back to the main menu (%s)", joined ? "a joined game: out of the network game, then the menu as at the start"
                                                     : "the game's own way");
  if (joined) {
    command("disconnect", false);
    command("map 000", false);
    return;
  }
  reinterpret_cast<Handler>(game::at(0x666B70))(nullptr, nullptr);
}

void on_log(void (*fn)(const char*)) {
  for (auto& slot : g_log_listeners)
    if (!slot) {
      slot = fn;
      return;
    }
  LOGE("engine: no room for another log listener");
}

void on_tick(void (*fn)()) {
  for (auto& slot : g_tickers)
    if (!slot) {
      slot = fn;
      return;
    }
  LOGE("engine: no room for another tick");
}

bool ready() { return g_ready.load(); }

double since_ready() {
  if (!ready()) return 0;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return static_cast<double>(now.QuadPart - g_ready_at.QuadPart) / static_cast<double>(g_freq.QuadPart);
}

void command(const std::string& line, bool queued) {
  void* con = console();
  if (!con) {
    LOGE("console command '%s': no console", line.c_str());
    return;
  }
  using Exec = void (*)(void*, const char*, ...);
  void** vtbl = *reinterpret_cast<void***>(con);
  Exec exec = reinterpret_cast<Exec>(vtbl[(queued ? game::kConsoleExecQueued : game::kConsoleExecNow) / sizeof(void*)]);
  LOGI("console%s: %s", queued ? "" : " (now)", line.c_str());
  exec(con, "%s", line.c_str());
}

int count_log(const char* needle) {
  std::lock_guard<std::mutex> hold(g_lock);
  int n = 0;
  size_t len = strlen(needle);
  for (size_t p = g_seen.find(needle); p != std::string::npos; p = g_seen.find(needle, p + len)) ++n;
  return n;
}

}  // namespace engine
