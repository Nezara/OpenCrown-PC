// dcp_run <eboot.bin> [--dry] [--input script.txt] [--frames N] [--render] [--window] [--fullscreen] [--size WxH] [--shot out.png] [--renderer auto|gpu|gl]: load the user's eboot (and its sce_module/libc.prx, if present) into this process,
// relocate, bind imports to the user's libc exports, HLE shims or logging stubs, and (without --dry) run libc's
// module_start and the eboot entry until the first fault / exit, reporting where and why.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <map>
#include <utility>
#include <vector>
#include <string>
#include <mutex>

#include <dirent.h>
#include <initializer_list>
#include <pthread.h>
#include <strings.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>

#include "hle/hle.h"
#include "hle/ime.h"
#include "hle/confirm_button.h"
#include "hle/skip_offers.h"
#include "loader/elf.h"
#include "loader/hle.h"
#include "render/capture.h"
#ifdef DCP_HAVE_RENDER
#include "../common/input_defaults.h"
#include "render_loop.h"
#include "sdl_audio.h"
#include "sdl_input.h"
#endif

using namespace dcp::loader;

namespace {

struct Loaded { std::string name; ElfInfo info; Image img; std::vector<uint8_t> elf; };
std::vector<Loaded*> g_mods;  // [0] = eboot

// dir/a/b with each component matched without case when the exact name is missing (a dump copied from a
// case-insensitive filesystem may spell it SCE_MODULE/LIBC.PRX; /app0 lookups do the same in hle/kernel/file.cpp).
std::string find_ci(std::string dir, std::initializer_list<const char*> comps) {
    for (const char* c : comps) {
        std::string hit = c;
        if (access((dir + "/" + c).c_str(), F_OK) != 0)
            if (DIR* d = opendir(dir.c_str())) {
                while (dirent* e = readdir(d))
                    if (strcasecmp(e->d_name, c) == 0) { hit = e->d_name; break; }
                closedir(d);
            }
        dir += "/" + hit;
    }
    return dir;
}

const char* reloc_name(uint32_t t) {
    switch (t) {
    case kRRelative: return "RELATIVE";
    case kR64: return "64";
    case kRGlobDat: return "GLOB_DAT";
    case kRJumpSlot: return "JUMP_SLOT";
    case kRDtpMod64: return "DTPMOD64";
    case kRDtpOff64: return "DTPOFF64";
    default: return "?";
    }
}

[[noreturn]] void finish(int code) {
    std::fflush(stdout);
    std::fflush(stderr);
    _exit(code);  // ends every thread (exit_group); game threads may hold locks, so skip atexit / static teardown
}

void print_addr(const char* what, uint64_t a) {
    for (const Loaded* m : g_mods) {
        uint64_t b = reinterpret_cast<uint64_t>(m->img.base);
        if (a >= b && a < b + m->img.size) {
            std::fprintf(stderr, "%s %s+0x%llx", what, m->name.c_str(), (unsigned long long)(a - b));
            return;
        }
    }
    std::string stub;
    if (stub_name_at(a, &stub)) std::fprintf(stderr, "%s stub %s", what, stub.c_str());
    else std::fprintf(stderr, "%s host 0x%llx", what, (unsigned long long)a);
}

// Probes (DCP_PROBE=258e0,53c9b0*,...: hex eboot vaddrs, i.e. q.py addresses): an int3 on the first byte. A hit
// logs the address, the frame clock, rdi/rsi (as a string when they point at text) and the caller. Plain entries fire
// once; entries ending in '*' log every hit (re-armed after a single step; hits on other threads in that window are
// missed). Debug aid only.
struct Probe { uint64_t addr; uint8_t orig; bool every = false; std::atomic<bool> hit{false}; };
thread_local uint64_t t_rearm = 0;
Probe g_probes[64];
int g_nprobes = 0;
std::mutex g_probe_lock;

void patch_byte(uint64_t a, uint8_t v) {  // probes sit in r-x text; x86 needs no instruction cache flush
    void* pg = reinterpret_cast<void*>(a & ~0xfffull);
    mprotect(pg, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC);
    *reinterpret_cast<volatile uint8_t*>(a) = v;
    mprotect(pg, 0x1000, PROT_READ | PROT_EXEC);
}

// Copies n bytes at host address a without faulting (process_vm_readv on ourselves fails on unmapped memory).
bool safe_read(uint64_t a, void* out, size_t n) {
    iovec local{out, n}, remote{reinterpret_cast<void*>(a), n};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == ssize_t(n);
}

void install_probes(uint64_t base) {
    const char* env = std::getenv("DCP_PROBE");
    for (const char* p = env; p && *p && g_nprobes < 64;) {
        char* end = nullptr;
        uint64_t off = std::strtoull(p, &end, 16);
        if (end == p) break;
        Probe& pr = g_probes[g_nprobes++];
        pr.addr = base + off;
        if (*end == '*') pr.every = true, ++end;
        pr.orig = *reinterpret_cast<uint8_t*>(pr.addr);
        patch_byte(pr.addr, 0xCC);
        p = *end == ',' ? end + 1 : end;
    }
    if (g_nprobes) std::fprintf(stderr, "dcp_run: %d probes armed\n", g_nprobes);
}

std::string text_at(uint64_t a) {  // printable ASCII string at a readable address, else ""
    char buf[64];
    if (a < 0x10000 || !safe_read(a, buf, sizeof buf)) return "";
    std::string r;
    for (const char* p = buf; *p && r.size() < 48; ++p) {
        if (*p < 0x20 || *p > 0x7e) return "";
        r += *p;
    }
    return r.size() >= 2 ? " '" + r + "'" : "";
}

using Regs = greg_t*;  // ucontext general registers (REG_RIP, REG_RAX, ...)
#define R64(r) (unsigned long long)c[REG_##r]

bool handle_probe(Regs c) {
    uint64_t at = uint64_t(c[REG_RIP]) - 1;  // Linux reports the address after the int3
    for (int i = 0; i < g_nprobes; ++i) {
        Probe& pr = g_probes[i];
        if (pr.addr != at) continue;
        std::lock_guard<std::mutex> lk(g_probe_lock);
        if (*reinterpret_cast<volatile uint8_t*>(at) == 0xCC && (pr.every || !pr.hit.exchange(true))) {
            patch_byte(pr.addr, pr.orig);
            uint64_t ret = *reinterpret_cast<uint64_t*>(c[REG_RSP]);
            std::fprintf(stderr, "PROBE eboot+0x%llx vblank %llu thread %ld rdi=%llx%s rsi=%llx%s",
                         (unsigned long long)(at - reinterpret_cast<uint64_t>(g_mods[0]->img.base)),
                         (unsigned long long)dcp::hle::vblank_count_public(), long(gettid()), R64(RDI),
                         text_at(uint64_t(c[REG_RDI])).c_str(), R64(RSI), text_at(uint64_t(c[REG_RSI])).c_str());
            print_addr(" ret", ret);
            std::fprintf(stderr, "\n");
            if (pr.every) t_rearm = pr.addr, c[REG_EFL] |= 0x100;  // trap after one instruction, then re-arm
        }
        if (*reinterpret_cast<volatile uint8_t*>(at) != 0xCC) c[REG_RIP] = greg_t(at);  // run the restored instruction
        return true;
    }
    return false;
}

const char* sig_name(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGTRAP: return "SIGTRAP";
    default: return "?";
    }
}

