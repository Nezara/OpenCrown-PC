// Import binding: NID -> known SDK name, real HLE shims where registered, otherwise a logging stub per import.
// Everything the game calls uses the SysV ABI, so shims must be declared DCP_SYSV.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "elf.h"

#define DCP_SYSV __attribute__((sysv_abi))

namespace dcp::loader {

// Bind the SDK function `name` to `fn` (a DCP_SYSV function). Keyed by NID, so it applies to every module importing it
// whatever its library id. `lib` is informational. Call before relocating. Takes precedence over module exports.
void register_hle(const char* lib, const char* name, void* fn);
// Same, keyed by a raw NID (imports whose SDK name is unknown, e.g. the private libSceNpToolkit2 C++ NIDs).
void register_hle_nid(const char* lib, const char* nid, void* fn);
// Bind a data import (STT_OBJECT) by name to storage we own (e.g. __progname). Precedence as register_hle.
void register_hle_data(const char* name, void* obj);
// Exports of a loaded module (e.g. the user's libc.prx): NID -> absolute address. Consulted after HLE shims.
void add_module_exports(const std::map<std::string, uint64_t>& by_nid);

// Known SDK name for a NID ("" if unknown) and its library.
struct NidName { std::string lib, name; };
const NidName* lookup_nid(const std::string& nid);

struct BindStats { size_t hle = 0, exported = 0, stubs = 0, data = 0, unknown = 0; std::vector<std::string> unknown_nids; };

class Binder {
public:
    // Resolver for relocate(): HLE fn, generated stub, or a zeroed data block for STT_OBJECT imports.
    uint64_t resolve(const Symbol& s);
    const BindStats& stats() const { return stats_; }
    void finalize();  // make the stub page executable (call after relocation)

private:
    uint64_t resolve_new(const Symbol& s);
    BindStats stats_;
    std::map<std::string, uint64_t> cache_;  // by NID: one stub / data block per import, shared by every module
};

// Most recent stub calls (newest last), "lib::name(rdi=.., ...)", for crash reports.
std::vector<std::string> recent_unimpl_calls();
// Is `addr` inside the stub area? Gives the import's display name if so.
bool stub_name_at(uint64_t addr, std::string* name);

}  // namespace dcp::loader
