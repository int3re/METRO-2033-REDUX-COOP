#include "mod/menu.h"
#include "mod/loot.h"
#include "mod/motions.h"
#include "mod/revive.h"
#include "mod/hud.h"
#include "mod/lab.h"

#include <windows.h>
#include <tlhelp32.h>

#include <d3d11.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/log.h"
#include "mod/engine.h"
#include "mod/game.h"
#include "mod/net.h"
#include "mod/patch.h"

namespace lab {
namespace {

Config g_cfg;
HWND g_window = nullptr;
constexpr int kOffX = -30000, kOffY = -30000;  // where the lab's window lives: far off every monitor

// --- window: off the screen, never the active one ----------------------------------------------------
using CreateWindowExAFn = HWND(WINAPI*)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
CreateWindowExAFn g_create_window = nullptr;
HWND WINAPI on_create_window(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int w, int h, HWND parent,
                             HMENU menu, HINSTANCE inst, LPVOID param) {
  bool top = parent == nullptr && !(style & WS_CHILD);
  if (top) {
    ex |= WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    ex &= ~static_cast<DWORD>(WS_EX_TOPMOST | WS_EX_APPWINDOW);
    x = kOffX;
    y = kOffY;
  }
  HWND hwnd = g_create_window(ex, cls, title, style, x, y, w, h, parent, menu, inst, param);
  if (top && !g_window) {
    g_window = hwnd;
    LOGI("lab: the game's window %p is made off the screen (%dx%d)", hwnd, w, h);
  }
  return hwnd;
}

using ShowWindowFn = BOOL(WINAPI*)(HWND, int);
ShowWindowFn g_show_window = nullptr;
BOOL WINAPI on_show_window(HWND hwnd, int cmd) {
  if (hwnd == g_window) {
    if (cmd == SW_MINIMIZE || cmd == SW_SHOWMINIMIZED || cmd == SW_FORCEMINIMIZE || cmd == SW_HIDE) return TRUE;
    BOOL r = g_show_window(hwnd, SW_SHOWNOACTIVATE);
    // The game takes itself for a background application until its window is told it is the active one.
    PostMessageA(hwnd, WM_ACTIVATEAPP, TRUE, 0);
    return r;
  }
  return g_show_window(hwnd, cmd);
}

using SetWindowPosFn = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
SetWindowPosFn g_set_window_pos = nullptr;
BOOL WINAPI on_set_window_pos(HWND hwnd, HWND after, int x, int y, int w, int h, UINT flags) {
  if (hwnd == g_window) {
    flags |= SWP_NOACTIVATE | SWP_NOZORDER;
    flags &= ~static_cast<UINT>(SWP_SHOWWINDOW);
    x = kOffX;
    y = kOffY;
  }
  return g_set_window_pos(hwnd, after, x, y, w, h, flags);
}

// --- no keyboard, no mouse, no cursor ------------------------------------------------------------------
BOOL WINAPI on_set_cursor_pos(int, int) { return TRUE; }
BOOL WINAPI on_get_cursor_pos(POINT* p) {
  if (p) p->x = kOffX + 100, p->y = kOffY + 100;
  return TRUE;
}
SHORT WINAPI on_get_async_key_state(int) { return 0; }
int WINAPI on_show_cursor(BOOL show) { return show ? 0 : -1; }
// The game sets a keyboard hook (it holds the Windows key): never in a test run.
HHOOK WINAPI on_set_windows_hook(int id, HOOKPROC, HINSTANCE, DWORD) {
  LOGI("lab: SetWindowsHookEx(%d) not installed", id);
  return nullptr;
}

int WINAPI on_message_box(HWND, LPCSTR text, LPCSTR caption, UINT type) {
  LOGW("lab: message box [%s] %s (type %#x): answered without showing", caption ? caption : "", text ? text : "", type);
  UINT kind = type & 0xF;
  return kind == MB_YESNO || kind == MB_YESNOCANCEL ? IDNO : IDOK;
}

// --- the user's folder and the registry -----------------------------------------------------------------
using SpecialFolderFn = BOOL(WINAPI*)(HWND, LPWSTR, int, BOOL);
SpecialFolderFn g_special_folder = nullptr;
BOOL WINAPI on_special_folder(HWND owner, LPWSTR path, int csidl, BOOL create) {
  if (g_cfg.lab_userdir.empty() || !path) return g_special_folder(owner, path, csidl, create);
  lstrcpynW(path, g_cfg.lab_userdir.c_str(), MAX_PATH);
  CreateDirectoryW(path, nullptr);
  LOGI("lab: special folder %#x -> %ls", csidl, path);
  return TRUE;
}

// The game keeps a "previous launch failed" mark in the registry: a test run that is stopped from outside
// must not leave it for the player's own game.
LSTATUS WINAPI on_reg_open(HKEY, LPCSTR, DWORD, REGSAM, PHKEY) { return ERROR_ACCESS_DENIED; }
LSTATUS WINAPI on_reg_create(HKEY, LPCSTR, DWORD, LPSTR, DWORD, REGSAM, const LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD) {
  return ERROR_ACCESS_DENIED;
}

// --- never a full screen ------------------------------------------------------------------------------------
// The game asks dxgi.dll for CreateDXGIFactory by name. The lab hands it its own: the swap chain is always
// made windowed and can never be switched to the full screen, whatever the game's settings say.
using SetFullscreenFn = HRESULT(STDMETHODCALLTYPE*)(void*, BOOL, void*);
SetFullscreenFn g_set_fullscreen = nullptr;
HRESULT STDMETHODCALLTYPE on_set_fullscreen(void* self, BOOL fullscreen, void*) {
  if (fullscreen) LOGW("lab: the game asked for the full screen: refused");
  return g_set_fullscreen(self, FALSE, nullptr);
}

// --- a picture of what the game shows -------------------------------------------------------------------------
// "!shot <name>": the next frame the game presents is written to shots/<name>.bmp next to the exe. The frame is
// read from the swap chain's back buffer in the game's own Present call, so the window may be off the screen.
CRITICAL_SECTION g_shot_lock;
std::string g_shot_name;  // empty: no shot is asked for

void save_frame(IDXGISwapChain* swap, const std::string& name) {
  ID3D11Texture2D* back = nullptr;
  if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) {
    LOGW("lab: shot %s: the swap chain gave no back buffer", name.c_str());
    return;
  }
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  ID3D11Texture2D* resolved = nullptr;
  ID3D11Texture2D* staging = nullptr;
  back->GetDevice(&dev);
  dev->GetImmediateContext(&ctx);
  D3D11_TEXTURE2D_DESC d{};
  back->GetDesc(&d);
  D3D11_TEXTURE2D_DESC sd = d;
  sd.MipLevels = sd.ArraySize = 1;
  sd.SampleDesc.Count = 1;
  sd.SampleDesc.Quality = 0;
  sd.MiscFlags = 0;
  bool ok = true;
  if (d.SampleDesc.Count > 1) {
    sd.Usage = D3D11_USAGE_DEFAULT;
    sd.BindFlags = D3D11_BIND_RENDER_TARGET;
    sd.CPUAccessFlags = 0;
    ok = SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &resolved));
    if (ok) ctx->ResolveSubresource(resolved, 0, back, 0, d.Format);
  }
  sd.Usage = D3D11_USAGE_STAGING;
  sd.BindFlags = 0;
  sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ok = ok && SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &staging));
  D3D11_MAPPED_SUBRESOURCE m{};
  if (ok) {
    ctx->CopyResource(staging, resolved ? resolved : back);
    ok = SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m));
  }
  if (!ok) {
    LOGW("lab: shot %s: could not read the frame (%ux%u, format %u, samples %u)", name.c_str(), d.Width, d.Height,
         static_cast<unsigned>(d.Format), d.SampleDesc.Count);
  } else {
    // 0 = red first, 1 = blue first, 2 = 10 bits a channel
    int kind = d.Format >= DXGI_FORMAT_B8G8R8A8_UNORM && d.Format <= DXGI_FORMAT_B8G8R8X8_UNORM_SRGB ? 1
               : d.Format >= DXGI_FORMAT_R10G10B10A2_TYPELESS && d.Format <= DXGI_FORMAT_R10G10B10A2_UINT ? 2 : 0;
    const unsigned row = (d.Width * 3 + 3) & ~3u;
    std::vector<uint8_t> out(static_cast<size_t>(row) * d.Height);
    for (unsigned y = 0; y < d.Height; ++y) {
      const uint8_t* src = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
      uint8_t* dst = out.data() + static_cast<size_t>(d.Height - 1 - y) * row;  // a .bmp goes bottom up, blue first
      for (unsigned x = 0; x < d.Width; ++x, src += 4, dst += 3) {
        if (kind == 1) {
          dst[0] = src[0], dst[1] = src[1], dst[2] = src[2];
        } else if (kind == 2) {
          uint32_t v;
          memcpy(&v, src, 4);
          dst[2] = static_cast<uint8_t>((v & 0x3FF) >> 2);
          dst[1] = static_cast<uint8_t>(((v >> 10) & 0x3FF) >> 2);
          dst[0] = static_cast<uint8_t>(((v >> 20) & 0x3FF) >> 2);
        } else {
          dst[0] = src[2], dst[1] = src[1], dst[2] = src[0];
        }
      }
    }
    ctx->Unmap(staging, 0);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t* slash = wcsrchr(path, L'\\')) *slash = 0;
    std::wstring dir = std::wstring(path) + L"\\shots";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring file = dir + L"\\" + std::wstring(name.begin(), name.end()) + L".bmp";
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + static_cast<DWORD>(out.size());
    ih.biSize = sizeof(ih);
    ih.biWidth = static_cast<LONG>(d.Width);
    ih.biHeight = static_cast<LONG>(d.Height);
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biSizeImage = static_cast<DWORD>(out.size());
    if (FILE* f = _wfopen(file.c_str(), L"wb")) {
      fwrite(&fh, sizeof(fh), 1, f);
      fwrite(&ih, sizeof(ih), 1, f);
      fwrite(out.data(), 1, out.size(), f);
      fclose(f);
      LOGI("lab: shot %s: %ux%u (format %u) written", name.c_str(), d.Width, d.Height, static_cast<unsigned>(d.Format));
    } else {
      LOGW("lab: shot %s: could not write %ls", name.c_str(), file.c_str());
    }
  }
  if (staging) staging->Release();
  if (resolved) resolved->Release();
  if (ctx) ctx->Release();
  if (dev) dev->Release();
  back->Release();
}

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
PresentFn g_present = nullptr;
HRESULT STDMETHODCALLTYPE on_present(IDXGISwapChain* self, UINT sync, UINT flags) {
  if (!(flags & DXGI_PRESENT_TEST)) {
    std::string name;
    EnterCriticalSection(&g_shot_lock);
    name.swap(g_shot_name);
    LeaveCriticalSection(&g_shot_lock);
    if (!name.empty()) save_frame(self, name);
  }
  HRESULT r = g_present(self, sync, flags);
  // A test copy is not looked at: it draws [lab] fps frames a second and sleeps the rest (the user's PC is in use
  // while tests run - two copies drawing as fast as they could took 70 % of the processor).
  if (g_cfg.lab_fps > 0 && !(flags & DXGI_PRESENT_TEST)) {
    static LARGE_INTEGER freq = {}, last = {};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double frame = 1.0 / g_cfg.lab_fps;
    double spent = last.QuadPart ? static_cast<double>(now.QuadPart - last.QuadPart) / static_cast<double>(freq.QuadPart) : frame;
    if (spent < frame) Sleep(static_cast<DWORD>((frame - spent) * 1000.0));
    QueryPerformanceCounter(&last);
  }
  return r;
}

