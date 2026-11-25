#include "mod/patch.h"

#include <windows.h>

#include <MinHook.h>

#include <string>

#include "common/log.h"

namespace patch {
namespace {

struct Image {
  uintptr_t begin = 0, end = 0;
};

const Image& image() {
  static Image img = [] {
    Image i;
    auto base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    i.begin = reinterpret_cast<uintptr_t>(base);
    i.end = i.begin + nt->OptionalHeader.SizeOfImage;
    return i;
  }();
  return img;
}

std::string hex(const void* p, size_t n) {
  static const char* d = "0123456789ABCDEF";
  std::string s;
  auto b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; ++i) {
    if (i) s += ' ';
    s += d[b[i] >> 4];
    s += d[b[i] & 15];
  }
  return s;
}

// "jmp [rip+0]; dq target" in memory the exe's code can reach with a rel32 call (the mod itself is loaded
// anywhere in the 64-bit address space).
uint8_t* near_pad(void* target) {
  static uint8_t* page = nullptr;
  static size_t used = 0;
  const Image& img = image();
  if (!page) {
    uintptr_t low = img.begin > 0x60000000u ? img.begin - 0x60000000u : 0x10000u;
    for (uintptr_t a = (img.begin - 0x10000) & ~uintptr_t(0xFFFF); !page && a >= low; a -= 0x10000)
      page = static_cast<uint8_t*>(
          VirtualAlloc(reinterpret_cast<void*>(a), 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    for (uintptr_t a = (img.end + 0xFFFF) & ~uintptr_t(0xFFFF); !page && a < img.end + 0x60000000u; a += 0x10000)
      page = static_cast<uint8_t*>(
          VirtualAlloc(reinterpret_cast<void*>(a), 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  }
  if (!page || used + 14 > 0x1000) return nullptr;
  uint8_t* pad = page + used;
  used += 16;
  const uint8_t jump[6] = {0xFF, 0x25, 0, 0, 0, 0};
  std::memcpy(pad, jump, 6);
  std::memcpy(pad + 6, &target, 8);
  FlushInstructionCache(GetCurrentProcess(), pad, 14);
  return pad;
}

}  // namespace

bool in_image(uintptr_t addr, size_t size) {
  const Image& img = image();
  return addr >= img.begin && addr + size <= img.end;
}

bool bytes_are(uintptr_t addr, const void* expect, size_t n) {
  return in_image(addr, n) && std::memcmp(reinterpret_cast<const void*>(addr), expect, n) == 0;
}

bool write(uintptr_t addr, const void* bytes, size_t n) {
  if (!in_image(addr, n)) return false;
  DWORD old;
  void* p = reinterpret_cast<void*>(addr);
  if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old)) return false;
  std::memcpy(p, bytes, n);
  VirtualProtect(p, n, old, &old);
  FlushInstructionCache(GetCurrentProcess(), p, n);
  return true;
}

bool replace(const char* name, uintptr_t addr, const void* expect, const void* bytes, size_t n) {
  if (!bytes_are(addr, expect, n)) {
    LOGW("patch %s at %p: unexpected bytes (%s), skipped", name, reinterpret_cast<void*>(addr),
         in_image(addr, n) ? hex(reinterpret_cast<const void*>(addr), n).c_str() : "outside the exe");
    return false;
  }
  if (!write(addr, bytes, n)) {
    LOGE("patch %s at %p: write failed", name, reinterpret_cast<void*>(addr));
    return false;
  }
  LOGI("patched %s at %p", name, reinterpret_cast<void*>(addr));
  return true;
}

bool call_site(const char* name, uintptr_t addr, const void* expect, size_t n, void* target) {
  uint8_t code[16];
  if (n < 5 || n > sizeof(code)) return false;
  if (!bytes_are(addr, expect, n)) return replace(name, addr, expect, expect, n);  // logs the unexpected bytes
  uint8_t* pad = near_pad(target);
  if (!pad) {
    LOGE("patch %s at %p: no memory near the exe", name, reinterpret_cast<void*>(addr));
    return false;
  }
  std::memset(code, 0x90, sizeof(code));
  code[0] = 0xE8;
  int32_t rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(pad) - static_cast<intptr_t>(addr + 5));
  std::memcpy(code + 1, &rel, 4);
  return replace(name, addr, expect, code, n);
}

bool hook(const char* name, uintptr_t target, void* detour, void** original) {
  static bool initialized = false;
  if (!initialized) {
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
      LOGE("MH_Initialize: %s", MH_StatusToString(st));
      return false;
    }
    initialized = true;
  }
  void* t = reinterpret_cast<void*>(target);
  MH_STATUS st = MH_CreateHook(t, detour, original);
  if (st == MH_OK) st = MH_EnableHook(t);
  if (st != MH_OK) {
    LOGE("hook %s at %p: %s", name, t, MH_StatusToString(st));
    return false;
  }
  LOGI("hooked %s at %p", name, t);
  return true;
}

bool hook_api(const char* dll, const char* func, void* detour, void** original) {
  HMODULE m = GetModuleHandleA(dll);
  if (!m) m = LoadLibraryA(dll);
  FARPROC p = m ? GetProcAddress(m, func) : nullptr;
  if (!p) {
    LOGE("hook %s!%s: not found", dll, func);
    return false;
  }
  std::string name = std::string(dll) + "!" + func;
  return hook(name.c_str(), reinterpret_cast<uintptr_t>(p), detour, original);
}

bool import(const char* dll, const char* func, void* hook, void** original) {
  auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
  auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return false;
  for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
    if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dll) != 0) continue;
    auto names = reinterpret_cast<IMAGE_THUNK_DATA64*>(
        base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
    auto slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++slots) {
      if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
      auto by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
      if (strcmp(reinterpret_cast<const char*>(by_name->Name), func) != 0) continue;
      if (original) *original = reinterpret_cast<void*>(static_cast<uintptr_t>(slots->u1.Function));
      uintptr_t target = reinterpret_cast<uintptr_t>(hook);
      bool ok = write(reinterpret_cast<uintptr_t>(&slots->u1.Function), &target, sizeof(target));
      if (ok)
        LOGI("import %s!%s redirected", dll, func);
      else
        LOGE("import %s!%s: write failed", dll, func);
      return ok;
    }
  }
  LOGW("import %s!%s not found", dll, func);
  return false;
}

bool slot(const char* name, uintptr_t addr, uintptr_t expect, void* value) {
  uintptr_t e = expect;
  uintptr_t v = reinterpret_cast<uintptr_t>(value);
  return replace(name, addr, &e, &v, sizeof(v));
}

}  // namespace patch
