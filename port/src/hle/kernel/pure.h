// Host-independent pieces of the kernel shims (unit-tested in tests/test_hle.cpp).
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dcp::hle {

constexpr uint64_t align_up(uint64_t v, uint64_t a) { return a ? (v + a - 1) / a * a : v; }

// Reserve + commit `len` bytes of RW host memory whose address is a multiple of `align` (power of two; 0 = any).
void* alloc_aligned(uint64_t len, uint64_t align);

// Like alloc_aligned, but below 1 TiB (from 8 GiB up, as on the PS4): GPU packets carry 40-bit addresses (e.g. the
// EOP label address is masked with 0xfffffffff8), so GPU-visible (direct) memory must fit in 40 bits. Thread-safe.
void* alloc_low(uint64_t len, uint64_t align);

// PS4 path under mount point `prefix` (e.g. "/app0", "/savedata0") -> host path under `root`. Rejects "..", drive
// letters and backslashes; a path outside the prefix -> nullopt.
std::optional<std::string> map_under(const std::string& prefix, const std::string& root, const std::string& ps4_path);
inline std::optional<std::string> map_app0(const std::string& app0_root, const std::string& ps4_path) {
    return map_under("/app0", app0_root, ps4_path);
}

// SaveData directory names: 1..31 chars of [A-Za-z0-9_-] (they become host directory names).
bool valid_savedata_dirname(const char* name);
// SDK dir-name search pattern: '%' = any run of characters, '_' = one character; empty/null pattern matches all.
bool savedata_name_match(const char* pattern, const std::string& name);

// Headless input script: lines "frame buttons" (buttons = hex/decimal ScePadData mask or names joined by '+' / '|',
// e.g. "120 CROSS", "130 0", "200 OPTIONS+UP"); '#' starts a comment. A line's state holds until the next line.
// Names LS_LEFT/LS_RIGHT/LS_UP/LS_DOWN set the script-only bits below (left stick at an edge).
constexpr uint32_t kScriptStickLeft = 0x10000000, kScriptStickRight = 0x20000000, kScriptStickUp = 0x40000000,
                   kScriptStickDown = 0x80000000, kScriptStickMask = 0xf0000000;
struct InputEvent { uint64_t frame; uint32_t buttons; };
// Returns false (and the 1-based line number in *bad_line) on a malformed line. Events are sorted by frame.
bool parse_input_script(const std::string& text, std::vector<InputEvent>* out, int* bad_line);
uint32_t script_buttons_at(const std::vector<InputEvent>& ev, uint64_t frame);

// Bump allocator for the direct-memory offset space: first `align`-aligned run of `len` in [start, end) at or after
// `*cursor`. Returns the offset or -1; advances *cursor.
int64_t dmem_alloc(uint64_t* cursor, uint64_t start, uint64_t end, uint64_t len, uint64_t align);

}  // namespace dcp::hle

namespace dcp::hle {

// Save-data mount points "/savedata0".."/savedata15" -> host directories. Not thread-safe (callers lock).
class SaveMountTable {
public:
    static constexpr int kMax = 16;
    // Returns the mount point name or "" when the directory is already mounted / the table is full.
    std::string mount(const std::string& dir_name, const std::string& host_dir);
    bool umount(const std::string& mount_point);
    std::optional<std::string> resolve(const std::string& ps4_path) const;  // nullopt outside any mount
    const std::string* dir_name_of(const std::string& mount_point) const;
    const std::string* host_of(const std::string& mount_point) const;

private:
    struct Slot { std::string dir, host; bool used = false; };
    int index_of(const std::string& mount_point) const;
    Slot slots_[kMax];
};

}  // namespace dcp::hle
