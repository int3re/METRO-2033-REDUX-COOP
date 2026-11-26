#include "mod/window.h"

#include <windows.h>

#include "common/log.h"

namespace window {
namespace {

Config g_cfg;
uint64_t g_last = 0;
int g_told = 0;

BOOL CALLBACK pick(HWND w, LPARAM out) {
  if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
  RECT r;
  if (!GetWindowRect(w, &r) || r.right - r.left < 320 || r.bottom - r.top < 200) return TRUE;
  *reinterpret_cast<HWND*>(out) = w;
  return FALSE;
}

}  // namespace

void install(const Config& cfg) { g_cfg = cfg; }

void tick() {
  if (!g_cfg.borderless || g_cfg.lab) return;
  uint64_t now = GetTickCount64();
  if (now - g_last < 500) return;
  g_last = now;
  HWND w = nullptr;
  EnumThreadWindows(GetCurrentThreadId(), &pick, reinterpret_cast<LPARAM>(&w));  // (the game's window is this thread's)
  if (!w) return;
  LONG_PTR style = GetWindowLongPtrW(w, GWL_STYLE);
  HMONITOR mon = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(mon, &mi)) return;
  const RECT& m = mi.rcMonitor;
  RECT r;
  GetWindowRect(w, &r);
  bool framed = (style & (WS_CAPTION | WS_THICKFRAME)) != 0;
  bool fits = r.left == m.left && r.top == m.top && r.right == m.right && r.bottom == m.bottom;
  if (!framed && fits) return;
  SetWindowLongPtrW(w, GWL_STYLE, (style & ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_POPUP);
  SetWindowPos(w, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
  if (g_told++ < 5)
    LOGI("window: the game's window without a frame over the monitor (%ldx%ld)", m.right - m.left, m.bottom - m.top);
}

}  // namespace window
