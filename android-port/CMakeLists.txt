cmake_minimum_required(VERSION 3.24)
project(OpenCrownMobile C CXX)

# OpenCrown Mobile, for the PS Vita build (PCSE00019, update 1.09): the game's ARM32 eboot runs under Dynarmic, everything it
# imports is our own code. Separate tree: never shares files with port/ or linux-port/. For now the host is Windows
# (MinGW, Dynarmic's x64 backend); the Android (NDK, arm64 backend) build comes later from the same sources.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(MINGW)
  add_link_options(-static)  # self-contained tools; no MinGW runtime DLLs needed on PATH
endif()
# Release files must not carry this PC's paths (mbedTLS and assert() put __FILE__ into the binary): the source tree,
# the build dir and every FETCHCONTENT_SOURCE_DIR_* override are mapped to neutral names.
set(DCP_PREFIX_MAPS "${CMAKE_CURRENT_SOURCE_DIR}=." "${CMAKE_BINARY_DIR}=build")
get_cmake_property(_dcp_cache CACHE_VARIABLES)
foreach(_v IN LISTS _dcp_cache)
  if(_v MATCHES "^FETCHCONTENT_SOURCE_DIR_(.+)$" AND NOT "${${_v}}" STREQUAL "")
    list(APPEND DCP_PREFIX_MAPS "${${_v}}=deps/${CMAKE_MATCH_1}")
  endif()
endforeach()
foreach(_m IN LISTS DCP_PREFIX_MAPS)
  add_compile_options("-ffile-prefix-map=${_m}")
endforeach()

include(FetchContent)

# Boost headers (BSL-1.0), needed only by Dynarmic (icl, variant). Header-only: fetched and never configured.
# Offline: -DFETCHCONTENT_SOURCE_DIR_BOOST=<unpacked boost source>.
FetchContent_Declare(boost
  URL https://github.com/boostorg/boost/releases/download/boost-1.86.0/boost-1.86.0-b2-nodocs.tar.xz
  URL_HASH SHA256=a4d99d032ab74c9c5e76eddcecc4489134282245fffa7e079c5804b92b45f51d
  SOURCE_SUBDIR no-cmake-here)