void shot_command(const char* args) {
  std::string name;
  for (const char* p = args; *p; ++p)
    if (isalnum(static_cast<unsigned char>(*p)) || *p == '_' || *p == '-') name += *p;
  if (name.empty()) {
    LOGW("lab: !shot <name>");
    return;
  }
  EnterCriticalSection(&g_shot_lock);
  g_shot_name = name;
  LeaveCriticalSection(&g_shot_lock);
}

struct SwapDesc {  // DXGI_SWAP_CHAIN_DESC
  UINT width, height, refresh_num, refresh_den, format, scanline, scaling;
  UINT sample_count, sample_quality, usage, buffer_count;
  HWND window;
  BOOL windowed;
  UINT effect, flags;
};
using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(void*, void*, SwapDesc*, void**);
CreateSwapChainFn g_create_swap_chain = nullptr;
HRESULT STDMETHODCALLTYPE on_create_swap_chain(void* self, void* device, SwapDesc* desc, void** out) {
  if (desc) {
    LOGI("lab: swap chain %ux%u, %s -> windowed", desc->width, desc->height, desc->windowed ? "windowed" : "FULL SCREEN");
    desc->windowed = TRUE;
  }
  HRESULT hr = g_create_swap_chain(self, device, desc, out);
  if (SUCCEEDED(hr) && out && *out && !g_set_fullscreen) {
    void** vt = *reinterpret_cast<void***>(*out);
    patch::hook("IDXGISwapChain::SetFullscreenState", reinterpret_cast<uintptr_t>(vt[10]),
                reinterpret_cast<void*>(&on_set_fullscreen), reinterpret_cast<void**>(&g_set_fullscreen));
    patch::hook("IDXGISwapChain::Present", reinterpret_cast<uintptr_t>(vt[8]), reinterpret_cast<void*>(&on_present),
                reinterpret_cast<void**>(&g_present));
  }
  return hr;
}

using CreateFactoryFn = HRESULT(WINAPI*)(const void*, void**);
CreateFactoryFn g_create_factory = nullptr;
HRESULT WINAPI on_create_factory(const void* iid, void** out) {
  HRESULT hr = g_create_factory(iid, out);
  if (SUCCEEDED(hr) && out && *out && !g_create_swap_chain) {
    void** vt = *reinterpret_cast<void***>(*out);
    patch::hook("IDXGIFactory::CreateSwapChain", reinterpret_cast<uintptr_t>(vt[10]),
                reinterpret_cast<void*>(&on_create_swap_chain), reinterpret_cast<void**>(&g_create_swap_chain));
  }
  return hr;
}

using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
GetProcAddressFn g_get_proc = nullptr;
FARPROC WINAPI on_get_proc(HMODULE module, LPCSTR name) {
  FARPROC p = g_get_proc(module, name);
  if (p && reinterpret_cast<uintptr_t>(name) > 0xFFFF && !strcmp(name, "CreateDXGIFactory")) {
    g_create_factory = reinterpret_cast<CreateFactoryFn>(p);
    return reinterpret_cast<FARPROC>(&on_create_factory);
  }
  return p;
}

