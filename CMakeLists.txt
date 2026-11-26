cmake_minimum_required(VERSION 3.20)
project(metro_netplay C CXX)

# metro.exe (Metro 2033 Redux) is a 64-bit process: everything that goes into it must be x64.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Configure with -A x64: metro.exe is a 64-bit process")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: players do not need the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

# --- third_party: MinHook (x64 hooks) ----------------------------------------
add_library(minhook STATIC
  third_party/minhook/src/buffer.c
  third_party/minhook/src/hook.c
  third_party/minhook/src/trampoline.c
  third_party/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC third_party/minhook/include)

set(OUR_DEFS WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS= _WINSOCK_DEPRECATED_NO_WARNINGS UNICODE _UNICODE)
set(OUR_OPTS /W4 /permissive- /utf-8)

# --- dinput8.dll: the loader. metro.exe imports DirectInput8Create from it ----------------------------
add_library(loader SHARED src/loader/dinput8.cpp src/loader/dinput8.def)
target_compile_definitions(loader PRIVATE ${OUR_DEFS})
target_compile_options(loader PRIVATE ${OUR_OPTS})
set_target_properties(loader PROPERTIES PREFIX "" OUTPUT_NAME "dinput8")

# --- metronet.asi: the mod ------------------------------------------------------------------------------
add_library(metronet MODULE
  src/common/log.cpp
  src/mod/dllmain.cpp
  src/mod/config.cpp
  src/mod/patch.cpp
  src/mod/engine.cpp
  src/mod/net.cpp
  src/mod/link.cpp
  src/mod/session.cpp
  src/mod/steamnet.cpp
  src/mod/steam_mock.cpp
  src/mod/players.cpp
  src/mod/hud.cpp
  src/mod/menu.cpp
  src/mod/revive.cpp
  src/mod/motions.cpp
  src/mod/loot.cpp
  src/mod/physics.cpp
  src/mod/screenshot.cpp
  src/mod/window.cpp
  src/mod/netmenu.cpp
  src/mod/lab.cpp)
target_include_directories(metronet PRIVATE src)
target_compile_definitions(metronet PRIVATE ${OUR_DEFS})
target_compile_options(metronet PRIVATE ${OUR_OPTS})
target_link_libraries(metronet PRIVATE minhook ws2_32 user32 shell32 winmm dxguid)

# --- optional: Steam (the Steamworks SDK is never part of the repo) -------------------------------------------
#   cmake -B build -A x64 -DSTEAMWORKS_SDK_DIR=C:/sdk/steamworks_sdk/sdk
# Only the SDK's headers (structures, constants) and its redistributable steam_api64.dll are used. The DLL goes
# into the game folder under another name: the game has a steam_api64.dll of its own there (GOG's empty one).
set(STEAMWORKS_SDK_DIR "${CMAKE_SOURCE_DIR}/../steamworks_sdk_165/sdk" CACHE PATH "Steamworks SDK (public/steam, redistributable_bin)")
set(STEAM_API_DLL ${STEAMWORKS_SDK_DIR}/redistributable_bin/win64/steam_api64.dll)
if(EXISTS ${STEAMWORKS_SDK_DIR}/public/steam/isteamnetworkingmessages.h AND EXISTS ${STEAM_API_DLL})
  message(STATUS "Steam: ON (${STEAMWORKS_SDK_DIR})")
  target_sources(metronet PRIVATE src/mod/steam_real.cpp)
  target_include_directories(metronet SYSTEM PRIVATE ${STEAMWORKS_SDK_DIR}/public)
  set(METRONET_STEAM ON)
else()
  message(WARNING "Steam: OFF (no Steamworks SDK at ${STEAMWORKS_SDK_DIR}); the game goes by IP address only")
  target_sources(metronet PRIVATE src/mod/steam_stub.cpp)
  set(METRONET_STEAM OFF)
endif()
set_target_properties(metronet PROPERTIES PREFIX "" SUFFIX ".asi" OUTPUT_NAME "metronet")

# --- dist: what goes into the game folder, in build/dist/<config> -----------------------------------------
set(DIST ${CMAKE_BINARY_DIR}/dist/$<CONFIG>)
add_custom_command(TARGET metronet POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${DIST}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:metronet> ${DIST}/
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:loader> ${DIST}/
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/dist/metronet.ini ${DIST}/
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/dist/README.md ${DIST}/metronet-README.md)
if(METRONET_STEAM)
  add_custom_command(TARGET metronet POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${STEAM_API_DLL} ${DIST}/metronet_steam_api64.dll)
endif()
add_dependencies(metronet loader)

# --- metronet-launcher.exe: the one file a player gets -------------------------------------------------------
# It carries the mod inside (the files of build/dist/<config>, as resources): finds the game, puts the mod in and
# takes it out, asks GitHub for a newer release - a newer launcher with a newer mod inside - and starts the game.
# Direct2D + DirectWrite + WinHTTP and the fonts Windows has: nothing but Windows.
enable_language(RC)

# The mod's files as the launcher's resource script names them: build/payload, a copy of build/dist/<config>
# (a resource script cannot name a folder that depends on the configuration).
set(PAYLOAD_DIR ${CMAKE_BINARY_DIR}/payload)
set(PAYLOAD_FILES ${PAYLOAD_DIR}/dinput8.dll ${PAYLOAD_DIR}/metronet.asi ${PAYLOAD_DIR}/metronet.ini
                  ${PAYLOAD_DIR}/metronet-README.md)
