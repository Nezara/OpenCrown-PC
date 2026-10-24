// Story / tutorial skip questions, see skip_offers.h and notes/map/story_progress.md for the asm evidence.
//
// Story skip, FUN_003adbf0 (creation screen) state 1 on confirm (asm 0x3aed82..0x3aedb2):
//   cmp byte [r13+0xf8da], 0 ; je ->state 8     (0xf8da = new hero, or an edited hero with story counter <= 2)
//   and ... (six "class k cleared" header bytes all <= 0) ; jne ->state 8     <- NOPed
//   edx = 9 (the skip offer; "yes" runs the game's own writes, 0x3ae3cb..0x3ae5eb)
// Tutorial skip, FUN_003a69e0 state 3 (asm 0x3a8ac9..):
//   call FUN_00477f80 (story counter > 0 or file tutorial mark slot+0x48) ; test al, al ; je 0x3a9025   <- NOPed
//   0x3a9025 is the auto-start path (wait ~3 s, then round/Tutorial.brb); the fall-through builds dialog 0x5d8,
//   whose "no" writes hero+0x103 = 1 (0x3a90dc) and goes to town. The state is only entered while hero+0x103 == 0.
//
// No-skip warning (story_progress.md 9; with the story skip). Online needs hero+0x6f >= 0x2d (FUN_00086f00), which
// the skip sets, so the first "no" asks again with a warning; the second answer is final. State 9 of FUN_003adbf0:
//   0x3ae391  mov esi, 0x3cc ... FUN_004ec630 -> FUN_0006b850 -> FUN_00591450 (message id -> UTF-8, vsnprintf'd)
//   0x3ae3b3 / 0x3ae3bc  je 0x3ae5f0 (answer 0 cancel / 3 no -> state 8)        <- rel32 retargeted to relay B
//   0x3ae3cb  mov rbx, r15 ; mov esi, 0x2d (first yes instructions)             <- jmp relay Y (+3-byte nop)
//   0x591450  xor eax, eax ; test esi, esi ; js ret (lookup entry)               <- jmp relay L (+nop)
// Two flag bytes: [0] text_due (cleared by the one lookup it serves), [1] in_second. Relay B: second answer -> clear,
// state 8 as before; first -> set both, SetState(kind, 9) (= the 0x3ae5f0 sequence with edx 9), which re-arms the
// first-frame build, so the next frame shows a fresh dialog whose 0x3cc text is ours. Relay Y clears both. Every
// relay is plain bytes, checked with capstone (DCP_SKIP_DUMP=1 prints them); no register but rax is touched where
// rax is dead (the lookup returns in rax; B and Y reload it before use in the original code).
#include "skip_offers.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include <sys/mman.h>

