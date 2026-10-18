// libSceGnmDriver without a GPU. Two jobs, enough for the game's CPU side to keep running:
// 1. Packet-emitting entry points (Set*Shader, Draw*, Dispatch*, InsertWaitFlipDone) own `size` dwords of the
//    caller's command buffer; they are filled with one PM4 NOP so the stream stays well-formed.
// 2. Submits walk the submitted DCBs and perform only the memory writes the CPU waits on: EVENT_WRITE_EOP,
//    RELEASE_MEM and WRITE_DATA (to memory). The RenderDLThread spins on such labels after every submit (engine.md 3);
//    GPU addresses are CPU addresses here (direct memory is mapped 1:1). Flips are counted ("FRAME" line / 60).
#include <atomic>
#include <cerrno>
#include <cstring>

#include <sys/uio.h>
#include <unistd.h>

#include "common.h"
#include "render/capture.h"

namespace dcp::hle {
namespace {

std::atomic<uint64_t> g_flips{0}, g_submits{0}, g_labels{0};

// The DCB start is readable (one probe byte, without faulting): stale / garbage submits are skipped.
bool mapped(const void* p) {
    uint8_t b;
    iovec local{&b, 1}, remote{const_cast<void*>(p), 1};
    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == 1) return true;
    return errno == ENOSYS || errno == EPERM;  // syscall filtered: assume mapped
}

// Checked write: process_vm_writev on ourselves fails (EFAULT) on unmapped or read-only memory instead of faulting,
// so a label address that is unmapped / read-only (stale or garbage packet) is skipped.
void mem_write(uint64_t addr, uint64_t value, int bytes) {
    iovec local{&value, size_t(bytes)}, remote{reinterpret_cast<void*>(addr), size_t(bytes)};
    ssize_t r = addr ? process_vm_writev(getpid(), &local, 1, &remote, 1, 0) : -1;
    if (r < 0 && addr && (errno == ENOSYS || errno == EPERM)) {  // syscall filtered (some sandboxes): write unchecked
        std::memcpy(reinterpret_cast<void*>(addr), &value, size_t(bytes));
        r = bytes;
    }
    if (r != ssize_t(bytes)) {
        DCP_HLE_LOG("gnm pm4", "label write to unmapped 0x%llx skipped", (unsigned long long)addr);
        return;
    }
    ++g_labels;
}

uint64_t gpu_clock() { return host_ns() / 10; }  // 100 MHz-style counter for data_sel 3/4

// data_sel: 1 = 32-bit data, 2 = 64-bit data, 3 = GPU clock, 4 = system clock (others write nothing).
void eop_write(uint32_t sel, uint64_t addr, uint32_t lo, uint32_t hi) {
    switch (sel) {
    case 1: mem_write(addr, lo, 4); break;
    case 2: mem_write(addr, uint64_t(hi) << 32 | lo, 8); break;
    case 3: case 4: mem_write(addr, gpu_clock(), 8); break;
    default: break;
    }
}

void run_labels(const uint32_t* d, uint64_t n) {
    if (!d || n > (64u << 20) || !mapped(d)) return;
    for (uint64_t i = 0; i < n;) {
        uint32_t h = d[i], type = h >> 30;
        if (type == 2 || type == 1) { ++i; continue; }
        uint64_t len = ((h >> 16) & 0x3fff) + 2;
        if (type == 3 && i + len <= n) {
            const uint32_t* p = d + i;
            switch ((h >> 8) & 0xff) {
            case 0x10:  // NOP: draw markers left by render capture (--render)
                if (len >= 4 && p[1] == render::kMarkerMagic) render::on_marker(p + 1, uint32_t(len - 1));
                break;
            case 0x47:  // EVENT_WRITE_EOP: cntl, addr_lo, addr_hi|int_sel<<24|data_sel<<29, data_lo, data_hi
                if (len >= 6) eop_write(p[3] >> 29, (uint64_t(p[3] & 0xffff) << 32) | p[2], p[4], p[5]);
                break;
            case 0x49:  // RELEASE_MEM: cntl, data_sel<<29|int_sel<<24, addr_lo, addr_hi, data_lo, data_hi
                if (len >= 7) eop_write(p[2] >> 29, (uint64_t(p[4]) << 32) | p[3], p[5], p[6]);
                break;
            case 0x37: {  // WRITE_DATA: control (dst_sel bits 8..11), addr_lo, addr_hi, data...
                uint32_t dst = (p[1] >> 8) & 0xf;
                if (len >= 5 && (dst == 1 || dst == 2 || dst == 5)) {
                    uint64_t a = (uint64_t(p[3]) << 32) | p[2];
                    for (uint64_t k = 4; k < len; ++k, a += 4) mem_write(a, p[k], 4);
                }
                break;
            }
            default: break;
            }
        }
        i += len;
    }
}

void run_submit(uint32_t count, void* const* dcb, const uint32_t* dcb_bytes) {
    for (uint32_t i = 0; dcb && dcb_bytes && i < count && i < 64; ++i)
        run_labels(static_cast<const uint32_t*>(dcb[i]), dcb_bytes[i] / 4);
}

void count_flip() {
    uint64_t f = ++g_flips;
    if (f % 60 == 0)
        std::fprintf(stderr, "FRAME %llu flips, %llu vblanks, %llu submits, %llu label writes\n", (unsigned long long)f,
                     (unsigned long long)vblank_count(), (unsigned long long)g_submits.load(),
                     (unsigned long long)g_labels.load());
}

void fill_nop(uint32_t* cmd, uint32_t size) {
    if (!cmd || size == 0 || size > 0x4000) return;
    if (size == 1) { cmd[0] = 0x80000000u; return; }  // type-2 filler
    cmd[0] = 0xC0001000u | ((size - 2) << 16);         // type-3 NOP covering size dwords
    std::memset(cmd + 1, 0, (size - 1) * 4);
}

#define DCP_GNM_PACKET(fn)                                                                   \
    DCP_SYSV int32_t fn(uint32_t* cmd, uint32_t size) {                                      \
        DCP_HLE_LOG(#fn, "%u dwords -> NOP (no GPU)", size);                                 \
        fill_nop(cmd, size);                                                                 \
        return kOk;                                                                          \
    }
// Draws: NOP (no GPU); with --render the NOP carries a marker for the captured draw (src/render/capture.h).
DCP_SYSV int32_t sceGnmDrawIndex(uint32_t* cmd, uint32_t size, uint32_t count, uint64_t index_addr, uint32_t, uint32_t) {
    fill_nop(cmd, size);
    render::on_draw(cmd, size, count, index_addr, true);
    return kOk;
}
DCP_SYSV int32_t sceGnmDrawIndexAuto(uint32_t* cmd, uint32_t size, uint32_t count, uint32_t) {
    fill_nop(cmd, size);
    render::on_draw(cmd, size, count, 0, false);
    return kOk;
}
DCP_GNM_PACKET(sceGnmDispatchDirect)
DCP_GNM_PACKET(sceGnmSetVsShader)
DCP_GNM_PACKET(sceGnmSetPsShader350)
DCP_GNM_PACKET(sceGnmSetCsShaderWithModifier)
DCP_GNM_PACKET(sceGnmSetHsShader)
DCP_GNM_PACKET(sceGnmSetLsShader)
DCP_GNM_PACKET(sceGnmUpdateVsShader)
DCP_GNM_PACKET(sceGnmUpdatePsShader350)
DCP_GNM_PACKET(sceGnmUpdateHsShader)
DCP_GNM_PACKET(sceGnmInsertWaitFlipDone)

// Return the number of dwords written: none.
DCP_SYSV uint32_t sceGnmDrawInitDefaultHardwareState350(uint32_t*, uint32_t) { return 0; }
DCP_SYSV uint32_t sceGnmDispatchInitDefaultHardwareState(uint32_t*, uint32_t) { return 0; }
DCP_SYSV void sceGnmDingDong(uint32_t, uint32_t) {}

DCP_SYSV int32_t sceGnmSubmitCommandBuffers(uint32_t count, void* const* dcb, const uint32_t* dcb_bytes, void*, void*) {
    ++g_submits;
    run_submit(count, dcb, dcb_bytes);
    return kOk;
}
DCP_SYSV int32_t sceGnmSubmitCommandBuffersForWorkload(uint64_t, uint32_t count, void* const* dcb,
                                                       const uint32_t* dcb_bytes, void*, void*) {
    ++g_submits;
    run_submit(count, dcb, dcb_bytes);
    return kOk;
}
DCP_SYSV int32_t sceGnmSubmitAndFlipCommandBuffers(uint32_t count, void* const* dcb, const uint32_t* dcb_bytes, void*,
                                                   void*, uint32_t, uint32_t buf, uint32_t, int64_t) {
    DCP_HLE_LOG("sceGnmSubmitAndFlipCommandBuffers", "first flip, display buffer %u", buf);
    ++g_submits;
    run_submit(count, dcb, dcb_bytes);
    count_flip();
    render::on_flip(buf);
    return kOk;
}
DCP_SYSV int32_t sceGnmSubmitAndFlipCommandBuffersForWorkload(uint64_t, uint32_t count, void* const* dcb,
                                                              const uint32_t* dcb_bytes, void*, void*, uint32_t,
                                                              uint32_t, uint32_t buf, uint32_t, int64_t) {
    ++g_submits;
    run_submit(count, dcb, dcb_bytes);
    count_flip();
    render::on_flip(buf);
    return kOk;
}
DCP_SYSV int32_t sceGnmSubmitDone() { return kOk; }

}  // namespace

uint64_t flip_count() { return g_flips.load(); }

void register_gnm() {
    const char* g = "libSceGnmDriver";
    REG(g, sceGnmDrawIndex); REG(g, sceGnmDrawIndexAuto); REG(g, sceGnmDispatchDirect); REG(g, sceGnmSetVsShader);
    REG(g, sceGnmSetPsShader350); REG(g, sceGnmSetCsShaderWithModifier); REG(g, sceGnmSetHsShader);
    REG(g, sceGnmSetLsShader); REG(g, sceGnmUpdateVsShader); REG(g, sceGnmUpdatePsShader350);
    REG(g, sceGnmUpdateHsShader); REG(g, sceGnmInsertWaitFlipDone); REG(g, sceGnmDrawInitDefaultHardwareState350);
    REG(g, sceGnmDispatchInitDefaultHardwareState); REG(g, sceGnmDingDong);
    REG(g, sceGnmSubmitCommandBuffers); REG(g, sceGnmSubmitCommandBuffersForWorkload);
    REG(g, sceGnmSubmitAndFlipCommandBuffers); REG(g, sceGnmSubmitAndFlipCommandBuffersForWorkload);
    REG(g, sceGnmSubmitDone);
}

}  // namespace dcp::hle