set(PKG_STEAM_RC "")
set(PKG_STEAM_INDEX "")
set(PAYLOAD_STEAM_COPY "")
if(METRONET_STEAM)
  list(APPEND PAYLOAD_FILES ${PAYLOAD_DIR}/metronet_steam_api64.dll)
  set(PKG_STEAM_RC "303 RCDATA \"${PAYLOAD_DIR}/metronet_steam_api64.dll\"")
  set(PKG_STEAM_INDEX "file metronet_steam_api64.dll 303\n")
  set(PAYLOAD_STEAM_COPY COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DIST}/metronet_steam_api64.dll ${PAYLOAD_DIR}/)
endif()
add_custom_target(mod_payload
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PAYLOAD_DIR}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DIST}/dinput8.dll ${PAYLOAD_DIR}/
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DIST}/metronet.asi ${PAYLOAD_DIR}/
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DIST}/metronet.ini ${PAYLOAD_DIR}/
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DIST}/metronet-README.md ${PAYLOAD_DIR}/
  ${PAYLOAD_STEAM_COPY}
  BYPRODUCTS ${PAYLOAD_FILES})
add_dependencies(mod_payload metronet)

# What the package inside says of itself: the mod's version and protocol, the launcher's own version; and the
# words of the release for the launcher's news (dist/release.json when there is one, else the launcher's own).
set(LAUNCHER_RELEASE_JSON ${CMAKE_SOURCE_DIR}/src/launcher/release.json)
if(EXISTS ${CMAKE_SOURCE_DIR}/dist/release.json)
  set(LAUNCHER_RELEASE_JSON ${CMAKE_SOURCE_DIR}/dist/release.json)
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  src/common/version.h src/mod/link.h src/launcher/launcher.h ${LAUNCHER_RELEASE_JSON})
file(STRINGS src/common/version.h _line REGEX "define METRONET_VERSION \"")
string(REGEX REPLACE ".*\"([0-9.]+)\".*" "\\1" MOD_VERSION "${_line}")
file(STRINGS src/mod/link.h _line REGEX "kProtocol = [0-9]+")
string(REGEX REPLACE ".*kProtocol = ([0-9]+).*" "\\1" MOD_PROTOCOL "${_line}")
file(STRINGS src/launcher/launcher.h _line REGEX "kLauncherVersion.. = L\"")
string(REGEX REPLACE ".*L\"([0-9.]+)\".*" "\\1" LAUNCHER_VERSION "${_line}")
string(REPLACE "." "," LAUNCHER_VERSION_COMMAS "${LAUNCHER_VERSION}")
file(READ ${LAUNCHER_RELEASE_JSON} _notes)
if(NOT _notes MATCHES "\"version\": *\"${MOD_VERSION}\"")
  message(WARNING "${LAUNCHER_RELEASE_JSON} is not for ${MOD_VERSION}: the launcher will show no notes for the mod inside")
endif()
message(STATUS "Launcher ${LAUNCHER_VERSION} with the mod ${MOD_VERSION} (protocol ${MOD_PROTOCOL}) inside")
file(CONFIGURE OUTPUT ${CMAKE_BINARY_DIR}/package-index.txt CONTENT
"metronet-package 1
version ${MOD_VERSION}
protocol ${MOD_PROTOCOL}
launcher ${LAUNCHER_VERSION}
file dinput8.dll 301 loader
file metronet.asi 302
${PKG_STEAM_INDEX}file metronet.ini 304 keep
file metronet-README.md 305
" @ONLY NEWLINE_STYLE LF)

configure_file(src/launcher/launcher.rc.in ${CMAKE_BINARY_DIR}/launcher.rc @ONLY)
set_source_files_properties(${CMAKE_BINARY_DIR}/launcher.rc PROPERTIES OBJECT_DEPENDS
  "${CMAKE_SOURCE_DIR}/src/launcher/assets/launcher.ico;${PAYLOAD_FILES};${CMAKE_BINARY_DIR}/package-index.txt;${LAUNCHER_RELEASE_JSON}")
add_executable(metronet_launcher WIN32
  src/launcher/main.cpp
  src/launcher/view.cpp
  src/launcher/gfx.cpp
  src/launcher/core.cpp
  src/launcher/modset.cpp
  src/launcher/gamecfg.cpp
  src/launcher/package.cpp
  src/launcher/net.cpp
  src/launcher/sha256.cpp
  src/launcher/launcher.manifest
  ${CMAKE_BINARY_DIR}/launcher.rc)
target_include_directories(metronet_launcher PRIVATE src)
target_compile_definitions(metronet_launcher PRIVATE ${OUR_DEFS})
target_compile_options(metronet_launcher PRIVATE ${OUR_OPTS})
target_link_libraries(metronet_launcher PRIVATE d2d1 dwrite windowscodecs winhttp ws2_32 dwmapi ole32 shell32 advapi32 bcrypt
                                                user32 gdi32)
set_target_properties(metronet_launcher PROPERTIES OUTPUT_NAME "metronet-launcher")
add_dependencies(metronet_launcher mod_payload)
add_custom_command(TARGET metronet_launcher POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${DIST}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:metronet_launcher> ${DIST}/)