void on_fault(int sig, siginfo_t* si, void* ucv) {
    Regs c = static_cast<ucontext_t*>(ucv)->uc_mcontext.gregs;
    if (sig == SIGTRAP && t_rearm) {  // single step after a repeating probe
        {
            std::lock_guard<std::mutex> lk(g_probe_lock);
            patch_byte(t_rearm, 0xCC);
        }
        t_rearm = 0;
        c[REG_EFL] &= ~greg_t(0x100);
        return;
    }
    if (sig == SIGTRAP && g_nprobes && handle_probe(c)) return;
    std::fprintf(stderr, "\nFAULT %s code %d (thread %ld)", sig_name(sig), si->si_code, long(gettid()));
    print_addr(" at", uint64_t(c[REG_RIP]));
    if ((sig == SIGSEGV || sig == SIGBUS) && si->si_code > 0) {  // a real fault (not kill -SEGV)
        // x86-64 page-fault error code: bit 1 = write, bit 4 = instruction fetch.
        uint64_t err = uint64_t(c[REG_ERR]);
        std::fprintf(stderr, " (%s of 0x%llx)", (err & 16) ? "exec" : (err & 2) ? "write" : "read",
                     (unsigned long long)reinterpret_cast<uint64_t>(si->si_addr));
    }
    std::fprintf(stderr, "\n  bytes:");
    uint8_t code[16];
    if (safe_read(uint64_t(c[REG_RIP]), code, sizeof code))
        for (uint8_t b : code) std::fprintf(stderr, " %02x", b);
    std::fprintf(stderr, "\n  rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx\n  rbp=%llx rsp=%llx r8=%llx r9=%llx "
                         "r12=%llx r13=%llx r14=%llx r15=%llx\n",
                 R64(RAX), R64(RBX), R64(RCX), R64(RDX), R64(RSI), R64(RDI), R64(RBP), R64(RSP), R64(R8), R64(R9),
                 R64(R12), R64(R13), R64(R14), R64(R15));
    // Frame-pointer walk: the game is built with rbp frames (push rbp; mov rbp, rsp).
    uint64_t bp = uint64_t(c[REG_RBP]);
    for (int i = 0; i < 16 && bp && !(bp & 7); ++i) {
        uint64_t fr[2];
        if (!safe_read(bp, fr, sizeof fr)) break;
        print_addr("  caller", fr[1]);
        std::fprintf(stderr, "\n");
        bp = fr[0];
    }
    auto calls = recent_unimpl_calls();
    std::fprintf(stderr, "  last %zu stub calls (oldest first):\n", calls.size());
    for (const auto& s : calls) std::fprintf(stderr, "    %s\n", s.c_str());
    finish(3);
}

