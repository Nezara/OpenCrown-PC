#include "hle.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

#include <sys/mman.h>

#include "nid.h"

namespace dcp::loader {
namespace {

const NidName kNames[] = {
#include "sce_names.inc"
};

std::unordered_map<std::string, NidName>& nid_table() {
    static std::unordered_map<std::string, NidName> t = [] {
        std::unordered_map<std::string, NidName> m;
        for (const auto& n : kNames) m[nid_from_name(n.name)] = n;
        return m;
    }();
    return t;
}

std::map<std::string, void*>& hle_table() {  // key NID (functions and data)
    static std::map<std::string, void*> t;
    return t;
}
std::map<std::string, uint64_t>& export_table() {  // key NID
    static std::map<std::string, uint64_t> t;
    return t;
}

// Stub thunk: `mov eax, idx; movabs r11, dcp_unimpl_tramp; jmp r11`. The import index travels in eax (free at a SysV
// call boundary apart from al = vector-arg count for varargs, which stubs ignore), so concurrent game threads cannot race.
constexpr size_t kThunkSize = 32, kMaxStubs = 4096;
uint8_t* g_stub_page = nullptr;
size_t g_nstubs = 0;
std::vector<std::string> g_stub_names;
std::vector<bool> g_stub_logged;

std::mutex g_ring_mu;
std::vector<std::string> g_ring;  // last calls
constexpr size_t kRing = 16;

alignas(16) uint8_t g_data_blocks[64][256];
size_t g_ndata = 0;

__attribute__((noinline)) uint64_t unimpl_impl(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f,
                                               uint32_t idx) noexcept {
    const std::string& name = idx < g_stub_names.size() ? g_stub_names[idx] : std::string("?");
    char buf[512];
    std::snprintf(buf, sizeof buf, "%s(%llx, %llx, %llx, %llx, %llx, %llx)", name.c_str(), (unsigned long long)a,
                  (unsigned long long)b, (unsigned long long)c, (unsigned long long)d, (unsigned long long)e,
                  (unsigned long long)f);
    {
        std::lock_guard<std::mutex> lk(g_ring_mu);
        if (g_ring.size() == kRing) g_ring.erase(g_ring.begin());
        g_ring.push_back(buf);
        if (idx < g_stub_logged.size() && !g_stub_logged[idx]) {
            g_stub_logged[idx] = true;
            std::fprintf(stderr, "UNIMPL %s\n", buf);
        }
    }
    return 0;
}

}  // namespace
}  // namespace dcp::loader

extern "C" DCP_SYSV uint64_t dcp_unimpl_handler(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f,
                                               uint64_t idx) {
    return dcp::loader::unimpl_impl(a, b, c, d, e, f, uint32_t(idx));
}
extern "C" void dcp_unimpl_tramp();
// Realign the stack and pass eax (import index) as the 7th SysV argument (on the stack).
asm(R"(
    .text
    .globl dcp_unimpl_tramp
dcp_unimpl_tramp:
    push %rbp
    mov %rsp, %rbp
    and $-16, %rsp
    sub $8, %rsp
    push %rax
    call dcp_unimpl_handler@PLT
    leave
    ret
)");

namespace dcp::loader {
namespace {

uint64_t make_stub(const std::string& display) {
    if (!g_stub_page) {
        void* p = mmap(nullptr, kThunkSize * kMaxStubs, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        g_stub_page = p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
        if (!g_stub_page) throw std::runtime_error("hle: cannot allocate stub page");
    }
    if (g_nstubs == kMaxStubs) throw std::runtime_error("hle: too many stubs");
    uint32_t idx = uint32_t(g_nstubs++);
    uint8_t* t = g_stub_page + idx * kThunkSize;
    uint64_t ph = reinterpret_cast<uint64_t>(&dcp_unimpl_tramp);
    std::memset(t, 0xCC, kThunkSize);
    t[0] = 0xB8, std::memcpy(t + 1, &idx, 4);                 // mov eax, idx
    t[5] = 0x49, t[6] = 0xBB, std::memcpy(t + 7, &ph, 8);     // movabs r11, dcp_unimpl_tramp
    t[15] = 0x41, t[16] = 0xFF, t[17] = 0xE3;                 // jmp r11
    g_stub_names.push_back(display);
    g_stub_logged.push_back(false);
    return reinterpret_cast<uint64_t>(t);
}

}  // namespace

void register_hle(const char*, const char* name, void* fn) { hle_table()[nid_from_name(name)] = fn; }
void register_hle_nid(const char*, const char* nid, void* fn) { hle_table()[nid] = fn; }
void register_hle_data(const char* name, void* obj) { hle_table()[nid_from_name(name)] = obj; }
void add_module_exports(const std::map<std::string, uint64_t>& by_nid) {
    for (const auto& [k, v] : by_nid) export_table().emplace(k, v);
}

const NidName* lookup_nid(const std::string& nid) {
    auto it = nid_table().find(nid);
    return it == nid_table().end() ? nullptr : &it->second;
}

uint64_t Binder::resolve(const Symbol& s) {
    std::string nid = s.name.substr(0, s.name.find('#'));
    auto c = cache_.find(nid);
    if (c != cache_.end()) return c->second;
    return cache_[nid] = resolve_new(s);
}

uint64_t Binder::resolve_new(const Symbol& s) {
    std::string nid = s.name.substr(0, s.name.find('#'));
    const NidName* n = lookup_nid(nid);
    std::string display = n ? n->lib + "::" + n->name : "?::" + s.name;
    if (!n) {
        ++stats_.unknown;
        stats_.unknown_nids.push_back(s.name);
    }
    if (auto h = hle_table().find(nid); h != hle_table().end()) {
        ++stats_.hle;
        return reinterpret_cast<uint64_t>(h->second);
    }
    if (auto x = export_table().find(nid); x != export_table().end()) {
        ++stats_.exported;
        return x->second;
    }
    if ((s.info & 0xF) == 1) {  // STT_OBJECT: give it zeroed storage (vtables / guards / markers)
        if (g_ndata == 64) throw std::runtime_error("hle: too many data imports");
        uint8_t* blk = g_data_blocks[g_ndata++];
        if (n && n->name == "__stack_chk_guard") std::memcpy(blk, "\x5a\xa5\x13\x37\xc0\xde\x42\x00", 8);
        ++stats_.data;
        return reinterpret_cast<uint64_t>(blk);
    }
    ++stats_.stubs;
    return make_stub(display);
}

void Binder::finalize() {
    if (g_stub_page) mprotect(g_stub_page, kThunkSize * kMaxStubs, PROT_READ | PROT_EXEC);
}

std::vector<std::string> recent_unimpl_calls() {
    std::lock_guard<std::mutex> lk(g_ring_mu);
    return g_ring;
}

bool stub_name_at(uint64_t addr, std::string* name) {
    uint64_t b = reinterpret_cast<uint64_t>(g_stub_page);
    if (!g_stub_page || addr < b || addr >= b + g_nstubs * kThunkSize) return false;
    if (name) *name = g_stub_names[(addr - b) / kThunkSize];
    return true;
}

}  // namespace dcp::loader
