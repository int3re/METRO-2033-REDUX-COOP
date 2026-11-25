// metronet.asi: loaded into metro.exe by the dinput8.dll next to it, before the game's own code starts.
// The exe is not packed, so the patches go in right here; they are only pointer and code writes, and the
// one thread the lab starts begins to run after the loader is done.
#include <windows.h>

#include <string>

#include "common/log.h"
#include "common/version.h"
#include "mod/config.h"
#include "mod/engine.h"
#include "mod/game.h"
#include "mod/hud.h"
#include "mod/lab.h"
#include "mod/menu.h"
#include "mod/net.h"
#include "mod/session.h"

namespace {

Config g_cfg;

bool running_in_game() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  const wchar_t* name = wcsrchr(path, L'\\');
  return _wcsicmp(name ? name + 1 : path, L"metro.exe") == 0;
}

// The exe this mod was made for: the GOG build of Metro 2033 Redux.
bool known_exe() {
  auto base = reinterpret_cast<const uint8_t*>(game::base());
  auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return false;
  LOGI("metro.exe: timestamp %08lX, image size %lX", nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
  return nt->FileHeader.TimeDateStamp == game::kTimeDateStamp && nt->OptionalHeader.SizeOfImage == game::kSizeOfImage;
}

void init() {
  std::wstring dir = module_dir();
  bool have_ini = load_config(dir + L"metronet.ini", g_cfg);
  tlog::open(dir + L"metronet.log", g_cfg.log_level, false);
  LOGI("metronet " METRONET_VERSION " in %ls", dir.c_str());
  if (!have_ini) LOGW("metronet.ini not found next to metro.exe, using defaults");
  if (!known_exe()) {
    LOGE("this metro.exe is not the build the mod knows: nothing is patched, the game runs as it is");
    return;
  }
  engine::install(g_cfg);
  net::install(g_cfg);
  session::install(g_cfg);
  hud::install(g_cfg);
  menu::install(g_cfg);
  lab::install(g_cfg);
  LOGI("metronet ready");
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(self);
    if (running_in_game()) init();
  }
  return TRUE;
}