// --- a crash goes into the log, not into a dialog -----------------------------------------------------------
// The real call chain of the thread that fell, by the modules' unwind tables (the scan of the stack that
// on_crash prints as "stack:" also shows stale return addresses of earlier calls).
void real_calls(const CONTEXT* c, char* out, size_t cap) {
  out[0] = 0;
  size_t used = 0;
  __try {
    CONTEXT walk = *c;
    uintptr_t base = game::base();
    for (int i = 0; i < 48 && used + 48 < cap; ++i) {
      DWORD64 image = 0;
      PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(walk.Rip, &image, nullptr);
      if (!fn) {  // a function without a frame: its return address is on the top of the stack
        walk.Rip = *reinterpret_cast<const DWORD64*>(walk.Rsp);
        walk.Rsp += 8;
      } else {
        void* data = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, walk.Rip, fn, &walk, &data, &frame, nullptr);
      }
      if (walk.Rip < 0x10000) break;
      if (walk.Rip >= base && walk.Rip < base + game::kSizeOfImage) {
        used += snprintf(out + used, cap - used, " +%llX", static_cast<unsigned long long>(walk.Rip - base));
        continue;
      }
      HMODULE m = nullptr;
      char path[MAX_PATH] = "";
      if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCSTR>(walk.Rip), &m) &&
          m && GetModuleFileNameA(m, path, sizeof(path))) {
        const char* name = strrchr(path, '\\');
        used += snprintf(out + used, cap - used, " %s+%llX", name ? name + 1 : path,
                         static_cast<unsigned long long>(walk.Rip - reinterpret_cast<uintptr_t>(m)));
      } else {
        used += snprintf(out + used, cap - used, " ?%llX", static_cast<unsigned long long>(walk.Rip));
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// --- "!watch <id> <offset>": who writes this field -------------------------------------------------------------
// A hardware data breakpoint (debug register 0, 4 bytes, on write) on every thread of the game; each write is
// logged with the writer's call chain (the first 40). New threads made after the command are not watched.
std::atomic<int> g_watch_hits{0};
uintptr_t g_watch_at = 0;

LONG WINAPI on_watch(EXCEPTION_POINTERS* ep) {
  if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !g_watch_at || !(ep->ContextRecord->Dr6 & 1))
    return EXCEPTION_CONTINUE_SEARCH;
  ep->ContextRecord->Dr6 = 0;
  // only the writes that lower the value (a float field: the health's regrowth writes it every frame)
  static float last = 1e30f;
  float now = *reinterpret_cast<const float*>(g_watch_at);
  bool lower = now < last - 0.001f;
  last = now;
  if (lower && g_watch_hits.fetch_add(1) < 40) {
    char calls[1024];
    real_calls(ep->ContextRecord, calls, sizeof(calls));
    uintptr_t base = game::base();
    uintptr_t rip = ep->ContextRecord->Rip;
    LOGI("lab: watch: %llX became %.3f (%08X), written at %s%llX; calls:%s", static_cast<unsigned long long>(g_watch_at), now,
         *reinterpret_cast<const uint32_t*>(g_watch_at), rip >= base && rip < base + game::kSizeOfImage ? "+" : "?",
         static_cast<unsigned long long>(rip >= base && rip < base + game::kSizeOfImage ? rip - base : rip), calls);
  }
  return EXCEPTION_CONTINUE_EXECUTION;
}

void watch_address(uintptr_t at) {
  static PVOID handler = nullptr;
  if (!handler) handler = AddVectoredExceptionHandler(1, &on_watch);
  g_watch_at = at;
  g_watch_hits.store(0);
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te = {sizeof(te)};
  int set = 0;
  DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid) continue;
    HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
    if (!t) continue;
    bool self = te.th32ThreadID == me;
    if (!self) SuspendThread(t);
    CONTEXT c = {};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (self ? true : GetThreadContext(t, &c) != 0) {
      if (self) GetThreadContext(t, &c);
      c.Dr0 = at;
      c.Dr7 = (c.Dr7 & ~0xF0003ull) | 1ull | (1ull << 16) | (3ull << 18);  // local 0, on write, 4 bytes
      if (SetThreadContext(t, &c)) ++set;
    }
    if (!self) ResumeThread(t);
    CloseHandle(t);
  }
  CloseHandle(snap);
  LOGI("lab: watching %llX for writes on %d threads", static_cast<unsigned long long>(at), set);
}

