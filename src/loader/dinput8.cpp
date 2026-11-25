// dinput8.dll next to metro.exe. The game imports one function from DirectInput 8; this file hands that
// call on to the system's dinput8.dll and, when the process starts, loads metronet.asi from its own folder.
// Nothing else: the mod itself is metronet.asi, so a new version of the mod never replaces this file.
#include <windows.h>
#include <unknwn.h>

namespace {

using CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
CreateFn g_create = nullptr;

CreateFn real_create() {
  if (g_create) return g_create;
  wchar_t path[MAX_PATH];
  UINT n = GetSystemDirectoryW(path, MAX_PATH);
  if (!n || n > MAX_PATH - 16) return nullptr;
  lstrcatW(path, L"\\dinput8.dll");
  HMODULE real = LoadLibraryW(path);
  if (real) g_create = reinterpret_cast<CreateFn>(GetProcAddress(real, "DirectInput8Create"));
  return g_create;
}

}  // namespace

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD version, REFIID iid, LPVOID* out, LPUNKNOWN outer) {
  CreateFn f = real_create();
  return f ? f(inst, version, iid, out, outer) : E_FAIL;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(self);
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    while (n && path[n - 1] != L'\\' && path[n - 1] != L'/') --n;
    if (n && n < MAX_PATH - 16) {
      path[n] = 0;
      lstrcatW(path, L"metronet.asi");
      if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) LoadLibraryW(path);
    }
  }
  return TRUE;
}
