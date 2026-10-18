// Internal helpers shared by the shim sources.
#pragma once
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

#include "hle.h"
#include "loader/hle.h"

namespace dcp::hle {

const Config& config();

// Host monotonic clock in nanoseconds (CLOCK_MONOTONIC; the pacing clock of video / audio / pad timestamps).
inline uint64_t host_ns() {
    timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}
// Sleeps `ns` nanoseconds (relative; 0 = return at once).
inline void host_sleep_ns(uint64_t ns) {
    timespec ts{time_t(ns / 1000000000ull), long(ns % 1000000000ull)};
    while (ts.tv_sec || ts.tv_nsec)
        if (::nanosleep(&ts, &ts) == 0) break;
}
bool trace_enabled();  // DCP_TRACE=1: log every shim call, not only the first

// Logs "HLE name: fmt" (first call per call site only, unless tracing).
#define DCP_HLE_LOG(name, fmt, ...)                                                    \
    do {                                                                               \
        static bool dcp_once_ = false;                                                 \
        if (!dcp_once_ || ::dcp::hle::trace_enabled()) {                               \
            dcp_once_ = true;                                                          \
            std::fprintf(stderr, "HLE %s: " fmt "\n", name __VA_OPT__(, ) __VA_ARGS__); \
        }                                                                              \
    } while (0)

#define REG(lib, fn) ::dcp::loader::register_hle(lib, #fn, reinterpret_cast<void*>(&fn))

// SCE kernel error codes (0x80020000 | errno).
constexpr int32_t kOk = 0;
constexpr int32_t kUserId = 1000;  // the one signed-in local user (shadPS4 uses 1000 too)
constexpr int32_t kErr(int e) { return int32_t(0x80020000u | unsigned(e)); }
enum { kEPERM = 1, kENOENT = 2, kEBADF = 9, kENOMEM = 12, kEACCES = 13, kEFAULT = 14, kEBUSY = 16, kEEXIST = 17,
       kEINVAL = 22, kEMFILE = 24, kENOSPC = 28, kEAGAIN = 35, kETIMEDOUT = 60, kENOSYS = 78 };

int* errno_ptr();  // per-thread errno (__error)
inline int64_t set_errno(int e) { *errno_ptr() = e; return -1; }

void register_kernel_memory();
void register_kernel_thread();
void register_kernel_file();
void register_kernel_misc();
void register_services();
void register_savedata();
void register_dialogs();
void register_pad();
void register_audio();
void register_video();
void register_font();
void register_net();
void register_np();  // NpManager, NetCtl, NpToolkit2, Matching2 (np.cpp, matching2.cpp)

// Local PSN identity: online id from $DCP_NP_NAME (else the Linux user name); account id + lobby secret from
// <user dir>/identity.ini (net/identity.h; created on first use). Without that file (unwritable dir) the account id is
// derived from the name and secret is empty (the lobby then treats the connection as unverified: no friends).
struct NpIdentity {
    std::string online_id;
    uint64_t account_id = 0;
    std::string secret;  // 64 hex chars, sent in the lobby hello
};
const NpIdentity& np_identity();

// Toolkit2 presence over the game's lobby session (hle/matching2.cpp; notes/map/friends.md 5, lobby_protocol.md 4.3).
struct PresenceFriend {
    uint64_t account = 0;
    std::string name;
    uint64_t room = 0;                // lobby room id the friend sits in
    std::vector<uint8_t> data, status;  // its SetPresence game data (0x78-B wire RoomInfo) and 0xc0-B status field
};
void presence_publish(const uint8_t* data, size_t n, const uint8_t* status, size_t status_n);  // SetPresence
void presence_clear();
void presence_fetch();  // GetPresence: one friend_list round trip (answered at once when not online)
bool presence_busy();
// Friends online in a room with a blob, from the last finished fetch; gen changes with every finished fetch.
std::vector<PresenceFriend> presence_result(uint64_t* gen);

// Host directory for a path under a mounted save-data mount point ("/savedata0/..."), or nullopt.
std::optional<std::string> resolve_savedata(const std::string& ps4_path);
// Vblanks waited so far (the frame clock that the input script and --frames use).
uint64_t vblank_count();
uint64_t flip_count();
void register_gnm();

}  // namespace dcp::hle