LONG WINAPI on_crash(EXCEPTION_POINTERS* ep) {
  const EXCEPTION_RECORD* r = ep->ExceptionRecord;
  const CONTEXT* c = ep->ContextRecord;
  uintptr_t base = game::base();
  uintptr_t ip = reinterpret_cast<uintptr_t>(r->ExceptionAddress);
  char where[64];
  if (ip >= base && ip < base + game::kSizeOfImage)
    snprintf(where, sizeof(where), "metro.exe+%llX", static_cast<unsigned long long>(ip - base));
  else
    snprintf(where, sizeof(where), "%p", r->ExceptionAddress);
  LOGE("CRASH %#lx at %s (info %llX %llX)", r->ExceptionCode, where,
       r->NumberParameters > 0 ? static_cast<unsigned long long>(r->ExceptionInformation[0]) : 0ull,
       r->NumberParameters > 1 ? static_cast<unsigned long long>(r->ExceptionInformation[1]) : 0ull);
  LOGE("  rax %016llX rbx %016llX rcx %016llX rdx %016llX", c->Rax, c->Rbx, c->Rcx, c->Rdx);
  LOGE("  rsi %016llX rdi %016llX rbp %016llX rsp %016llX", c->Rsi, c->Rdi, c->Rbp, c->Rsp);
  LOGE("  r8  %016llX r9  %016llX r10 %016llX r11 %016llX", c->R8, c->R9, c->R10, c->R11);
  LOGE("  r12 %016llX r13 %016llX r14 %016llX r15 %016llX", c->R12, c->R13, c->R14, c->R15);
  // Return addresses on the stack that point into the exe: a rough call chain.
  std::string chain;
  const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(c->Rsp);
  int found = 0;
  for (int i = 0; i < 400 && found < 24; ++i) {
    uintptr_t v = 0;
    if (IsBadReadPtr(sp + i, sizeof(v))) break;
    v = sp[i];
    if (v >= base + 0x1000 && v < base + 0x9CA000) {
      char one[32];
      snprintf(one, sizeof(one), " +%llX", static_cast<unsigned long long>(v - base));
      chain += one;
      ++found;
    }
  }
  LOGE("  stack:%s", chain.c_str());
  {
    char calls[1600];
    real_calls(c, calls, sizeof(calls));
    LOGE("  calls:%s", calls);
  }
  LOGE("  thread %lu (the frame's thread %lu)", GetCurrentThreadId(), net::lab_main_thread());
  {
    std::string updating = net::lab_updating();
    if (!updating.empty()) LOGE("  updating: %s", updating.c_str());
  }
  {
    const struct { const char* name; DWORD64 value; } regs[] = {
        {"rax", c->Rax}, {"rbx", c->Rbx}, {"rcx", c->Rcx}, {"rdx", c->Rdx}, {"rsi", c->Rsi}, {"rdi", c->Rdi}, {"rbp", c->Rbp},
        {"r8", c->R8},   {"r9", c->R9},   {"r10", c->R10}, {"r11", c->R11}, {"r12", c->R12}, {"r13", c->R13}, {"r14", c->R14},
        {"r15", c->R15}};
    for (const auto& reg : regs) {
      std::string who = net::lab_whose(static_cast<uintptr_t>(reg.value));
      if (!who.empty()) LOGE("  %s points %s", reg.name, who.c_str());
    }
    int named = 0;
    uintptr_t last = 0;
    for (int i = 0; i < 900 && named < 16; ++i) {
      if (IsBadReadPtr(sp + i, sizeof(uintptr_t))) break;
      uintptr_t v = sp[i];
      if (v < 0x10000 || v == last || (v >= base && v < base + game::kSizeOfImage)) continue;
      std::string who = net::lab_whose(v);
      if (who.empty()) continue;
      last = v;
      LOGE("  stack +%X points %s", i * 8, who.c_str());
      ++named;
    }
    // ... and the objects the registers point into: whose part are they (an animator, a model, a state)
    for (const auto& reg : regs) {
      std::string who = net::lab_held_by(static_cast<uintptr_t>(reg.value));
      if (!who.empty()) LOGE("  %s is %s", reg.name, who.c_str());
    }
    // rdi in the animation's node code is the node's state: the two "motions" it holds at +8 and +16
    const uint8_t* state = reinterpret_cast<const uint8_t*>(c->Rdi);
    if (c->Rdi > 0x10000 && !IsBadReadPtr(state, 0x1C0)) {
      for (int at : {8, 16}) {
        uintptr_t m = *reinterpret_cast<const uintptr_t*>(state + at);
        if (m < 0x10000 || IsBadReadPtr(reinterpret_cast<const void*>(m), 0xA0)) {
          LOGE("  [rdi+%d] = %016llX (not readable)", at, static_cast<unsigned long long>(m));
          continue;
        }
        const uint8_t* mp = reinterpret_cast<const uint8_t*>(m);
        std::string who = net::lab_held_by(m);
        LOGE("  [rdi+%d] = %016llX: first word %016llX, word at +0x96 %04X%s%s", at, static_cast<unsigned long long>(m),
             static_cast<unsigned long long>(*reinterpret_cast<const uintptr_t*>(mp)), *reinterpret_cast<const uint16_t*>(mp + 0x96),
             who.empty() ? "" : "; it is ", who.c_str());
      }
    }
  }
  // What rcx and rsi point at (a reader's place in its stream, an object): 160 bytes before, 64 after.
  for (uintptr_t p : {static_cast<uintptr_t>(c->Rcx), static_cast<uintptr_t>(c->Rsi), static_cast<uintptr_t>(c->Rdi)}) {
    const uint8_t* at = reinterpret_cast<const uint8_t*>(p);
    if (p < 0x10000 || IsBadReadPtr(at - 160, 224)) continue;
    for (int row = -160; row < 64; row += 32) {
      char hex[128];
      int n = 0;
      for (int i = 0; i < 32; ++i) n += snprintf(hex + n, sizeof(hex) - n, "%02X%s", at[row + i], (i & 3) == 3 ? " " : "");
      LOGE("  [%016llX%+d] %s", static_cast<unsigned long long>(p), row, hex);
    }
  }
  TerminateProcess(GetCurrentProcess(), 0xC0DE);
  return EXCEPTION_EXECUTE_HANDLER;
}

using SetFilterFn = LPTOP_LEVEL_EXCEPTION_FILTER(WINAPI*)(LPTOP_LEVEL_EXCEPTION_FILTER);
SetFilterFn g_set_filter = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER WINAPI on_set_filter(LPTOP_LEVEL_EXCEPTION_FILTER) { return nullptr; }  // ours stays

// --- a loaded map starts without "press any key" -------------------------------------------------------------
using LoadWaitBeginFn = void (*)(void*);
LoadWaitBeginFn g_load_wait_begin = nullptr;
void on_load_wait_begin(void* state) {
  g_load_wait_begin(state);
  if (g_cfg.lab_autostart) *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(state) + 0x10) = 1;
}

// --- a second copy of the game on one PC ---------------------------------------------------------------------
bool on_already_running() { return false; }

// --- a gamepad the script holds ---------------------------------------------------------------------------
// The game reads pad 0 through XInputGetState (xinput1_3, ordinal 2) whether its window is in front or not.
// In a test run the real pads are never read; "!pad" gives the game a pad of the script's own for a while:
//   !pad <buttons hex> <left x> <left y> <milliseconds> [right x] [right y] [left trigger] [right trigger]
// (sticks -32768..32767, triggers 0..255; buttons: A 1000, B 2000, X 4000, Y 8000, start 10, back 20,
// left / right shoulder 100 / 200, d-pad up / down / left / right 1 / 2 / 4 / 8).
#pragma pack(push, 1)
struct PadState {  // XINPUT_STATE
  DWORD packet;
  WORD buttons;
  BYTE left_trigger, right_trigger;
  SHORT lx, ly, rx, ry;
};
#pragma pack(pop)
CRITICAL_SECTION g_pad_lock;
PadState g_pad{};
bool g_pad_connected = false;  // from the first "!pad" on the pad stays plugged in (idle between commands)
ULONGLONG g_pad_until = 0;

DWORD WINAPI on_xinput_get_state(DWORD user, PadState* state) {
  if (user != 0 || !state || !g_pad_connected) return ERROR_DEVICE_NOT_CONNECTED;
  EnterCriticalSection(&g_pad_lock);
  if (GetTickCount64() >= g_pad_until) {
    g_pad.buttons = 0;
    g_pad.left_trigger = g_pad.right_trigger = 0;
    g_pad.lx = g_pad.ly = g_pad.rx = g_pad.ry = 0;
  }
  ++g_pad.packet;
  *state = g_pad;
  LeaveCriticalSection(&g_pad_lock);
  revive::filter_pad(state);
  return ERROR_SUCCESS;
}

void pad_command(const char* args) {
  unsigned buttons = 0;
  int lx = 0, ly = 0, ms = 0, rx = 0, ry = 0, lt = 0, rt = 0;
  if (sscanf(args, "%x %d %d %d %d %d %d %d", &buttons, &lx, &ly, &ms, &rx, &ry, &lt, &rt) < 4) {
    LOGW("lab: !pad <buttons hex> <lx> <ly> <ms> [rx ry lt rt]");
    return;
  }
  EnterCriticalSection(&g_pad_lock);
  g_pad.buttons = static_cast<WORD>(buttons);
  g_pad.lx = static_cast<SHORT>(lx);
  g_pad.ly = static_cast<SHORT>(ly);
  g_pad.rx = static_cast<SHORT>(rx);
  g_pad.ry = static_cast<SHORT>(ry);
  g_pad.left_trigger = static_cast<BYTE>(lt);
  g_pad.right_trigger = static_cast<BYTE>(rt);
  g_pad_until = GetTickCount64() + static_cast<ULONGLONG>(ms);
  g_pad_connected = true;
  LeaveCriticalSection(&g_pad_lock);
  LOGI("lab: pad buttons %X, left (%d,%d), right (%d,%d), triggers %d/%d for %d ms", buttons, lx, ly, rx, ry, lt, rt, ms);
}

