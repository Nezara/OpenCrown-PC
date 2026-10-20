cmake_minimum_required(VERSION 3.24)
project(DragonsCrownPort C CXX)
set(OPENCROWN_VERSION "1.0.3")  # shown in the launcher, compared with GitHub releases by its updater; bump per release
# Test builds only (updater end-to-end test): -DOPENCROWN_VERSION_OVERRIDE=9.9.9. Never for a release.
set(OPENCROWN_VERSION_OVERRIDE "" CACHE STRING "Override OPENCROWN_VERSION (updater tests only)")
if(OPENCROWN_VERSION_OVERRIDE)
  set(OPENCROWN_VERSION "${OPENCROWN_VERSION_OVERRIDE}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra)
endif()
if(MINGW)
  add_link_options(-static)  # self-contained tools; no MinGW runtime DLLs needed on PATH
endif()

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

add_executable(dcp_cpk tools/dcp_cpk/main.cpp)
target_link_libraries(dcp_cpk PRIVATE dcp_formats)

add_executable(dcp_tex tools/dcp_tex/main.cpp)
target_link_libraries(dcp_tex PRIVATE dcp_formats)

add_executable(dcp_audio tools/dcp_audio/main.cpp)
target_link_libraries(dcp_audio PRIVATE dcp_formats)

add_executable(dcp_sprite tools/dcp_sprite/main.cpp)
target_link_libraries(dcp_sprite PRIVATE dcp_formats)

# Stage 3 loader prototype (Windows x64): maps the user's eboot into this process, relocates, binds imports to stubs.
add_library(dcp_loader STATIC src/loader/nid.cpp src/loader/elf.cpp src/loader/hle.cpp)
target_include_directories(dcp_loader PUBLIC src)

# FreeType (FTL / GPLv2, we use it under the FTL) for the libSceFont shim. Fetched at configure time; no optional deps.
# Offline / if the download fails: -DFETCHCONTENT_SOURCE_DIR_FREETYPE=<unpacked freetype source>.
include(FetchContent)
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
FetchContent_Declare(freetype
  URL https://github.com/freetype/freetype/archive/refs/tags/VER-2-13-3.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP ON)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(freetype)
# FreeType's version resource breaks windres when the build path has spaces; it is only metadata, drop it.
get_target_property(_ft_src freetype SOURCES)
list(FILTER _ft_src EXCLUDE REGEX "\.rc$")
set_target_properties(freetype PROPERTIES SOURCES "${_ft_src}")

# Guest texture write tracking (render/texwatch.h): used by the file shim (dcp_hle) and the GL texture cache (dcp_render).
add_library(dcp_texwatch STATIC src/render/texwatch.cpp)
target_include_directories(dcp_texwatch PUBLIC src)

# miniupnpc (BSD-3) for UPnP IGD port mapping of the P2P UDP port (net/upnp.*, best effort). Pinned release tag;
# offline: -DFETCHCONTENT_SOURCE_DIR_MINIUPNPC=<unpacked miniupnp repo root> (the CMake project is in miniupnpc/).
set(UPNPC_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(UPNPC_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(UPNPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UPNPC_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
set(UPNPC_NO_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(miniupnpc
  URL https://github.com/miniupnp/miniupnp/archive/refs/tags/miniupnpc_2_2_8.tar.gz
  SOURCE_SUBDIR miniupnpc
  DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_MakeAvailable(miniupnpc)
target_compile_options(libminiupnpc-static PRIVATE -w)  # third-party

# P2P transport under the libSceRudp shim (Carrier interface + own reliable UDP framing); host C++, testable alone.
add_library(dcp_net STATIC src/net/net.cpp src/net/udp_carrier.cpp src/net/stun.cpp src/net/upnp.cpp src/net/rudp.cpp src/net/json.cpp src/net/ws.cpp
  src/net/lobby_client.cpp src/net/lobby_server.cpp src/net/tls.cpp src/net/online_check.cpp src/net/identity.cpp
  src/net/friends.cpp src/net/bug_report.cpp)
target_include_directories(dcp_net PUBLIC src)
target_link_libraries(dcp_net PUBLIC ws2_32 secur32 bcrypt PRIVATE libminiupnpc-static)

# HLE shims for the PS4 system libraries (kernel + services). -fno-exceptions keeps SysV shims free of EH cleanups.
add_library(dcp_hle STATIC src/hle/kernel/pure.cpp src/hle/kernel/memory.cpp src/hle/kernel/thread.cpp
  src/hle/kernel/file.cpp src/hle/kernel/misc.cpp src/hle/services.cpp src/hle/savedata.cpp src/hle/dialogs.cpp
  src/hle/pad.cpp src/hle/audio.cpp src/hle/video.cpp src/hle/gnm.cpp src/hle/font.cpp src/hle/net.cpp src/hle/np.cpp
  src/hle/matching2.cpp src/hle/net_test.cpp src/hle/net_trace.cpp src/hle/skip_offers.cpp src/render/capture.cpp)
target_include_directories(dcp_hle PUBLIC src)
target_compile_options(dcp_hle PRIVATE -fno-exceptions)
target_link_libraries(dcp_hle PUBLIC dcp_loader winmm freetype dcp_texwatch dcp_net)

# Optional viewer (SDL3 + OpenGL 3.3). Point DCP_SDL3_DIR at an SDL3 MinGW dev tree (x86_64-w64-mingw32); skipped if absent.
set(DCP_SDL3_DIR "" CACHE PATH "SDL3 prefix (contains lib/cmake/SDL3)")
find_package(SDL3 CONFIG QUIET HINTS "${DCP_SDL3_DIR}" PATHS "${DCP_SDL3_DIR}/lib/cmake/SDL3")
# Stage 4 renderer (GL side): shared by dcp_viewer and dcp_run --render. GL entry points come from the caller's loader.
add_library(dcp_render STATIC src/render/gl.cpp src/render/png.cpp src/render/backend.cpp src/render/prompts.cpp)
target_include_directories(dcp_render PUBLIC src)
target_link_libraries(dcp_render PUBLIC dcp_formats dcp_texwatch freetype)

# dcp_run: --render (GL window / screenshots) only when SDL3 is available; headless layer-3 behaviour otherwise.
add_executable(dcp_run tools/dcp_run/main.cpp)
target_link_libraries(dcp_run PRIVATE dcp_loader dcp_hle)
if(SDL3_FOUND)
  target_sources(dcp_run PRIVATE tools/dcp_run/render_loop.cpp tools/dcp_run/sdl_audio.cpp tools/dcp_run/sdl_input.cpp)
  target_compile_definitions(dcp_run PRIVATE DCP_HAVE_RENDER)
  target_link_libraries(dcp_run PRIVATE dcp_render SDL3::SDL3 ole32)
  add_custom_command(TARGET dcp_run POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:dcp_run>)
endif()

if(SDL3_FOUND)
  add_executable(dcp_viewer tools/dcp_viewer/main.cpp tools/dcp_viewer/anim.cpp)
  target_link_libraries(dcp_viewer PRIVATE dcp_formats dcp_render SDL3::SDL3)
  add_custom_command(TARGET dcp_viewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:dcp_viewer>)
else()
  message(STATUS "SDL3 not found (set DCP_SDL3_DIR); dcp_viewer skipped")
endif()

# Launcher self-updater (tools/launcher/update.h): GitHub releases check, verified download, zip install. No SDL.
add_library(dcp_update STATIC tools/launcher/update.cpp tools/launcher/update_win.cpp)
target_link_libraries(dcp_update PUBLIC dcp_net winhttp bcrypt)

# End-user launcher (GUI, no console): dump folder, controls (input.ini), display options, starts dcp_run.
# Dear ImGui (MIT) fetched at configure time; offline: -DFETCHCONTENT_SOURCE_DIR_IMGUI=<unpacked imgui source>.
if(SDL3_FOUND)
  FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
  FetchContent_MakeAvailable(imgui)
  add_library(dcp_imgui STATIC ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
  target_include_directories(dcp_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  target_compile_options(dcp_imgui PRIVATE -w)  # third-party
  target_link_libraries(dcp_imgui PUBLIC SDL3::SDL3 opengl32)
  # dcp_run's name-entry box (the PS4 keyboard dialog's stand-in).
  target_sources(dcp_run PRIVATE tools/dcp_run/ime_box.cpp)
  target_link_libraries(dcp_run PRIVATE dcp_imgui)

  add_executable(DragonsCrownPro WIN32 tools/launcher/main.cpp)
  target_link_libraries(DragonsCrownPro PRIVATE dcp_imgui dcp_net dcp_update shell32 windowscodecs ole32 uuid)
  set_target_properties(DragonsCrownPro PROPERTIES OUTPUT_NAME "OpenCrownPC Launcher")
  target_compile_definitions(DragonsCrownPro PRIVATE OPENCROWN_VERSION="${OPENCROWN_VERSION}")
  add_custom_command(TARGET DragonsCrownPro POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:DragonsCrownPro>)

endif()

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

add_executable(test_net tests/test_net.cpp)
target_link_libraries(test_net PRIVATE dcp_net)
add_test(NAME net COMMAND test_net)
# Mock lobby server (notes/map/lobby_protocol.md) for local multiplayer tests, and its protocol test.
add_executable(dcp_lobby tools/dcp_lobby/main.cpp)
target_link_libraries(dcp_lobby PRIVATE dcp_net)
add_executable(test_lobby tests/test_lobby.cpp)
target_link_libraries(test_lobby PRIVATE dcp_net)
add_test(NAME lobby COMMAND test_lobby)
add_executable(test_texwatch tests/test_texwatch.cpp)  # concurrent faults on one protected page
target_link_libraries(test_texwatch PRIVATE dcp_texwatch)
add_test(NAME texwatch COMMAND test_texwatch)
add_executable(test_updater tests/test_updater.cpp)  # launcher updater logic + zip install, no network
target_link_libraries(test_updater PRIVATE dcp_update)
add_test(NAME updater COMMAND test_updater)
