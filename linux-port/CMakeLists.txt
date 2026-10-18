cmake_minimum_required(VERSION 3.22)  # Ubuntu 22.04 stock cmake
project(OpenCrownLinux C CXX)
set(OPENCROWN_VERSION "1.0.1")  # Linux build; versioned separately from the Windows build in ../port
# Updater end-to-end tests build a fake newer release in a separate build dir (packaging/make-appimage.sh, OC_VERSION).
if(OPENCROWN_VERSION_OVERRIDE)
  set(OPENCROWN_VERSION "${OPENCROWN_VERSION_OVERRIDE}")
endif()

# Linux / Steam Deck build. Separate from the Windows tree (../port): own sources, own build, no shared files.
# Targets are added here as each part of src/ is ported (see README.md, Phase 1).
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "linux-port builds on Linux only (WSL2 works); the Windows build is ../port")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra)
# Everything PIE: dcp_run must not load at 0x400000 itself, that is where the eboot wants to go (loader/elf.cpp).
include(CheckPIESupported)
check_pie_supported()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# File-format readers. Operate only on user-supplied game data; no game data lives in this tree.
add_library(dcp_formats STATIC
  src/formats/utf_table.cpp
  src/formats/crilayla.cpp
  src/formats/cpk.cpp
  src/formats/afs2.cpp
  src/formats/hca_header.cpp
  src/formats/eboot_image.cpp
  src/formats/hca_decoder.cpp
  src/formats/acb.cpp
  src/formats/gnm_detile.cpp
  src/formats/ftx.cpp
  src/formats/dds.cpp
  src/formats/mbs.cpp
  src/formats/mcb.cpp
  src/formats/bsb.cpp)
target_include_directories(dcp_formats PUBLIC src)

# Dump tools (CLI).
foreach(tool dcp_cpk dcp_tex dcp_audio dcp_sprite)
  add_executable(${tool} tools/${tool}/main.cpp)
  target_link_libraries(${tool} PRIVATE dcp_formats)
endforeach()

# Loader: maps the user's eboot into this process, relocates, binds imports to shims / module exports / stubs.
add_library(dcp_loader STATIC src/loader/nid.cpp src/loader/elf.cpp src/loader/hle.cpp)
target_include_directories(dcp_loader PUBLIC src)

include(FetchContent)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)  # third-party projects below declare old minimums