// --- aiming at an entity ----------------------------------------------------------------------------------
// "!aim <entity id hex> <ms> [trigger 0..255] [height cm]": for that long the script's pad turns the body this PC
// controls towards the entity - sideways and up / down, at a point that many centimetres above the entity's feet
// (100 by default: the middle of a human) - and, once it looks at it, pulls the right trigger that far, in
// bursts. Runs on the script's thread; the next step waits for it.
// An entity id of a lab command: F000N names the body of player slot N (the ids differ from run to run).
unsigned lab_id(unsigned id) {
  if (id < 0xF0000) return id;
  struct Players {
    uint8_t* array;
    uint16_t capacity;
    uint16_t size;
  };
  Players& p = game::ref<Players>(game::kNetPlayers);
  unsigned slot = id - 0xF0000;
  if (!p.array || slot >= p.size) return 0xFFFF;
  return *reinterpret_cast<uint16_t*>(p.array + game::kNetPlayerSize * slot + 0x64);
}

void aim_command(const char* args) {
  unsigned id = 0;
  int ms = 0, trigger = 0, height = 100;
  const int sign = -1;  // a positive angle (target to the right of the look) needs the stick to the left - as measured
  if (sscanf(args, "%x %d %d %d", &id, &ms, &trigger, &height) < 2) {
    LOGW("lab: !aim <id hex> <ms> [trigger] [height cm]");
    return;
  }
  id = lab_id(id);
  float last_pitch = 0;
  ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(ms);
  float worst = 0, last = 0, distance = 0;
  int looks = 0, steps = 0;
  bool seen = false;
  while (GetTickCount64() < end) {
    float yaw = 0;
    bool ok = net::aim_error(id, &yaw, &distance);
    EnterCriticalSection(&g_pad_lock);
    g_pad.buttons = 0;
    g_pad.lx = g_pad.ly = 0;
    g_pad.ry = 0;
    g_pad.left_trigger = 0;
    if (ok) {
      float turn = yaw * 3.0f;  // full stick from 1/3 rad on
      if (turn > 1) turn = 1;
      if (turn < -1) turn = -1;
      if (fabsf(turn) < 0.45f) turn = turn < 0 ? -0.45f : 0.45f;  // below that the game takes the stick for idle
      if (fabsf(yaw) < 0.015f) turn = 0;
      g_pad.rx = static_cast<SHORT>(turn * 32000 * sign);
      float pitch = 0;
      bool level = true;
      if (net::aim_pitch_error(id, height / 100.f, &pitch)) {
        float nod = pitch * 4.0f;
        if (nod > 1) nod = 1;
        if (nod < -1) nod = -1;
        if (fabsf(nod) < 0.45f) nod = nod < 0 ? -0.45f : 0.45f;
        if (fabsf(pitch) < 0.02f) nod = 0;
        g_pad.ry = static_cast<SHORT>(-nod * 32000);  // stick down = look down = the pitch grows
        level = fabsf(pitch) < 0.05f;
        last_pitch = pitch;
      }
      bool on = fabsf(yaw) < 0.06f && level;
      // bursts: the recoil lifts the barrel, a pause lets it come down
      bool burst = (GetTickCount64() % 600) < 220;
      g_pad.right_trigger = static_cast<BYTE>(on && burst ? trigger : 0);
      looks += on;
      ++steps;
      last = yaw;
      if (!seen || fabsf(yaw) > worst) worst = fabsf(yaw);
      seen = true;
    } else {
      g_pad.rx = 0;
      g_pad.right_trigger = 0;
    }
    g_pad_until = GetTickCount64() + 200;
    g_pad_connected = true;
    LeaveCriticalSection(&g_pad_lock);
    Sleep(15);
  }
  EnterCriticalSection(&g_pad_lock);
  g_pad.rx = g_pad.ry = 0;
  g_pad.right_trigger = 0;
  g_pad_until = 0;
  LeaveCriticalSection(&g_pad_lock);
  LOGI("lab: aimed at %X for %d ms: %s, the angle went from %.2f to %.3f rad (up / down left: %.3f), on target %d of %d steps, distance %.1f m",
       id, ms, seen ? "seen" : "NOT THERE", worst, last, last_pitch, looks, steps, distance);
}

// "!hunt <class RVA hex> <ms> <trigger> [height cm] [metres]": as "!aim", but at the nearest living entity of
// that class (a kind of monster) within so many metres, chosen anew all the time; what it aimed at and how the
// targets' health went is logged.
void hunt_command(const char* args) {
  unsigned cls = 0;
  int ms = 0, trigger = 255, height = 60, metres = 12;
  char part[64] = {};  // only entities whose name has this in it
  if (sscanf(args, "%x %d %d %d %d %63s", &cls, &ms, &trigger, &height, &metres, part) < 2) {
    LOGW("lab: !hunt <class hex> <ms> [trigger] [height cm] [metres] [part of the name]");
    return;
  }
  ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(ms);
  unsigned target = 0xFFFF, targets = 0, looks = 0, steps = 0;
  float first = 0, health = 0;
  while (GetTickCount64() < end) {
    float h = 0;
    unsigned now = net::nearest_of_class(cls, static_cast<float>(metres), &h, part);
    if (now != target) {
      if (target != 0xFFFF) LOGI("lab: hunt: %X went from health %.2f to %.2f (%s)", target, first, health, now == 0xFFFF ? "no target left" : "another is nearer");
      target = now;
      first = h;
      targets += now != 0xFFFF;
    }
    health = h;
    float yaw = 0, pitch = 0, distance = 0;
    bool ok = target != 0xFFFF && net::aim_error(target, &yaw, &distance);
    EnterCriticalSection(&g_pad_lock);
    g_pad.buttons = 0;
    g_pad.lx = g_pad.ly = g_pad.ry = g_pad.rx = 0;
    g_pad.left_trigger = g_pad.right_trigger = 0;
    if (ok) {
      float turn = yaw * 3.0f;
      if (turn > 1) turn = 1;
      if (turn < -1) turn = -1;
      if (fabsf(turn) < 0.45f) turn = turn < 0 ? -0.45f : 0.45f;
      if (fabsf(yaw) < 0.015f) turn = 0;
      g_pad.rx = static_cast<SHORT>(-turn * 32000);
      bool level = true;
      if (net::aim_pitch_error(target, height / 100.f, &pitch)) {
        float nod = pitch * 4.0f;
        if (nod > 1) nod = 1;
        if (nod < -1) nod = -1;
        if (fabsf(nod) < 0.45f) nod = nod < 0 ? -0.45f : 0.45f;
        if (fabsf(pitch) < 0.02f) nod = 0;
        g_pad.ry = static_cast<SHORT>(-nod * 32000);
        level = fabsf(pitch) < 0.06f;
      }
      bool on = fabsf(yaw) < 0.08f && level;
      g_pad.right_trigger = static_cast<BYTE>(on && (GetTickCount64() % 600) < 260 ? trigger : 0);
      looks += on;
      ++steps;
    }
    g_pad_until = GetTickCount64() + 200;
    g_pad_connected = true;
    LeaveCriticalSection(&g_pad_lock);
    Sleep(15);
  }
  EnterCriticalSection(&g_pad_lock);
  g_pad.rx = g_pad.ry = 0;
  g_pad.right_trigger = 0;
  g_pad_until = 0;
  LeaveCriticalSection(&g_pad_lock);
  if (target != 0xFFFF) LOGI("lab: hunt: %X went from health %.2f to %.2f (time is up)", target, first, health);
  LOGI("lab: hunted class +%X for %d ms: %u targets, on target %u of %u steps", cls, ms, targets, looks, steps);
}

