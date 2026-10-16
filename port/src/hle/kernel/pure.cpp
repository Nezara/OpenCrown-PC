#include "pure.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <mutex>

#include <windows.h>

namespace dcp::hle {

void* alloc_aligned(uint64_t len, uint64_t align) {
    if (len == 0) return nullptr;
    if (align <= 0x10000) return VirtualAlloc(nullptr, len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);  // granularity
    // Reserve an oversized range to find an aligned hole, release it, then claim the aligned part (another thread may
    // take it in between; retry).
    for (int tries = 0; tries < 32; ++tries) {
        void* p = VirtualAlloc(nullptr, len + align, MEM_RESERVE, PAGE_NOACCESS);
        if (!p) return nullptr;
        uint64_t a = align_up(reinterpret_cast<uint64_t>(p), align);
        VirtualFree(p, 0, MEM_RELEASE);
        if (void* q = VirtualAlloc(reinterpret_cast<void*>(a), len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return q;
    }
    return nullptr;
}

void* alloc_low(uint64_t len, uint64_t align) {
    static std::mutex mu;
    static uint64_t cursor = 0x200000000ull;
    constexpr uint64_t kLimit = 1ull << 40;
    if (len == 0) return nullptr;
    if (align < 0x10000) align = 0x10000;  // Windows allocation granularity
    std::lock_guard<std::mutex> lk(mu);
    for (uint64_t at = align_up(cursor, align); at + len <= kLimit;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(reinterpret_cast<void*>(at), &mi, sizeof mi)) break;
        uint64_t end = reinterpret_cast<uint64_t>(mi.BaseAddress) + mi.RegionSize;
        if (mi.State == MEM_FREE && end >= at + len) {
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(at), len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
                cursor = at + len;
                return p;
            }
        }
        at = align_up(end > at ? end : at + align, align);
    }
    return nullptr;
}

std::optional<std::string> map_under(const std::string& prefix, const std::string& root, const std::string& path) {
    if (path.compare(0, prefix.size(), prefix) != 0) return std::nullopt;
    std::string rest = path.substr(prefix.size());
    if (!rest.empty() && rest[0] != '/') return std::nullopt;  // "/app0x"
    std::string out = root;
    size_t i = 0;
    while (i < rest.size()) {
        while (i < rest.size() && rest[i] == '/') ++i;
        size_t j = rest.find('/', i);
        if (j == std::string::npos) j = rest.size();
        std::string comp = rest.substr(i, j - i);
        i = j;
        if (comp.empty() || comp == ".") continue;
        if (comp == ".." || comp.find(':') != std::string::npos || comp.find('\\') != std::string::npos)
            return std::nullopt;
        out += '/';
        out += comp;
    }
    return out;
}

bool valid_savedata_dirname(const char* n) {
    if (!n) return false;
    size_t len = 0;
    for (; n[len]; ++len) {
        char c = n[len];
        if (len >= 31 || !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    }
    return len > 0;
}

static bool match_from(const char* p, const char* s) {
    for (; *p; ++p, ++s) {
        if (*p == '%') {
            for (const char* t = s;; ++t) {
                if (match_from(p + 1, t)) return true;
                if (!*t) return false;
            }
        }
        if (!*s || (*p != '_' && *p != *s)) return false;
    }
    return !*s;
}
bool savedata_name_match(const char* pattern, const std::string& name) {
    return !pattern || !*pattern || match_from(pattern, name.c_str());
}

namespace {
struct ButtonName { const char* name; uint32_t bit; };
// ScePadData.buttons bits (SDK ScePadButtonDataOffset).
constexpr ButtonName kButtons[] = {
    {"L3", 0x2}, {"R3", 0x4}, {"OPTIONS", 0x8}, {"UP", 0x10}, {"RIGHT", 0x20}, {"DOWN", 0x40}, {"LEFT", 0x80},
    {"L2", 0x100}, {"R2", 0x200}, {"L1", 0x400}, {"R1", 0x800}, {"TRIANGLE", 0x1000}, {"CIRCLE", 0x2000},
    {"CROSS", 0x4000}, {"SQUARE", 0x8000}, {"TOUCHPAD", 0x100000},
    // Left stick held to an edge (script-only bits, see kScriptStick*; the pad shim turns them into lx/ly).
    {"LS_LEFT", 0x10000000}, {"LS_RIGHT", 0x20000000}, {"LS_UP", 0x40000000}, {"LS_DOWN", 0x80000000}};

bool parse_buttons(std::string tok, uint32_t* out) {
    *out = 0;
    if (tok.empty()) return false;
    if (tok[0] >= '0' && tok[0] <= '9') {
        char* end = nullptr;
        unsigned long v = std::strtoul(tok.c_str(), &end, 0);
        if (*end) return false;
        *out = uint32_t(v);
        return true;
    }
    size_t i = 0;
    while (i <= tok.size()) {
        size_t j = tok.find_first_of("+|", i);
        if (j == std::string::npos) j = tok.size();
        std::string name = tok.substr(i, j - i);
        for (char& c : name) c = char(std::toupper(static_cast<unsigned char>(c)));
        bool found = false;
        for (const auto& b : kButtons)
            if (name == b.name) *out |= b.bit, found = true;
        if (!found) return false;
        i = j + 1;
    }
    return true;
}
}  // namespace

bool parse_input_script(const std::string& text, std::vector<InputEvent>* out, int* bad_line) {
    out->clear();
    int line_no = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        ++line_no;
        if (size_t h = line.find('#'); h != std::string::npos) line.resize(h);
        std::vector<std::string> tok;
        for (size_t i = 0; i < line.size();) {
            while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
            size_t j = i;
            while (j < line.size() && !std::isspace(static_cast<unsigned char>(line[j]))) ++j;
            if (j > i) tok.push_back(line.substr(i, j - i));
            i = j;
        }
        if (tok.empty()) continue;
        char* end = nullptr;
        unsigned long long f = std::strtoull(tok[0].c_str(), &end, 10);
        uint32_t b = 0;
        if (*end || tok.size() != 2 || !parse_buttons(tok[1], &b)) {
            if (bad_line) *bad_line = line_no;
            return false;
        }
        out->push_back({f, b});
    }
    std::stable_sort(out->begin(), out->end(), [](const InputEvent& a, const InputEvent& b) { return a.frame < b.frame; });
    return true;
}

uint32_t script_buttons_at(const std::vector<InputEvent>& ev, uint64_t frame) {
    uint32_t b = 0;
    for (const auto& e : ev) {
        if (e.frame > frame) break;
        b = e.buttons;
    }
    return b;
}

int64_t dmem_alloc(uint64_t* cursor, uint64_t start, uint64_t end, uint64_t len, uint64_t align) {
    if (len == 0 || (align & (align - 1))) return -1;
    uint64_t at = align_up(*cursor > start ? *cursor : start, align ? align : 1);
    if (at + len > end || at + len < at) return -1;
    *cursor = at + len;
    return int64_t(at);
}

}  // namespace dcp::hle