void install_fault_handler() {  // every thread runs it on its own alternate stack (dcp::hle::install_signal_stack)
    struct sigaction sa = {};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP}) sigaction(sig, &sa, nullptr);
}

void on_exit(int status) {
    std::fprintf(stderr, "game exited (%d); last stub calls:\n", status);
    for (const auto& s : recent_unimpl_calls()) std::fprintf(stderr, "    %s\n", s.c_str());
    finish(status & 0xFF);
}

DCP_SYSV void on_exit_sysv(int status) { on_exit(status); }
DCP_SYSV void game_exit_cb() { std::fprintf(stderr, "game exit callback called\n"); }

// PS4 entry: rdi -> {int argc; u32 pad; char* argv[]...}, rsi -> exit/atexit callback (see README for the guesses).
struct EntryParams { int32_t argc; uint32_t pad; const char* argv[4]; };
EntryParams g_params;
uint64_t g_entry, g_libc_start;

void* game_thread(void*) {
    dcp::hle::install_signal_stack();
    if (g_libc_start) {  // module_start(argc, argv, callback): runs libc's preinit/init arrays (guess: args unused)
        using Start = DCP_SYSV int (*)(uint64_t, void*, void*);
        int r = reinterpret_cast<Start>(g_libc_start)(0, nullptr, nullptr);
        std::fprintf(stderr, "libc module_start -> %d\n", r);
    }
    using Entry = DCP_SYSV void (*)(EntryParams*, void (*)());
    reinterpret_cast<Entry>(g_entry)(&g_params, reinterpret_cast<void (*)()>(&game_exit_cb));
    std::fprintf(stderr, "entry returned\n");
    return nullptr;
}