FetchContent_MakeAvailable(boost)
set(DCP_BOOST_INCLUDE "${boost_SOURCE_DIR}")
set(Boost_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake")  # our BoostConfig.cmake: Boost::boost from the headers above

# Dynarmic (0BSD), Vita3K's maintained fork, pinned. Bundled externals (fmt, mcl, xbyak, zydis, oaknut, robin-map).
# Offline: -DFETCHCONTENT_SOURCE_DIR_DYNARMIC=<unpacked dynarmic source>.
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(dynarmic
  URL https://github.com/Vita3K/dynarmic/archive/1fcd216467c889644921db949c218cf46f59ff3b.tar.gz
  URL_HASH SHA256=d35cd1c1425ff1ae4d862e63ca5f57e9e7df1251ddcdc7862c2624e1303e9338)
FetchContent_MakeAvailable(dynarmic)

# FreeType (FTL / GPLv2, used under the FTL) for ScePvf. No optional dependencies.
# Offline: -DFETCHCONTENT_SOURCE_DIR_FREETYPE=<unpacked freetype source>.
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
FetchContent_Declare(freetype
  URL https://github.com/freetype/freetype/archive/refs/tags/VER-2-13-3.tar.gz
  URL_HASH SHA256=bc5c898e4756d373e0d991bab053036c5eb2aa7c0d5c67e8662ddc6da40c4103)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(freetype)
# FreeType's version resource breaks windres when the build path has spaces; it is only metadata, drop it.
get_target_property(_ft_src freetype SOURCES)
list(FILTER _ft_src EXCLUDE REGEX "\.rc$")
set_target_properties(freetype PROPERTIES SOURCES "${_ft_src}")

# Vulkan-Headers (Apache-2.0 / MIT), header-only: the renderer loads every function at run time (no loader link).
# Offline: -DFETCHCONTENT_SOURCE_DIR_VULKAN_HEADERS=<unpacked Vulkan-Headers>.
FetchContent_Declare(vulkan_headers
  URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.3.296.tar.gz
  URL_HASH SHA256=e204e0b3c19f622d197df945737f5db913d6621830999b8578d34e80a7c90585
  SOURCE_SUBDIR no-cmake-here)
FetchContent_MakeAvailable(vulkan_headers)

# SPIRV-Headers (MIT), header-only: opcode / enum values for our own SPIR-V builder.
# Offline: -DFETCHCONTENT_SOURCE_DIR_SPIRV_HEADERS=<unpacked SPIRV-Headers>.
FetchContent_Declare(spirv_headers
  URL https://github.com/KhronosGroup/SPIRV-Headers/archive/refs/tags/vulkan-sdk-1.3.296.0.tar.gz
  URL_HASH SHA256=1423d58a1171611d5aba2bf6f8c69c72ef9c38a0aca12c3493e4fda64c9b2dc6
  SOURCE_SUBDIR no-cmake-here)
FetchContent_MakeAvailable(spirv_headers)

# SDL3 (zlib) for the window, input and Vulkan surface. Optional on the dev host: without it mobile_run is headless.
# Point DCP_SDL3_DIR at an SDL3 MinGW dev tree (x86_64-w64-mingw32).
set(DCP_SDL3_DIR "" CACHE PATH "SDL3 prefix (contains lib/cmake/SDL3)")
find_package(SDL3 CONFIG QUIET HINTS "${DCP_SDL3_DIR}" PATHS "${DCP_SDL3_DIR}/lib/cmake/SDL3")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set(DCP_WARN -Wall -Wextra)
endif()

# Our SPIR-V module builder: its own library so the dev validator tool builds without Dynarmic.
add_library(dcp_spirv STATIC src/gpu/spirv/builder.cpp)
target_include_directories(dcp_spirv PUBLIC src "${spirv_headers_SOURCE_DIR}/include")
target_compile_options(dcp_spirv PRIVATE ${DCP_WARN})

# Our USSE -> SPIR-V shader translator (the game's GXP programs), on the builder above.
add_library(dcp_usse STATIC src/gpu/usse/translator.cpp)
target_link_libraries(dcp_usse PUBLIC dcp_spirv)
target_compile_options(dcp_usse PRIVATE ${DCP_WARN})

# GXM texture decode: pure functions, no Vulkan or guest-memory dependency, so its test builds without Dynarmic.
add_library(dcp_texture STATIC src/gpu/texture.cpp)
target_include_directories(dcp_texture PUBLIC src)
target_compile_options(dcp_texture PRIVATE ${DCP_WARN})

# Runtime core: image loader, guest memory, module tables, CPU (Dynarmic), HLE dispatch + our system libraries.
add_library(mobile_core STATIC
  src/guest/image.cpp
  src/guest/memory.cpp
  src/guest/module.cpp
  src/guest/cpu.cpp
  src/guest/hle.cpp
  src/guest/guest_prof.cpp
  src/hle/kernel.cpp
  src/hle/thread.cpp
  src/hle/sync.cpp
  src/hle/ult.cpp
  src/hle/libc.cpp
  src/hle/system.cpp
  src/hle/io.cpp
  src/hle/math_rtc.cpp
  src/hle/ngs.cpp
  src/hle/audio.cpp
  src/codec/atrac9.cpp
  src/hle/display.cpp
  src/hle/frame_stats.cpp
  src/hle/gxm.cpp
  src/gpu/renderer.cpp
  src/hle/apputil.cpp
  src/hle/dialog.cpp
  src/hle/np.cpp
  src/hle/net.cpp
  src/hle/matching2.cpp
  src/hle/touch.cpp
  src/hle/pvf.cpp
  src/hle/skip_offers.cpp
  src/hle/confirm_button.cpp
  src/gpu/vulkan.cpp
  src/gpu/overlay_shaders.cpp
  src/gpu/compute_shaders.cpp)
target_include_directories(mobile_core PUBLIC src "${vulkan_headers_SOURCE_DIR}/include")
target_link_libraries(mobile_core PUBLIC dynarmic freetype dcp_spirv dcp_usse dcp_texture)
# Dynarmic IR headers for the guest profiler translation hook (src/guest/cpu.cpp) need mcl + Boost.
target_link_libraries(mobile_core PRIVATE merry::mcl)
target_include_directories(mobile_core SYSTEM PRIVATE "${DCP_BOOST_INCLUDE}")
target_compile_options(mobile_core PRIVATE ${DCP_WARN})

# miniupnpc (BSD-3) for UPnP IGD port mapping of the P2P UDP port (net/upnp.*, best effort). Pinned release tag.
# Offline: -DFETCHCONTENT_SOURCE_DIR_MINIUPNPC=<unpacked miniupnp repo root> (the CMake project is in miniupnpc/).
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

# mbedTLS 3.6 LTS (Apache-2.0) for the wss:// lobby link (net/tls.cpp) and SHA-256 (net/identity.cpp), static, on both
# hosts. The release tarball carries the generated sources (no Python needed).
# Offline: -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=<unpacked mbedtls-3.6.2>.
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
foreach(t mbedtls mbedx509 mbedcrypto)
  target_compile_options(${t} PRIVATE -w)  # third-party
endforeach()

# Online layer (src/net, own copy of linux-port's): lobby client (WebSocket + TLS), UDP carrier, STUN, UPnP, Rudp,
# identity, friends, local mock lobby. Winsock on the Windows dev host, BSD sockets on Android (net/sock.h). The game's
# SceNet / SceNpMatching2 / SceLibRudp HLE (src/hle/net.cpp, matching2.cpp) runs on it.
find_package(Threads REQUIRED)
add_library(dcp_net STATIC src/net/sock.cpp src/net/net.cpp src/net/udp_carrier.cpp src/net/stun.cpp src/net/upnp.cpp
  src/net/rudp.cpp src/net/json.cpp src/net/ws.cpp src/net/lobby_client.cpp src/net/lobby_server.cpp src/net/tls.cpp
  src/net/online_check.cpp src/net/identity.cpp src/net/friends.cpp src/net/bug_report.cpp src/net/chat.cpp)
target_include_directories(dcp_net PUBLIC src)
target_compile_options(dcp_net PRIVATE ${DCP_WARN})
target_link_libraries(dcp_net PUBLIC Threads::Threads PRIVATE libminiupnpc-static mbedtls mbedx509 mbedcrypto)
if(WIN32)
  target_link_libraries(dcp_net PUBLIC ws2_32 iphlpapi crypt32 bcrypt)
endif()
target_link_libraries(mobile_core PUBLIC dcp_net)

# Android (NDK, arm64-v8a, Dynarmic's arm64 backend): the program is libmain.so, started by SDL's activity; SDL3 is
# built from its pinned source as libSDL3.so. android/build_apk.sh configures this and packages the APK.
if(ANDROID)
  set(DCP_SDL3_SOURCE "" CACHE PATH "SDL3 source tree (release 3.4.18)")
  if(NOT EXISTS "${DCP_SDL3_SOURCE}/CMakeLists.txt")
    message(FATAL_ERROR "Android build: set DCP_SDL3_SOURCE to the SDL 3.4.18 source tree")
  endif()
  set(SDL_SHARED ON CACHE BOOL "" FORCE)
  set(SDL_STATIC OFF CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
  add_subdirectory("${DCP_SDL3_SOURCE}" sdl3 EXCLUDE_FROM_ALL)
  add_library(main SHARED tools/mobile_run/main.cpp src/platform/sdl_app.cpp src/platform/touch_controls.cpp
    src/platform/text_entry.cpp src/platform/overlay_text.cpp src/platform/chat_overlay.cpp src/platform/android_launcher.cpp)
  target_compile_definitions(main PRIVATE OC_WINDOW=1)
  target_compile_options(main PRIVATE ${DCP_WARN})
  target_link_libraries(main PRIVATE mobile_core SDL3::SDL3-shared android log)
  return()  # the desktop dev tools below are not built for Android
endif()

add_executable(mobile_run tools/mobile_run/main.cpp)
target_link_libraries(mobile_run PRIVATE mobile_core)
if(SDL3_FOUND)
  target_sources(mobile_run PRIVATE src/platform/sdl_app.cpp src/platform/touch_controls.cpp src/platform/text_entry.cpp
    src/platform/overlay_text.cpp src/platform/chat_overlay.cpp)
  target_compile_definitions(mobile_run PRIVATE OC_WINDOW=1)
  target_link_libraries(mobile_run PRIVATE SDL3::SDL3)
  add_custom_command(TARGET mobile_run POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:mobile_run>)
endif()

add_executable(texture_test tools/texture_test/main.cpp)
target_link_libraries(texture_test PRIVATE dcp_texture)
target_compile_options(texture_test PRIVATE ${DCP_WARN})

# Verify pass of the translator over dumped GXP programs (Work/vita/gxp); validates too with DCP_SPIRV_TOOLS.
add_executable(usse_test tools/usse_test/main.cpp)
target_link_libraries(usse_test PRIVATE dcp_usse)
target_compile_options(usse_test PRIVATE ${DCP_WARN})

# Net layer tests (host only, loopback; never the real Worker) and the local mock lobby for two-instance tests.
add_executable(test_net tests/test_net.cpp)
target_link_libraries(test_net PRIVATE dcp_net)
target_compile_options(test_net PRIVATE ${DCP_WARN})
add_executable(test_lobby tests/test_lobby.cpp)  # in-process mock lobby (loopback) unless $DCP_LOBBY_TEST_URL
target_link_libraries(test_lobby PRIVATE dcp_net)
target_compile_options(test_lobby PRIVATE ${DCP_WARN})
add_executable(dcp_lobby tools/dcp_lobby/main.cpp)
target_link_libraries(dcp_lobby PRIVATE dcp_net)
target_compile_options(dcp_lobby PRIVATE ${DCP_WARN})

# ATRAC9 decoder check against reference decodes.
add_executable(at9_test tools/at9_test/main.cpp)
target_link_libraries(at9_test PRIVATE mobile_core)
target_compile_options(mobile_run PRIVATE ${DCP_WARN})

# Dev option: validate the builder's output with SPIRV-Tools (Apache-2.0). Never linked into mobile_run.
# Offline: -DFETCHCONTENT_SOURCE_DIR_SPIRV_TOOLS=<unpacked SPIRV-Tools>. Needs Python for its generators.
option(DCP_SPIRV_TOOLS "Build spirv_test (needs SPIRV-Tools)" OFF)
if(DCP_SPIRV_TOOLS)
  set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
  set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
  set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)
  set(SPIRV_BUILD_COMPRESSION OFF CACHE BOOL "" FORCE)
  set(SPIRV-Headers_SOURCE_DIR "${spirv_headers_SOURCE_DIR}" CACHE PATH "" FORCE)
  FetchContent_Declare(spirv_tools
    URL https://github.com/KhronosGroup/SPIRV-Tools/archive/refs/tags/vulkan-sdk-1.3.296.0.tar.gz
    URL_HASH SHA256=75aafdf7e731b4b6bfb36a590ddfbb38ebc605d80487f38254da24fe0cb95837)
  FetchContent_MakeAvailable(spirv_tools)
  add_executable(spirv_test tools/spirv_test/main.cpp src/gpu/overlay_shaders.cpp src/gpu/compute_shaders.cpp)
  target_link_libraries(spirv_test PRIVATE dcp_spirv SPIRV-Tools-static)
  target_compile_options(spirv_test PRIVATE ${DCP_WARN})
  target_link_libraries(usse_test PRIVATE SPIRV-Tools-static)
  target_compile_definitions(usse_test PRIVATE DCP_HAVE_SPIRV_TOOLS=1)
endif()
