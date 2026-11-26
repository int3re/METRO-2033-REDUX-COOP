#include "mod/screenshot.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <cstring>
#include <vector>

#include "common/log.h"
#include "mod/hud.h"
#include "mod/patch.h"

namespace screenshot {
namespace {

Config g_cfg;
std::atomic<bool> g_wanted{false};   // a picture is asked for: the next Present takes it
std::atomic<bool> g_copied{false};   // the render thread has put one into the clipboard (the notice is the main thread's)
std::atomic<uint64_t> g_last_key{0};
bool g_hooked = false, g_tried = false;
bool g_key_was_down = false;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
PresentFn g_present = nullptr;

// The frame into the clipboard: a 24-bit picture, bottom up (CF_DIB).
bool copy_frame(IDXGISwapChain* swap) {
  ID3D11Texture2D* back = nullptr;
  if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) return false;
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
  if (ok) {
    int kind = d.Format >= DXGI_FORMAT_B8G8R8A8_UNORM && d.Format <= DXGI_FORMAT_B8G8R8X8_UNORM_SRGB ? 1
               : d.Format >= DXGI_FORMAT_R10G10B10A2_TYPELESS && d.Format <= DXGI_FORMAT_R10G10B10A2_UINT ? 2 : 0;
    const unsigned row = (d.Width * 3 + 3) & ~3u;
    const size_t bytes = sizeof(BITMAPINFOHEADER) + static_cast<size_t>(row) * d.Height;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    uint8_t* out = mem ? static_cast<uint8_t*>(GlobalLock(mem)) : nullptr;
    if (out) {
      BITMAPINFOHEADER ih{};
      ih.biSize = sizeof(ih);
      ih.biWidth = static_cast<LONG>(d.Width);
      ih.biHeight = static_cast<LONG>(d.Height);
      ih.biPlanes = 1;
      ih.biBitCount = 24;
      ih.biCompression = BI_RGB;
      ih.biSizeImage = static_cast<DWORD>(static_cast<size_t>(row) * d.Height);
      memcpy(out, &ih, sizeof(ih));
      uint8_t* pixels = out + sizeof(ih);
      for (unsigned y = 0; y < d.Height; ++y) {
        const uint8_t* src = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
        uint8_t* dst = pixels + static_cast<size_t>(d.Height - 1 - y) * row;
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
      GlobalUnlock(mem);
      bool put = false;
      if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        put = SetClipboardData(CF_DIB, mem) != nullptr;
        CloseClipboard();
      }
      if (!put) GlobalFree(mem);
      ok = put;
      LOGI("screenshot: %ux%u %s", d.Width, d.Height, put ? "put into the clipboard" : "could not be put into the clipboard");
    } else {
      if (mem) GlobalFree(mem);
      ok = false;
    }
    ctx->Unmap(staging, 0);
  }
  if (staging) staging->Release();
  if (resolved) resolved->Release();
  if (ctx) ctx->Release();
  if (dev) dev->Release();
  back->Release();
  return ok;
}

HRESULT STDMETHODCALLTYPE on_present(IDXGISwapChain* self, UINT sync, UINT flags) {
  if (!(flags & DXGI_PRESENT_TEST) && g_wanted.exchange(false)) {
    if (copy_frame(self)) g_copied.store(true);
  }
  return g_present(self, sync, flags);
}

LRESULT CALLBACK plain_proc(HWND w, UINT m, WPARAM a, LPARAM b) { return DefWindowProcW(w, m, a, b); }

// The address of IDXGISwapChain::Present: a swap chain of the mod's own on a hidden window, made and let go.
void hook_present() {
  g_tried = true;
  HMODULE d3d = LoadLibraryW(L"d3d11.dll");
  using CreateFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                    const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*,
                                    ID3D11DeviceContext**);
  CreateFn create = d3d ? reinterpret_cast<CreateFn>(GetProcAddress(d3d, "D3D11CreateDeviceAndSwapChain")) : nullptr;
  if (!create) return;
  WNDCLASSW wc{};
  wc.lpfnWndProc = plain_proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"metronet_present_probe";
  RegisterClassW(&wc);
  HWND w = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 8, 8, nullptr, nullptr, wc.hInstance, nullptr);
  if (!w) return;
  DXGI_SWAP_CHAIN_DESC sd{};
  sd.BufferCount = 1;
  sd.BufferDesc.Width = 8;
  sd.BufferDesc.Height = 8;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = w;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  IDXGISwapChain* swap = nullptr;
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  HRESULT hr = E_FAIL;
  for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
    hr = create(nullptr, type, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &swap, &dev, nullptr, &ctx);
    if (SUCCEEDED(hr) && swap) break;
  }
  if (SUCCEEDED(hr) && swap) {
    void** vt = *reinterpret_cast<void***>(swap);
    g_hooked = patch::hook("IDXGISwapChain::Present (Print Screen)", reinterpret_cast<uintptr_t>(vt[8]), reinterpret_cast<void*>(&on_present),
                           reinterpret_cast<void**>(&g_present));
  } else {
    LOGW("screenshot: no swap chain of the mod's own could be made (0x%08lX): Print Screen stays the system's", static_cast<unsigned long>(hr));
  }
  if (swap) swap->Release();
  if (ctx) ctx->Release();
  if (dev) dev->Release();
  DestroyWindow(w);
  UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

}  // namespace

void install(const Config& cfg) { g_cfg = cfg; }

void key_seen() { g_last_key.store(GetTickCount64()); }

void tick() {
  if (g_cfg.lab || !g_cfg.screenshot) return;
  if (!g_tried) hook_present();
  if (!g_hooked) return;
  // The key: DirectInput's keyboard (the game's own reading), or the system's key state (Windows marks Print
  // Screen on its release only).
  uint64_t now = GetTickCount64();
  bool down = (GetAsyncKeyState(VK_SNAPSHOT) & 0x8001) != 0 || now - g_last_key.load() < 150;
  if (down && !g_key_was_down) g_wanted.store(true);
  g_key_was_down = down;
  if (g_copied.exchange(false)) hud::notice("Снимок экрана скопирован в буфер обмена", "The screenshot is in the clipboard", 3);
}

}  // namespace screenshot