// --- the mod's own script commands --------------------------------------------------------------------------
void own_command(const std::string& cmd) {
  if (!cmd.compare(0, 5, "!pad ")) {
    pad_command(cmd.c_str() + 5);
    return;
  }
  if (!cmd.compare(0, 6, "!move ")) {  // !move <id hex, FFFF = own body> <x> <y> <z>
    unsigned id = 0;
    float x = 0, y = 0, z = 0;
    if (sscanf(cmd.c_str() + 6, "%x %f %f %f", &id, &x, &y, &z) == 4)
      engine::on_main([=] { net::move_entity(id, x, y, z); });
    return;
  }
  if (!cmd.compare(0, 7, "!front ")) {  // !front <id hex, F000N = slot N's body> <metres>: it stands before this PC's eyes
    unsigned id = 0;
    float metres = 1.f;
    if (sscanf(cmd.c_str() + 7, "%x %f", &id, &metres) >= 1) engine::on_main([id, metres] { net::place_in_front(lab_id(id), metres); });
    return;
  }
  if (!cmd.compare(0, 6, "!hunt ")) {
    hunt_command(cmd.c_str() + 6);
    return;
  }
  if (!cmd.compare(0, 5, "!aim ")) {
    aim_command(cmd.c_str() + 5);
    return;
  }
  if (!cmd.compare(0, 6, "!mark ")) {  // a line for the test scripts to find their place in the log
    LOGI("lab: mark %s", cmd.c_str() + 6);
    return;
  }
  if (!cmd.compare(0, 6, "!play ")) {  // !play <id hex> <motion name>: a thing starts a motion on this PC
    unsigned id = 0;
    char motion[96] = {};
    if (sscanf(cmd.c_str() + 6, "%x %95s", &id, motion) >= 1) engine::on_main([id, m = std::string(motion)] { motions::lab_play(lab_id(id), m); });
    return;
  }
  if (!cmd.compare(0, 12, "!motionlist ")) {  // !motionlist <id hex>: every motion of its skeleton into a file
    unsigned id = 0;
    if (sscanf(cmd.c_str() + 12, "%x", &id) == 1) engine::on_main([id] { motions::lab_list(id); });
    return;
  }
  if (!cmd.compare(0, 10, "!motionof ")) {  // !motionof <id hex>: the motion started last on a thing here
    unsigned id = 0;
    if (sscanf(cmd.c_str() + 10, "%x", &id) == 1) engine::on_main([id] { motions::lab_state(id); });
    return;
  }
  if (!cmd.compare(0, 6, "!take ")) {  // !take <id hex>: this PC's player takes a thing (its own "use")
    unsigned id = 0;
    int kind = 0;
    if (sscanf(cmd.c_str() + 6, "%x %d", &id, &kind) >= 1) engine::on_main([id, kind] { loot::lab_take(id, kind); });
    return;
  }
  if (!cmd.compare(0, 9, "!cantake ")) {
    unsigned id = 0;
    if (sscanf(cmd.c_str() + 9, "%x", &id) == 1) engine::on_main([id] { loot::lab_can_take(id); });
    return;
  }
  if (!cmd.compare(0, 6, "!takes")) {  // !takes <s>: log who hangs things on whom and what inventories take
    unsigned s = 60;
    sscanf(cmd.c_str() + 6, "%u", &s);
    loot::lab_trace(s);
    return;
  }
  if (!cmd.compare(0, 8, "!motions")) {  // !motions <s>: log the motions things start, with who started them
    unsigned s = 60;
    sscanf(cmd.c_str() + 8, "%u", &s);
    motions::lab_trace(s);
    return;
  }
  if (!cmd.compare(0, 9, "!holduse ")) {  // !holduse <ms>: "use" held without a pad (the raise of a fallen teammate)
    unsigned ms = 0;
    if (sscanf(cmd.c_str() + 9, "%u", &ms) == 1) revive::lab_hold_use(ms);
    return;
  }
  if (!cmd.compare(0, 8, "!actions")) {
    unsigned s = 30;
    sscanf(cmd.c_str() + 8, "%u", &s);
    revive::lab_actions(s);
    return;
  }
  if (!cmd.compare(0, 7, "!watch ")) {  // !watch <entity id hex, FFFF = own body> <offset hex>
    unsigned id = 0, offset = 0;
    if (sscanf(cmd.c_str(), "!watch %x %x", &id, &offset) == 2)
      engine::on_main([=] {
        uintptr_t at = net::entity_field(id, offset);
        if (at) watch_address(at);
        else LOGI("lab: watch: no entity %X", id);
      });
    return;
  }
  if (cmd == "!mainmenu") {  // the pause menu's "Exit to the main menu", as the game does it
    engine::on_main([] { engine::to_main_menu(); });
    return;
  }
  if (cmd == "!ask") {  // the game's own yes / no window with a text of the mod's
    menu::ask("Хантер сейчас играет в Metro 2033 Redux. Присоединиться к этой игре?", "Hunter is playing Metro 2033 Redux now. Join this game?",
              [] { LOGI("lab: the answer was yes"); });
    return;
  }
  if (cmd == "!scan") {
    hud::lab_scan();
    return;
  }
  if (!cmd.compare(0, 5, "!text")) {  // !text <font 1-3> <x> <y> <height> <colour hex> <text>: a line on the screen
    hud::lab_text(cmd.c_str() + 5);
    return;
  }
  if (!cmd.compare(0, 6, "!shot ")) {
    shot_command(cmd.c_str() + 6);
    return;
  }
  char name[32] = {};
  unsigned long long a = 0, b = 0;
  int n = sscanf(cmd.c_str(), "!%31s %llx %llx", name, &a, &b);
  if (n < 1) return;
  if (!strcmp(name, "byte") && n == 3) {
    game::ref<uint8_t>(a) = static_cast<uint8_t>(b);
    LOGI("lab: byte [+%llX] = %llX", a, b);
  } else if (!strcmp(name, "dword") && n == 3) {
    game::ref<uint32_t>(a) = static_cast<uint32_t>(b);
    LOGI("lab: dword [+%llX] = %llX", a, b);
  } else if (!strcmp(name, "peek") && n >= 2) {
    int count = n == 3 ? static_cast<int>(b) : 4;
    for (int i = 0; i < count; ++i)
      LOGI("lab: [+%llX] = %016llX", a + i * 8ull, static_cast<unsigned long long>(game::ref<uint64_t>(a + i * 8ull)));
  } else if (!strcmp(name, "net")) {
    engine::on_main([] { net::log_state("lab"); });
  } else if (!strcmp(name, "host")) {
    engine::on_main([] { net::host(); });
  } else if (!strcmp(name, "ctl")) {
    engine::on_main([all = n >= 2 && a != 0] { net::log_controllers(all); });
  } else if (!strcmp(name, "ent")) {
    engine::on_main([id = n >= 2 ? static_cast<unsigned>(a) : 0xFFFFu] { net::log_entity(id); });
  } else if (!strcmp(name, "health") && n >= 3) {
    // !health <entity id hex, FFFF = own body> <per cent, hex: 0 = dead, 64 = full>
    engine::on_main([id = static_cast<unsigned>(a), h = static_cast<float>(b) / 100.f] { net::set_health(id, h); });
  } else if (!strcmp(name, "respawn") && n >= 2) {
    engine::on_main([slot = static_cast<int>(a)] { net::respawn_body(slot); });
  } else if (!strcmp(name, "remove") && n >= 2) {
    engine::on_main([slot = static_cast<int>(a)] { net::remove_body(slot); });
  } else if (!strcmp(name, "without") && n >= 2) {
    engine::on_main([x = static_cast<unsigned>(a), y = n >= 3 ? static_cast<unsigned>(b) : 0u] { net::lab_leave_out(x, y); });
  } else if (!strcmp(name, "attach") && n >= 3) {
    engine::on_main([x = static_cast<unsigned>(a), y = static_cast<unsigned>(b)] { net::lab_attach(x, y); });
  } else if (!strcmp(name, "look")) {
    // !look <entity id hex, FFFF = own body> <offset hex> <count>
    unsigned id = 0, offset = 0, count = 1;
    if (sscanf(cmd.c_str(), "!look %x %x %u", &id, &offset, &count) >= 2)
      engine::on_main([=] { net::look_entity(id, offset, count); });
  } else if (!strcmp(name, "snap") && n >= 2) {
    engine::on_main([id = static_cast<unsigned>(a)] { net::snap_entity(id); });
  } else if (!strcmp(name, "diff") && n >= 2) {
    engine::on_main([id = static_cast<unsigned>(a)] { net::diff_entity(id); });
  } else if (!strcmp(name, "inv") && n >= 2) {
    engine::on_main([id = static_cast<unsigned>(a)] { net::log_inventory(id); });
  } else if (!strcmp(name, "act") && n >= 2) {
    engine::on_main([action = static_cast<unsigned>(a)] { net::lab_inventory_action(action); });
  } else if (!strcmp(name, "slot") && n >= 2) {
    engine::on_main([slot = static_cast<unsigned>(a)] { net::lab_select_slot(slot); });
  } else if (!strcmp(name, "ride") && n >= 3) {  // !ride <id hex, FFFF = own body> <platform id hex, FFFF = off>
    engine::on_main([x = static_cast<unsigned>(a), y = static_cast<unsigned>(b)] { net::lab_ride(x, y); });
  } else if (!strcmp(name, "hang") && n >= 3) {  // !hang <id hex, FFFF = own body> <parent id hex, FFFF = off>: the script's plain attach
    engine::on_main([x = static_cast<unsigned>(a), y = static_cast<unsigned>(b)] { net::lab_hang(x, y); });
  } else if (!strcmp(name, "mem")) {  // !mem <id hex> <offset of a pointer hex> <count>
    unsigned id = 0, offset = 0, count = 16;
    if (sscanf(cmd.c_str(), "!mem %x %x %u", &id, &offset, &count) >= 2)
      engine::on_main([=] { net::look_object(id, offset, count); });
  } else if (!strcmp(name, "bring") && n >= 2) {  // !bring <class hex> [metres hex, default 4]
    engine::on_main([c = static_cast<unsigned>(a), m = n >= 3 ? static_cast<float>(b) : 4.f] { net::lab_bring(c, m); });
  } else if (!strcmp(name, "uses")) {
    engine::on_main([] { net::lab_uses(); });
  } else if (!strcmp(name, "usename")) {  // !usename <entity name> [k]: the k-th use block (from the end) waiting on it
    char what[96] = {};
    unsigned k = 0;
    if (sscanf(cmd.c_str(), "!usename %95s %u", what, &k) >= 1)
      engine::on_main([w = std::string(what), k] { net::lab_use_named(w, k); });
  } else if (!strcmp(name, "use") && n >= 2) {
    engine::on_main([id = static_cast<unsigned>(a)] { net::lab_use(id); });
  } else if (!strcmp(name, "world") && n >= 2) {
    engine::on_main([m = static_cast<unsigned>(a)] { net::dump_world(m); });
  } else if (!strcmp(name, "near") && n >= 2) {
    engine::on_main([m = static_cast<unsigned>(a)] { net::log_near(m); });
  } else if (!strcmp(name, "hit") && n >= 3) {
    engine::on_main([x = static_cast<unsigned>(a), y = static_cast<unsigned>(b)] { net::lab_hit(x, y); });
  } else if (!strcmp(name, "hands") && n >= 2) {  // !hands <way>: the others' first-person hands (0 = left alone)
    engine::on_main([m = static_cast<int>(a)] { net::lab_hands(m); });
  } else if (!strcmp(name, "kids") && n >= 2) {
    engine::on_main([id = static_cast<unsigned>(a)] { net::log_children(id); });
  } else if (!strcmp(name, "refresh") && n >= 2) {
    engine::on_main([id = static_cast<unsigned>(a)] { net::refresh_item(id); });
  } else if (!strcmp(name, "kill") && n >= 2) {
    engine::on_main([id = static_cast<unsigned>(a)] { net::destroy_entity(id); });
  } else if (!strcmp(name, "body")) {
    engine::on_main([] { net::log_body(); });
  } else if (!strcmp(name, "poke")) {
    // !poke <entity id hex, FFFF = own body> <offset hex> <size> <value hex>
    unsigned id = 0, offset = 0, size = 0, value = 0;
    if (sscanf(cmd.c_str(), "!poke %x %x %u %x", &id, &offset, &size, &value) == 4)
      engine::on_main([=] { net::poke_entity(id, offset, size, value); });
  } else if (!strcmp(name, "ghost")) {
    // one copy of the game: a body for a player who is not there, to look at it
    engine::on_main([] {
      int slot = net::add_empty_slot();
      if (slot > 0) net::spawn_body(slot);
    });
  } else if (!strcmp(name, "spawn") && n >= 2) {
    engine::on_main([slot = static_cast<int>(a)] { net::spawn_body(slot); });
  } else if (!strcmp(name, "mp") && n >= 2) {
    engine::on_main([on = a != 0] { net::set_network_game(on); });
  } else if (!strcmp(name, "finish")) {
    engine::on_main([] { net::finish_load(); });
  } else if (!strcmp(name, "unpause")) {
    engine::on_main([] { net::unpause_all(); });
  } else {
    LOGW("lab: unknown command %s", cmd.c_str());
  }
}