namespace dcp::hle {

int SaveMountTable::index_of(const std::string& mp) const {
    static const std::string kPrefix = "/savedata";
    if (mp.compare(0, kPrefix.size(), kPrefix) != 0 || mp.size() == kPrefix.size() || mp.size() > kPrefix.size() + 2)
        return -1;
    int n = 0;
    for (size_t i = kPrefix.size(); i < mp.size(); ++i) {
        if (mp[i] < '0' || mp[i] > '9') return -1;
        n = n * 10 + (mp[i] - '0');
    }
    if (mp.size() == kPrefix.size() + 2 && mp[kPrefix.size()] == '0') return -1;  // "/savedata01"
    return n < kMax && slots_[n].used ? n : -1;
}

std::string SaveMountTable::mount(const std::string& dir, const std::string& host) {
    for (const Slot& s : slots_)
        if (s.used && s.dir == dir) return "";
    for (int i = 0; i < kMax; ++i)
        if (!slots_[i].used) {
            slots_[i] = {dir, host, true};
            return "/savedata" + std::to_string(i);
        }
    return "";
}

bool SaveMountTable::umount(const std::string& mp) {
    int i = index_of(mp);
    if (i < 0) return false;
    slots_[i] = {};
    return true;
}

std::optional<std::string> SaveMountTable::resolve(const std::string& path) const {
    for (int i = 0; i < kMax; ++i)
        if (slots_[i].used)
            if (auto h = map_under("/savedata" + std::to_string(i), slots_[i].host, path)) return h;
    return std::nullopt;
}

const std::string* SaveMountTable::dir_name_of(const std::string& mp) const {
    int i = index_of(mp);
    return i < 0 ? nullptr : &slots_[i].dir;
}
const std::string* SaveMountTable::host_of(const std::string& mp) const {
    int i = index_of(mp);
    return i < 0 ? nullptr : &slots_[i].host;
}

}  // namespace dcp::hle