namespace dcp::skip_offers {
namespace {

struct Patch {
    const char* env;
    const char* what;
    uint32_t site;
    uint8_t len;
    uint8_t expect[6];
    uint8_t repl[6];
};

constexpr Patch kPatches[] = {
    {"DCP_STORY_SKIP", "story skip offered to every new character", 0x3aeda9, 2, {0x75, 0x07}, {0x66, 0x90}},  // jne -> 2-byte nop
    {"DCP_TUTORIAL_SKIP", "tutorial question asked on the first character too", 0x3a8adb, 6,
     {0x0f, 0x84, 0x44, 0x05, 0x00, 0x00}, {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00}},  // je rel32 -> 6-byte nop
};

// The second ask: our text replaces message 0x3cc once. ASCII + '\n' only, no '%' (it is a vsnprintf format).
constexpr char kWarning[] =
    "Without the skip, this character can't play online\n"
    "until the first half of the story is finished.\n"
    "Skip to the talisman collecting? (Player starts at LV15)";

constexpr uint32_t kLookup = 0x591450, kLookupCont = 0x591456;
constexpr uint32_t kNoA = 0x3ae3b3, kNoB = 0x3ae3bc, kSetState = 0x3aecd2, kState8 = 0x3ae5f0;
constexpr uint32_t kYes = 0x3ae3cb, kYesCont = 0x3ae3d3;
constexpr uint8_t kLookupBytes[] = {0x31, 0xc0, 0x85, 0xf6, 0x78, 0x21};
constexpr uint8_t kNoABytes[] = {0x0f, 0x84, 0x37, 0x02, 0x00, 0x00}, kNoBBytes[] = {0x0f, 0x84, 0x2e, 0x02, 0x00, 0x00};
constexpr uint8_t kYesBytes[] = {0x4c, 0x89, 0xfb, 0xbe, 0x2d, 0x00, 0x00, 0x00};

bool off(const char* env) {
    const char* v = std::getenv(env);
    return v && std::strcmp(v, "0") == 0;
}

void write_code(uint8_t* at, const uint8_t* bytes, size_t n) {  // patch sites are in the r-x text segment
    uint64_t lo = reinterpret_cast<uint64_t>(at) & ~0xfffull, hi = (reinterpret_cast<uint64_t>(at) + n + 0xfff) & ~0xfffull;
    mprotect(reinterpret_cast<void*>(lo), hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC);
    std::memcpy(at, bytes, n);
    mprotect(reinterpret_cast<void*>(lo), hi - lo, PROT_READ | PROT_EXEC);  // x86: no instruction cache flush needed
}

uint8_t* alloc_near(uint64_t from, size_t size) {
    for (uint64_t a = (from + 0xffff) & ~0xffffull; a < from + (1ull << 30); a += 0x10000) {
        void* p = mmap(reinterpret_cast<void*>(a), size, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == MAP_FAILED) continue;
        if (p == reinterpret_cast<void*>(a)) return static_cast<uint8_t*>(p);
        munmap(p, size);  // pre-4.17 kernel: the address was only a hint
    }
    return nullptr;
}

struct Emit {
    uint8_t* p;
    void b(std::initializer_list<uint8_t> v) { for (uint8_t x : v) *p++ = x; }
    void q(uint64_t v) { std::memcpy(p, &v, 8), p += 8; }
    void jmp_abs(uint64_t to) { b({0xff, 0x25, 0, 0, 0, 0}), q(to); }  // jmp [rip+0]
};

bool fits_rel32(const uint8_t* to, const uint8_t* next) {
    int64_t r = int64_t(to - next);
    return r == int32_t(r);
}

void put_rel32(uint8_t* bytes, const uint8_t* to, const uint8_t* next) {
    int32_t r = int32_t(to - next);
    std::memcpy(bytes, &r, 4);
}

void install_warning(uint8_t* base) {
    uint8_t *lookup = base + kLookup, *no_a = base + kNoA, *no_b = base + kNoB, *yes = base + kYes;
    if (std::memcmp(lookup, kLookupBytes, sizeof kLookupBytes) || std::memcmp(no_a, kNoABytes, sizeof kNoABytes) ||
        std::memcmp(no_b, kNoBBytes, sizeof kNoBBytes) || std::memcmp(yes, kYesBytes, sizeof kYesBytes)) {
        std::fprintf(stderr, "skip_offers: unexpected bytes at the warning sites; no-skip warning not installed\n");
        return;
    }
    uint8_t* page = alloc_near(reinterpret_cast<uint64_t>(base) + 0x1200000, 0x1000);
    if (!page) { std::fprintf(stderr, "skip_offers: no relay memory near the image; no warning\n"); return; }
    const uint64_t b64 = reinterpret_cast<uint64_t>(base);
    uint8_t* flags = page;            // [0] text_due, [1] in_second
    char* text = reinterpret_cast<char*>(page + 0x10);
    std::memcpy(text, kWarning, sizeof kWarning);
    uint8_t *L = page + 0x100, *B = page + 0x180, *Y = page + 0x200;
    const uint64_t f64 = reinterpret_cast<uint64_t>(flags);

    Emit e{L};  // lookup: if (id == 0x3cc && text_due) { text_due = 0; return text; } else original entry
    e.b({0x81, 0xfe, 0xcc, 0x03, 0x00, 0x00, 0x75, 0x1d, 0x48, 0xb8}), e.q(f64);  // cmp esi,0x3cc; jne orig; mov rax,flags
    e.b({0x80, 0x38, 0x00, 0x74, 0x0e, 0xc6, 0x00, 0x00, 0x48, 0xb8});             // cmp [rax],0; je orig; mov [rax],0
    e.q(reinterpret_cast<uint64_t>(text)), e.b({0xc3});                          // mov rax,text; ret
    e.b({0x31, 0xc0, 0x85, 0xf6, 0x79, 0x01, 0xc3}), e.jmp_abs(b64 + kLookupCont);  // orig: xor; test; jns +1; ret
    const size_t nl = e.p - L;

    e.p = B;  // a "no": second -> final (state 8); first -> both flags, SetState(kind, 9)
    e.b({0x48, 0xb8}), e.q(f64);
    e.b({0x80, 0x78, 0x01, 0x00, 0x75, 0x22, 0x66, 0xc7, 0x00, 0x01, 0x01});  // cmp [rax+1],0; jne final; mov word [rax],0x101
    e.b({0x49, 0x8b, 0x45, 0x00, 0xba, 0x09, 0x00, 0x00, 0x00, 0x4c, 0x89, 0xef, 0x44, 0x89, 0xe6});  // rax=[r13] edx=9 rdi esi
    e.jmp_abs(b64 + kSetState);
    e.b({0xc6, 0x40, 0x01, 0x00}), e.jmp_abs(b64 + kState8);                    // final: mov [rax+1],0
    const size_t nb = e.p - B;

    e.p = Y;  // a "yes": clear both, replay mov rbx,r15 ; mov esi,0x2d
    e.b({0x48, 0xb8}), e.q(f64);
    e.b({0x66, 0xc7, 0x00, 0x00, 0x00, 0x4c, 0x89, 0xfb, 0xbe, 0x2d, 0x00, 0x00, 0x00});
    e.jmp_abs(b64 + kYesCont);
    const size_t ny = e.p - Y;

    if (!fits_rel32(L, lookup + 5) || !fits_rel32(B, no_a + 6) || !fits_rel32(B, no_b + 6) || !fits_rel32(Y, yes + 5)) {
        std::fprintf(stderr, "skip_offers: relay out of rel32 range; no warning\n");
        return;
    }
    if (const char* d = std::getenv("DCP_SKIP_DUMP"); d && *d && *d != '0') {
        auto hex = [](const char* n, const uint8_t* p, size_t len) {
            std::fprintf(stderr, "skip_offers: relay %s ", n);
            for (size_t i = 0; i < len; ++i) std::fprintf(stderr, "%02x", p[i]);
            std::fprintf(stderr, "\n");
        };
        hex("L", L, nl), hex("B", B, nb), hex("Y", Y, ny);
    }
    uint8_t j5[6] = {0xe9, 0, 0, 0, 0, 0x90};
    put_rel32(j5 + 1, L, lookup + 5);
    write_code(lookup, j5, 6);
    uint8_t ja[6] = {0x0f, 0x84}, jb[6] = {0x0f, 0x84};
    put_rel32(ja + 2, B, no_a + 6), put_rel32(jb + 2, B, no_b + 6);
    write_code(no_a, ja, 6), write_code(no_b, jb, 6);
    uint8_t jy[8] = {0xe9, 0, 0, 0, 0, 0x0f, 0x1f, 0x00};
    put_rel32(jy + 1, Y, yes + 5);
    write_code(yes, jy, 8);
    std::fprintf(stderr, "skip_offers: the first \"no\" to the story skip warns and asks again\n");
}


// Party return to town (town_session.md 12). Online, the result window's "Return to town" (answer 1) makes each
// player leave the room on its own (Act mode 5 state 7, 0x430e4a host-bit-off + 0x430e54 Net::DissolveSession).
// The game's own way back to town that keeps the party is the shared world map's "Return to town": the host's pick
// is broadcast (scene command 0x7e6, +0x39 = 1 -> Act+0x110 = 1 on every member) and state 0xd loads Town00 for all.
//   P at 0x42e9bf (state 7, answer = window+0x9108 -> Act+0x9314), only for answer 1, kinds 0/1/2/0xc (the ones that
//     show the choice and reach state 10's default branch) and an online party (platform sys+0x16908 bit 0):
//     guest -> answer 0 (it follows the host); host -> answer 0 and Act+0x110 = 1.
//   T at 0x431171 (state 0xb host branch, `call 54b9d0` = world map +0x2d18 == -2 "town confirmed"): with Act+0x110
//     set, write -2 first (what the map's "Return to town?" yes does at 0x548081), so the game sends its own town
//     command. Act+0x110 is cleared in state 2 of every round end and set otherwise only by that command.
constexpr uint32_t kAnswer = 0x42e9bf, kAnswerCont = 0x42e9cd, kTownCheck = 0x431171, kTownCheckFn = 0x54b9d0;
constexpr uint8_t kAnswerBytes[] = {0x41, 0x8b, 0x84, 0x24, 0x08, 0x91, 0x00, 0x00, 0x89, 0x83, 0x14, 0x93, 0x00, 0x00};
constexpr uint8_t kTownCheckBytes[] = {0xe8, 0x5a, 0xa8, 0x11, 0x00};

void install_party_return(uint8_t* base) {
    uint8_t *ans = base + kAnswer, *town = base + kTownCheck;
    if (std::memcmp(ans, kAnswerBytes, sizeof kAnswerBytes) || std::memcmp(town, kTownCheckBytes, sizeof kTownCheckBytes)) {
        std::fprintf(stderr, "skip_offers: unexpected bytes at the round-end sites; party return not installed\n");
        return;
    }
    uint8_t* page = alloc_near(reinterpret_cast<uint64_t>(base) + 0x1300000, 0x1000);
    if (!page) { std::fprintf(stderr, "skip_offers: no relay memory near the image; no party return\n"); return; }
    const uint64_t b64 = reinterpret_cast<uint64_t>(base);
    uint8_t *P = page, *T = page + 0x80;
    Emit e{P};
    e.b({0x41, 0x8b, 0x84, 0x24, 0x08, 0x91, 0x00, 0x00,   // mov eax,[r12+0x9108]   (original)
         0x51, 0x52, 0x83, 0xf8, 0x01, 0x75, 0x36,         // push rcx; push rdx; cmp eax,1; jne done
         0x8b, 0x93, 0xe8, 0x00, 0x00, 0x00,               // mov edx,[rbx+0xe8]      (kind)
         0x83, 0xfa, 0x0c, 0x77, 0x2b,                     // cmp edx,0xc; ja done
         0xb9, 0x07, 0x10, 0x00, 0x00, 0x0f, 0xa3, 0xd1,   // mov ecx,0x1007; bt ecx,edx
         0x73, 0x21,                                       // jnc done
         0x49, 0x8b, 0x4e, 0x78,                           // mov rcx,[r14+0x78]      (platform sys)
         0xf6, 0x81, 0x08, 0x69, 0x01, 0x00, 0x01,         // test byte [rcx+0x16908],1 (online party)
         0x74, 0x14,                                       // jz done
         0x0f, 0xb7, 0x91, 0xf0, 0x68, 0x01, 0x00,         // movzx edx,word [rcx+0x168f0] (my slot, host slot)
         0x38, 0xf2, 0x75, 0x07,                           // cmp dl,dh; jne zero    (guest)
         0xc6, 0x83, 0x10, 0x01, 0x00, 0x00, 0x01,         // mov byte [rbx+0x110],1 (host: town for all)
         0x31, 0xc0,                                       // zero: xor eax,eax
         0x5a, 0x59,                                       // done: pop rdx; pop rcx
         0x89, 0x83, 0x14, 0x93, 0x00, 0x00});             // mov [rbx+0x9314],eax   (original)
    e.jmp_abs(b64 + kAnswerCont);
    const size_t np = e.p - P;
    e.p = T;
    e.b({0x48, 0x8b, 0x44, 0x24, 0x20,                             // mov rax,[rsp+0x20] (Act, the caller's [rsp+0x18])
         0x80, 0xb8, 0x10, 0x01, 0x00, 0x00, 0x01, 0x75, 0x0a,       // cmp byte [rax+0x110],1; jne orig
         0xc7, 0x87, 0x18, 0x2d, 0x00, 0x00, 0xfe, 0xff, 0xff, 0xff}); // mov dword [rdi+0x2d18],-2
    e.jmp_abs(b64 + kTownCheckFn);                                     // orig: tail-jump to 54b9d0
    const size_t nt = e.p - T;
    if (!fits_rel32(P, ans + 5) || !fits_rel32(T, town + 5)) {
        std::fprintf(stderr, "skip_offers: relay out of rel32 range; no party return\n");
        return;
    }
    if (const char* d = std::getenv("DCP_SKIP_DUMP"); d && *d && *d != '0') {
        auto hex = [](const char* n, const uint8_t* p, size_t len) {
            std::fprintf(stderr, "skip_offers: relay %s ", n);
            for (size_t i = 0; i < len; ++i) std::fprintf(stderr, "%02x", p[i]);
            std::fprintf(stderr, "\n");
        };
        hex("P", P, np), hex("T", T, nt);
    }
    uint8_t ja[14] = {0xe9, 0, 0, 0, 0, 0x66, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};  // jmp P; 9-byte nop
    put_rel32(ja + 1, P, ans + 5);
    write_code(ans, ja, sizeof ja);
    uint8_t ct[5] = {0xe8};
    put_rel32(ct + 1, T, town + 5);
    write_code(town, ct, sizeof ct);
    std::fprintf(stderr, "skip_offers: online \"Return to town\" follows the host and keeps the party (eboot+0x%x, +0x%x)\n",
                 kAnswer, kTownCheck);
}
}  // namespace

void install(uint8_t* base) {
    for (const Patch& p : kPatches) {
        if (off(p.env)) continue;  // off: game code untouched
        uint8_t* at = base + p.site;
        if (std::memcmp(at, p.expect, p.len) != 0) {
            std::fprintf(stderr, "skip_offers: unexpected bytes at eboot+0x%x; %s not installed\n", p.site, p.what);
            continue;
        }
        write_code(at, p.repl, p.len);
        std::fprintf(stderr, "skip_offers: %s (eboot+0x%x)\n", p.what, p.site);
    }
    if (!off("DCP_STORY_SKIP") && !off("DCP_SKIP_WARNING")) install_warning(base);
    if (!off("DCP_PARTY_RETURN")) install_party_return(base);
}

}  // namespace dcp::skip_offers