// A game that stopped turning: where every thread of it is (its call chain by the unwind tables). Each thread is
// held only while its registers are read and the chain is walked; the lines are written after it goes on.
void dump_threads(const char* why) {
  LOGW("lab: %s - where every thread is:", why);
  DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te = {};
  te.dwSize = sizeof(te);
  uintptr_t base = game::base();
  int shown = 0, others = 0;
  for (BOOL more = Thread32First(snap, &te); more && shown < 80; more = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
    if (!t) continue;
    char calls[1200] = "";
    DWORD64 rip = 0;
    if (SuspendThread(t) != static_cast<DWORD>(-1)) {
      CONTEXT c = {};
      c.ContextFlags = CONTEXT_FULL;
      if (GetThreadContext(t, &c)) {
        rip = c.Rip;
        real_calls(&c, calls, sizeof(calls));
      }
      ResumeThread(t);
    }
    CloseHandle(t);
    if (!rip) continue;
    // (only the game's own threads: one whose chain has no address of the exe or of the mod is a driver's)
    if (!strstr(calls, " +") && !strstr(calls, "metronet.asi") && !(rip >= base && rip < base + game::kSizeOfImage)) {
      ++others;
      continue;
    }
    char at[40];
    if (rip >= base && rip < base + game::kSizeOfImage)
      snprintf(at, sizeof(at), "+%llX", static_cast<unsigned long long>(rip - base));
    else
      snprintf(at, sizeof(at), "%llX", static_cast<unsigned long long>(rip));
    LOGW("lab:   thread %lu%s at %s, called from:%s", te.th32ThreadID,
         te.th32ThreadID == net::lab_main_thread() ? " (the frame's)" : "", at, calls);
    ++shown;
  }
  CloseHandle(snap);
  LOGW("lab:   (and %d threads of drivers and the system)", others);
}