Loaded* load(const std::string& path, const std::string& name) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    Loaded* m = new Loaded{name, {}, {}, {}};
    m->elf = unwrap_self({std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()});
    m->info = parse_elf(m->elf);
    g_mods.push_back(m);
    return m;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: dcp_run <eboot.bin> [--dry] [--input script.txt] [--frames N]\n"
                             "               [--render] [--window] [--fullscreen] [--size WxH] [--4k]\n"
                             "               [--shot out.png] [--shots N=a.png,M=b.png] [--renderer auto|gpu|gl]\n"
                             "  --render: replay the game's frames (hidden window); --window shows them;\n"
                             "  --renderer: auto = SDL GPU (Vulkan), OpenGL if that fails (default; env\n"
                             "  DCP_RENDERER when the argument is absent); gpu = SDL GPU or fail; gl = OpenGL 3.3\n"
                             "  --fullscreen: borderless desktop fullscreen (implies --window); --size: window size (1280x720)\n"
                             "  --4k: PS4 Pro mode, the game renders at 3840x2160\n"
                             "  --shot writes the frame at vblank --frames N and exits; --shots several (vblank=path)\n"
                             "  user dir (input.ini, saves/): ${XDG_DATA_HOME:-~/.local/share}/OpenCrown (DCP_USER_DIR\n"
                             "  overrides; DCP_SAVE_ROOT overrides the saves folder alone)\n");
        return 2;
    }
    bool dry = false;
    std::string input_path;
    uint64_t max_frames = 0;
    bool render = false, window = false, fullscreen = false, neo = false;
    int width = 1280, height = 720;
    std::string shot, renderer;
    std::vector<std::pair<uint64_t, std::string>> shots;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dry") == 0) dry = true;
        else if (std::strcmp(argv[i], "--input") == 0 && i + 1 < argc) input_path = argv[++i];
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) max_frames = std::strtoull(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--render") == 0) render = true;
        else if (std::strcmp(argv[i], "--window") == 0) render = window = true;
        else if (std::strcmp(argv[i], "--fullscreen") == 0) render = window = fullscreen = true;
        else if (std::strcmp(argv[i], "--4k") == 0) neo = true;
        else if (std::strcmp(argv[i], "--renderer") == 0 && i + 1 < argc) {
            renderer = argv[++i];
            if (renderer != "auto" && renderer != "gpu" && renderer != "gl") {
                std::fprintf(stderr, "dcp_run: bad --renderer (auto, gpu or gl)\n");
                return 2;
            }
        }
        else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%dx%d", &width, &height) != 2 || width < 320 || height < 180 || width > 16384 ||
                height > 16384) {
                std::fprintf(stderr, "dcp_run: bad --size (WxH, e.g. 1920x1080)\n");
                return 2;
            }
        }
        else if (std::strcmp(argv[i], "--shot") == 0 && i + 1 < argc) render = true, shot = argv[++i];
        else if (std::strcmp(argv[i], "--shots") == 0 && i + 1 < argc) {
            render = true;
            std::string l = argv[++i];
            for (size_t p = 0; p < l.size();) {
                size_t c = l.find(',', p), colon = l.find('=', p);
                if (c == std::string::npos) c = l.size();
                if (colon == std::string::npos || colon > c) { std::fprintf(stderr, "dcp_run: bad --shots\n"); return 2; }
                shots.emplace_back(std::strtoull(l.substr(p, colon - p).c_str(), nullptr, 10), l.substr(colon + 1, c - colon - 1));
                p = c + 1;
            }
        }        else {
            std::fprintf(stderr, "dcp_run: unknown argument '%s'\n", argv[i]);
            return 2;
        }
    }
    try {
        std::string eboot_path = argv[1];
        size_t slash = eboot_path.find_last_of("/\\");
        std::string dir = slash == std::string::npos ? "." : eboot_path.substr(0, slash);
        Loaded* eb = load(eboot_path, "eboot");
        // The user's own libc.prx (decrypted like the eboot) provides libc; without it libc imports stay stubs.
        std::string libc_path = find_ci(dir, {"sce_module", "libc.prx"});
        Loaded* libc = access(libc_path.c_str(), F_OK) == 0 ? load(libc_path, "libc") : nullptr;

        eb->img = map_image(eb->info, eb->elf);
        if (libc) libc->img = map_image(libc->info, libc->elf, 0);
        for (Loaded* m : g_mods) {
            if (m->info.tls.memsz)
                m->img.tls_modid = dcp::hle::add_tls_module(m->img.base + m->info.tls.vaddr, m->info.tls.filesz,
                                                            m->info.tls.memsz, m->info.tls_align);
            if (m == eb) continue;
            std::map<std::string, uint64_t> exports;  // global / weak defined "nid#lib#mod" symbols
            for (const Symbol& s : m->info.syms)
                if (s.shndx && s.name.find('#') != std::string::npos && ((s.info >> 4) == 1 || (s.info >> 4) == 2))
                    exports[s.name.substr(0, s.name.find('#'))] = reinterpret_cast<uint64_t>(m->img.base) + s.value;
            add_module_exports(exports);
        }
        dcp::hle::Config cfg;
        cfg.app0 = dir;
        cfg.proc_param = eb->info.proc_param ? reinterpret_cast<uint64_t>(eb->img.base) + eb->info.proc_param : 0;
        cfg.eboot_base = reinterpret_cast<uint64_t>(eb->img.base);
        cfg.on_exit = &on_exit;
        cfg.save_root = dcp::hle::default_save_root();
        if (!shot.empty()) shots.emplace_back(max_frames, shot);
        std::sort(shots.begin(), shots.end());
        cfg.max_frames = render && !shots.empty() ? 0 : max_frames;
        cfg.neo = neo;  // with shots the render loop decides when to stop
        if (!input_path.empty()) {
            std::ifstream in(input_path, std::ios::binary);
            if (!in) throw std::runtime_error("cannot open input script " + input_path);
            cfg.input_script.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            if (cfg.input_script.empty()) cfg.input_script = "\n";  // empty file = explicit idle script
        }
        dcp::hle::register_all(cfg);
        if (!libc) register_hle("libc", "exit", reinterpret_cast<void*>(&on_exit_sysv));

        Binder binder;
        RelocStats rs;
        for (Loaded* m : g_mods) {  // order does not matter: every export is known before relocation
            RelocStats r = relocate(m->img, m->info, [&](const Symbol& s) { return binder.resolve(s); });
            for (auto [t, n] : r.by_type) rs.by_type[t] += n;
            for (auto [t, n] : r.unsupported) rs.unsupported[t] += n;
            protect_image(m->img, m->info);
        }
        binder.finalize();

        const BindStats& bs = binder.stats();
        std::string rel;
        for (auto [t, n] : rs.by_type) rel += std::string(rel.empty() ? "" : " ") + reloc_name(t) + "=" + std::to_string(n);
        for (auto [t, n] : rs.unsupported) rel += " UNSUPPORTED(" + std::to_string(t) + ")=" + std::to_string(n);
        for (Loaded* m : g_mods)
            std::printf("%s: base 0x%llx, %zu segments, image 0x%llx, tls 0x%llx\n", m->name.c_str(),
                        (unsigned long long)reinterpret_cast<uint64_t>(m->img.base), m->info.load.size(),
                        (unsigned long long)m->img.size, (unsigned long long)m->info.tls.memsz);
        std::printf("relocs: %s; imports: %zu module exports + %zu hle + %zu stubs + %zu data, %zu unknown NIDs; "
                    "entry eboot+0x%llx\n",
                    rel.c_str(), bs.exported, bs.hle, bs.stubs, bs.data, bs.unknown, (unsigned long long)eb->info.entry);
        if (dry) return rs.unsupported.empty() ? 0 : 1;  // unknown NIDs are expected (unnamed NpToolkit2 C++ API)

        std::fflush(stdout);
        g_params = {1, 0, {"eboot.bin", nullptr, nullptr, nullptr}};
        g_entry = reinterpret_cast<uint64_t>(eb->img.base) + eb->info.entry;
        if (libc) g_libc_start = reinterpret_cast<uint64_t>(libc->img.base) + libc->info.entry;
        dcp::hle::install_signal_stack();
        install_fault_handler();
        install_probes(reinterpret_cast<uint64_t>(eb->img.base));
        dcp::skip_offers::install(eb->img.base);  // story / tutorial skip questions for every new hero (skip_offers.h)
        dcp::confirm_button::install(eb->img.base);  // DCP_CONFIRM=circle: Japanese button layout (confirm_button.h)
#ifndef DCP_HAVE_RENDER
        if (render) throw std::runtime_error("--render needs a build with -DDCP_RENDER=ON (the default)");
#else
        if (render && !dcp::render::install_hooks(eb->img.base)) throw std::runtime_error("render hooks failed");
        if (render && window && !std::getenv("DCP_NO_AUDIO")) install_sdl_audio();  // before the game thread (sink swap)
        SdlInputPump* input = nullptr;  // live input only in a visible window without --input (scripted runs stay deterministic)
        std::string input_dir;
        {
            std::string dir = dcp::hle::default_save_root();  // ...\Dragons Crown Pro\saves -> its parent
            const char* user = std::getenv("DCP_USER_DIR");
            input_dir = user && *user ? dcp::user_dir() : dir.substr(0, dir.find_last_of("/\\"));
        }
        // DCP_TOUCH_TEST (render_loop.cpp): live input also in hidden / scripted runs, on top of the script.
        const bool touch_test = std::getenv("DCP_TOUCH_TEST") != nullptr;
        if (render && ((window && input_path.empty()) || touch_test)) input = install_sdl_input(input_dir, touch_test);
        if (input && !touch_test) dcp::hle::ime_set_host(true);  // the render loop draws the name-entry box (ime_box.h)
#endif
        // Own thread so the game gets a large stack (PS4 main thread: guess 8 MiB; 16 MiB here). One mapping with the
        // guard page at the bottom only, so frames > 4 KiB (game code has no stack probes) are fine.
        pthread_t t;
        pthread_attr_t pa;
        pthread_attr_init(&pa);
        pthread_attr_setstacksize(&pa, 16u << 20);
        int err = pthread_create(&t, &pa, game_thread, nullptr);
        pthread_attr_destroy(&pa);
        if (err) throw std::runtime_error("pthread_create failed");
#ifdef DCP_HAVE_RENDER
        if (render) {
            RenderOptions ro;
            ro.window = window;
            ro.fullscreen = fullscreen;
            ro.width = width, ro.height = height;
            ro.shots = shots;
            ro.input = input;
            ro.input_dir = input_dir;
            if (renderer.empty()) {  // DCP_RENDERER only when --renderer is absent
                const char* e = std::getenv("DCP_RENDERER");
                renderer = e && *e ? e : "auto";
            }
            ro.renderer = renderer;
            finish(run_render_loop(ro, t, &finish));
        }
#endif
        pthread_join(t, nullptr);
        finish(0);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "dcp_run: %s\n", e.what());
        return 1;
    }
}