# FreeType (FTL / GPLv2, we use it under the FTL) for the libSceFont shim. Fetched at configure time; no optional deps.
# Offline / if the download fails: -DFETCHCONTENT_SOURCE_DIR_FREETYPE=<unpacked freetype source>.
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
FetchContent_Declare(freetype
  URL https://github.com/freetype/freetype/archive/refs/tags/VER-2-13-3.tar.gz)
FetchContent_MakeAvailable(freetype)

# miniupnpc (BSD-3) for UPnP IGD port mapping of the P2P UDP port (net/upnp.*, best effort). Pinned release tag;
# offline: -DFETCHCONTENT_SOURCE_DIR_MINIUPNPC=<unpacked miniupnp repo root> (the CMake project is in miniupnpc/).
set(UPNPC_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(UPNPC_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(UPNPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UPNPC_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
set(UPNPC_NO_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(miniupnpc
  URL https://github.com/miniupnp/miniupnp/archive/refs/tags/miniupnpc_2_2_8.tar.gz
  SOURCE_SUBDIR miniupnpc)
FetchContent_MakeAvailable(miniupnpc)
target_compile_options(libminiupnpc-static PRIVATE -w)  # third-party

# mbedTLS 3.6 LTS (Apache-2.0) for the wss:// lobby link (net/tls.cpp) and SHA-256 (net/identity.cpp), static.
# The release tarball carries the generated sources (no Python needed). Offline: -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=<dir>.
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(GEN_FILES OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(mbedtls
  URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2)
FetchContent_MakeAvailable(mbedtls)
target_compile_options(freetype PRIVATE -w)  # third-party
foreach(t mbedtls mbedx509 mbedcrypto)
  target_compile_options(${t} PRIVATE -w)  # third-party
endforeach()

# Guest texture write tracking (render/texwatch.h): used by the file shim (dcp_hle) and the GL texture cache (dcp_render).
add_library(dcp_texwatch STATIC src/render/texwatch.cpp)
target_include_directories(dcp_texwatch PUBLIC src)

# P2P transport under the libSceRudp shim (Carrier interface + own reliable UDP framing); host C++, testable alone.
add_library(dcp_net STATIC src/net/net.cpp src/net/udp_carrier.cpp src/net/stun.cpp src/net/upnp.cpp src/net/rudp.cpp
  src/net/json.cpp src/net/ws.cpp src/net/lobby_client.cpp src/net/lobby_server.cpp src/net/tls.cpp
  src/net/online_check.cpp src/net/identity.cpp src/net/friends.cpp src/net/bug_report.cpp
  src/net/http.cpp src/net/update.cpp)
target_include_directories(dcp_net PUBLIC src)
target_link_libraries(dcp_net PUBLIC Threads::Threads PRIVATE libminiupnpc-static mbedtls mbedx509 mbedcrypto)

# HLE shims for the PS4 system libraries (kernel + services). -fno-exceptions: nothing may unwind through game frames.
add_library(dcp_hle STATIC src/hle/kernel/pure.cpp src/hle/kernel/memory.cpp src/hle/kernel/thread.cpp
  src/hle/kernel/file.cpp src/hle/kernel/misc.cpp src/hle/services.cpp src/hle/savedata.cpp src/hle/dialogs.cpp
  src/hle/pad.cpp src/hle/audio.cpp src/hle/video.cpp src/hle/gnm.cpp src/hle/font.cpp src/hle/net.cpp src/hle/np.cpp
  src/hle/matching2.cpp src/hle/net_test.cpp src/hle/skip_offers.cpp src/render/capture.cpp)
target_include_directories(dcp_hle PUBLIC src)
target_compile_options(dcp_hle PRIVATE -fno-exceptions)
target_link_libraries(dcp_hle PUBLIC dcp_loader freetype dcp_texwatch dcp_net Threads::Threads)

# SDL3 (zlib) for the window, GL context, audio and input of dcp_run --render and dcp_viewer: same release as the
# Windows build (3.4.18), built static so the binaries (and the future AppImage) need no system SDL3. SDL still loads
# X11 / Wayland / libdecor / PulseAudio / PipeWire / ALSA / libudev / dbus / libGL at run time (dlopen), so only their
# dev headers are needed to build: see build.sh. GL entry points come through SDL_GL_GetProcAddress (no -lGL).
# Offline: -DFETCHCONTENT_SOURCE_DIR_SDL3=<unpacked SDL3 source>. -DDCP_RENDER=OFF builds a headless dcp_run only.
option(DCP_RENDER "dcp_run --render and dcp_viewer (fetches SDL3)" ON)
if(DCP_RENDER)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
  set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz)
  FetchContent_MakeAvailable(SDL3)
  target_compile_options(SDL3-static PRIVATE -w)  # third-party

  # Stage 4 renderer (GL side): shared by dcp_viewer and dcp_run --render. GL entry points come from the caller's loader.
  add_library(dcp_render STATIC src/render/gl.cpp src/render/png.cpp src/render/backend.cpp src/render/prompts.cpp)
  target_include_directories(dcp_render PUBLIC src)
  target_link_libraries(dcp_render PUBLIC dcp_formats dcp_texwatch freetype)
endif()

# dcp_run: --render (GL window / screenshots) with DCP_RENDER, headless layer-3 behaviour otherwise.
add_executable(dcp_run tools/dcp_run/main.cpp)
target_link_libraries(dcp_run PRIVATE dcp_loader dcp_hle ${CMAKE_DL_LIBS})
if(DCP_RENDER)
  target_sources(dcp_run PRIVATE tools/dcp_run/render_loop.cpp tools/dcp_run/sdl_audio.cpp tools/dcp_run/sdl_input.cpp)
  target_compile_definitions(dcp_run PRIVATE DCP_HAVE_RENDER)
  target_link_libraries(dcp_run PRIVATE dcp_render SDL3::SDL3-static)

  # Asset viewer (CPK browser, textures, sprite animations).
  add_executable(dcp_viewer tools/dcp_viewer/main.cpp tools/dcp_viewer/anim.cpp)
  target_link_libraries(dcp_viewer PRIVATE dcp_formats dcp_render SDL3::SDL3-static)

  # End-user launcher (GUI): dump folder, controls (input.ini), display options, online, starts dcp_run next to it.
  # Dear ImGui (MIT), same release as the Windows build; offline: -DFETCHCONTENT_SOURCE_DIR_IMGUI=<unpacked source>.
  # No -lGL: imgui_impl_opengl3's own loader dlopens libGL.so.1, the launcher's few GL calls go through SDL_GL_GetProcAddress.
  FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b.tar.gz)
  FetchContent_MakeAvailable(imgui)
  add_library(dcp_imgui STATIC ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
  target_include_directories(dcp_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  target_compile_options(dcp_imgui PRIVATE -w)  # third-party
  target_link_libraries(dcp_imgui PUBLIC SDL3::SDL3-static ${CMAKE_DL_LIBS})

  add_executable(opencrown_launcher tools/launcher/main.cpp)
  target_link_libraries(opencrown_launcher PRIVATE dcp_imgui dcp_net)
  set_target_properties(opencrown_launcher PROPERTIES OUTPUT_NAME "opencrown-launcher")
  target_compile_definitions(opencrown_launcher PRIVATE OPENCROWN_VERSION="${OPENCROWN_VERSION}")
endif()

# Mock lobby server (notes/map/lobby_protocol.md) for local multiplayer tests.
add_executable(dcp_lobby tools/dcp_lobby/main.cpp)
target_link_libraries(dcp_lobby PRIVATE dcp_net)

enable_testing()
add_executable(test_formats tests/test_formats.cpp)
target_link_libraries(test_formats PRIVATE dcp_formats)
add_test(NAME formats COMMAND test_formats)
add_executable(test_loader tests/test_loader.cpp)
target_link_libraries(test_loader PRIVATE dcp_loader)
add_test(NAME loader COMMAND test_loader)
add_executable(test_hle tests/test_hle.cpp)
target_link_libraries(test_hle PRIVATE dcp_hle)
add_test(NAME hle COMMAND test_hle)
add_executable(test_net tests/test_net.cpp)  # loopback only
target_link_libraries(test_net PRIVATE dcp_net)
add_test(NAME net COMMAND test_net)
add_executable(test_lobby tests/test_lobby.cpp)  # against the in-process mock lobby (loopback) unless $DCP_LOBBY_TEST_URL
target_link_libraries(test_lobby PRIVATE dcp_net)
add_test(NAME lobby COMMAND test_lobby)
add_executable(test_touch tests/test_touch.cpp)  # touch screen / Steam Deck input logic (header only)
add_test(NAME touch COMMAND test_touch)
add_executable(test_texwatch tests/test_texwatch.cpp)  # concurrent faults on one protected page
target_link_libraries(test_texwatch PRIVATE dcp_texwatch)
add_test(NAME texwatch COMMAND test_texwatch)
add_executable(test_update tests/test_update.cpp)  # launcher self-updater: logic + HTTP client on loopback only
target_link_libraries(test_update PRIVATE dcp_net)
add_test(NAME update COMMAND test_update)