DWORD WINAPI script_thread(LPVOID) {
  // The engine never got to its main loop: do not leave a hidden process behind.
  int waited = 0;
  while (!engine::ready()) {
    Sleep(100);
    if (++waited > 1800) {
      LOGE("lab: the engine did not finish its start-up in 180 s, stopping the process");
      TerminateProcess(GetCurrentProcess(), 2);
    }
  }
  size_t next = 0;
  bool asked_quit = false;
  double last_done = 0;   // when the step before finished
  int waiting_from = -1;  // "?text": the log's hit count when the wait began
  double waiting_since = 0;
  for (;;) {
    double t = engine::since_ready();
    while (next < g_cfg.script.size()) {
      const Config::Step& st = g_cfg.script[next];
      const std::string& cmd = st.cmd;
      double due = st.relative ? last_done + st.at : st.at;
      if (t < due) break;
      if (cmd[0] == '?') {  // wait until the engine logs a new line with this text (120 s at most)
        const char* text = cmd.c_str() + 1;
        if (waiting_from < 0) {
          waiting_from = engine::count_log(text);
          waiting_since = t;
          LOGI("lab: waiting for the engine's line '%s'", text);
        }
        if (engine::count_log(text) > waiting_from) {
          LOGI("lab: got '%s' after %.1f s", text, t - waiting_since);
        } else if (t - waiting_since > 120) {
          LOGW("lab: no '%s' in 120 s, going on", text);
        } else {
          break;
        }
        waiting_from = -1;
      } else if (cmd[0] == '!') {
        own_command(cmd);
      } else if (cmd[0] == '>') {  // right now, on the main thread (the console's queue stands still during a movie)
        engine::on_main([c = cmd.substr(1)] { engine::command(c, false); });
      } else {
        engine::command(cmd, true);
      }
      last_done = t;
      ++next;
    }
    {
      // The main loop has not turned for 8 s (it turns even while a level loads): say where the game stands. Once.
      static bool stood = false;
      unsigned long long turned = net::lab_last_frame();
      if (!stood && !asked_quit && turned && GetTickCount64() - turned > 8000 && game::ref<void*>(game::kLevel) &&
          game::ref<int>(game::kMapState) != 2) {
        stood = true;
        dump_threads("the main loop has not turned for 8 s");
      }
    }
    if (g_cfg.lab_quit_after > 0) {
      if (!asked_quit && t >= g_cfg.lab_quit_after) {
        asked_quit = true;
        LOGI("lab: %s", net::lab_tree_lock().c_str());
        engine::command("quit", true);
      }
      if (t >= g_cfg.lab_quit_after + 15) {
        LOGW("lab: the game did not quit by itself, stopping the process");
        dump_threads("the game did not quit");
        TerminateProcess(GetCurrentProcess(), 1);
      }
    }
    Sleep(20);
  }
}

}  // namespace

void install(const Config& cfg) {
  g_cfg = cfg;
  if (!cfg.lab) return;
  LOGI("lab: test run (window off the screen, no input, own user folder '%ls')", cfg.lab_userdir.c_str());
  // out of the user's way: below-normal priority, a few cores only (each copy its own few where there is room)
  SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
  if (cfg.lab_cores > 0) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    DWORD all = si.dwNumberOfProcessors, cores = static_cast<DWORD>(cfg.lab_cores);
    if (all > cores && all <= 64) {
      DWORD groups = all / cores;
      // the last cores first (the system and the user's programs like the first ones); the socket port tells the copies apart
      DWORD slot = cfg.lab_bind_port > 0 ? static_cast<DWORD>(cfg.lab_bind_port) % groups : 0;
      DWORD first = all - cores * (1 + slot);
      DWORD_PTR mask = ((static_cast<DWORD_PTR>(1) << cores) - 1) << first;
      BOOL ok = SetProcessAffinityMask(GetCurrentProcess(), mask);
      LOGI("lab: below-normal priority, cores %lu-%lu of %lu (%s), %d frames a second at most", first, first + cores - 1, all,
           ok ? "set" : "NOT set", cfg.lab_fps);
    }
  }

  patch::import("USER32.dll", "CreateWindowExA", reinterpret_cast<void*>(&on_create_window), reinterpret_cast<void**>(&g_create_window));
  patch::import("USER32.dll", "ShowWindow", reinterpret_cast<void*>(&on_show_window), reinterpret_cast<void**>(&g_show_window));
  patch::import("USER32.dll", "SetWindowPos", reinterpret_cast<void*>(&on_set_window_pos), reinterpret_cast<void**>(&g_set_window_pos));
  patch::import("USER32.dll", "SetCursorPos", reinterpret_cast<void*>(&on_set_cursor_pos), nullptr);
  patch::import("USER32.dll", "GetCursorPos", reinterpret_cast<void*>(&on_get_cursor_pos), nullptr);
  patch::import("USER32.dll", "GetAsyncKeyState", reinterpret_cast<void*>(&on_get_async_key_state), nullptr);
  patch::import("USER32.dll", "ShowCursor", reinterpret_cast<void*>(&on_show_cursor), nullptr);
  patch::import("USER32.dll", "SetWindowsHookExA", reinterpret_cast<void*>(&on_set_windows_hook), nullptr);
  patch::import("USER32.dll", "MessageBoxA", reinterpret_cast<void*>(&on_message_box), nullptr);
  patch::import("SHELL32.dll", "SHGetSpecialFolderPathW", reinterpret_cast<void*>(&on_special_folder), reinterpret_cast<void**>(&g_special_folder));
  patch::import("ADVAPI32.dll", "RegOpenKeyExA", reinterpret_cast<void*>(&on_reg_open), nullptr);
  patch::import("ADVAPI32.dll", "RegCreateKeyExA", reinterpret_cast<void*>(&on_reg_create), nullptr);
  patch::import("KERNEL32.dll", "GetProcAddress", reinterpret_cast<void*>(&on_get_proc), reinterpret_cast<void**>(&g_get_proc));

  patch::hook("load wait begin", game::at(game::kLoadWaitBegin), reinterpret_cast<void*>(&on_load_wait_begin),
              reinterpret_cast<void**>(&g_load_wait_begin));

  InitializeCriticalSection(&g_pad_lock);
  InitializeCriticalSection(&g_shot_lock);
  if (HMODULE xinput = LoadLibraryW(L"xinput1_3.dll")) {
    if (FARPROC get_state = GetProcAddress(xinput, MAKEINTRESOURCEA(2)))
      patch::hook("XInputGetState", reinterpret_cast<uintptr_t>(get_state), reinterpret_cast<void*>(&on_xinput_get_state), nullptr);
  }
  patch::hook("single instance check", game::at(game::kAlreadyRunning), reinterpret_cast<void*>(&on_already_running), nullptr);

  SetUnhandledExceptionFilter(&on_crash);
  patch::hook_api("kernel32.dll", "SetUnhandledExceptionFilter", reinterpret_cast<void*>(&on_set_filter), reinterpret_cast<void**>(&g_set_filter));
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

  CloseHandle(CreateThread(nullptr, 0, &script_thread, nullptr, 0, nullptr));
}

}  // namespace lab
