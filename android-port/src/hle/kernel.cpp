// SceLibKernel / SceSysmem: module loading, memory blocks, process time, sceClibMemset.
#include <chrono>
#include <cstring>
#include <map>
#include <string>

#include "hle/kobj.h"

namespace hle {
namespace {

struct MemBlock {
  std::string name;
  uint32_t type, base, size;
};
std::map<uint32_t, MemBlock> memblocks;  // kernel_lock()

const auto t0 = std::chrono::steady_clock::now();

// --- modules -------------------------------------------------------------------------------------------------
// sceKernelLoadStartModule(path, args, argp, flags, option*, status*): we never load PRX code. Every module the game
// asks for is either ours (HLE) or, for app0:sce_module/steroid.suprx, a dumper's loader that the dumper's
// module_start shim starts before jumping to the real entry. Report success with a fake id.
void LoadStartModule(Cpu& c) {
  std::string path = str(c, arg(c, 0));
  uint32_t status = arg(c, 5);
  HLE_NOTE("sceKernelLoadStartModule(\"%s\"): not loaded (our own code or a dumper loader)\n", path.c_str());
  if (status) c.mem().write<uint32_t>(status, 0);
  ret(c, new_uid());
}

// --- memory blocks -------------------------------------------------------------------------------------------
// sceKernelAllocMemBlock(name, type, size, opt*). Sizes must be page multiples; CDRAM / phycont types need larger
// alignment on hardware (256 KiB / 1 MiB), which only matters for the GPU side later.
void AllocMemBlock(Cpu& c) {
  std::string name = str(c, arg(c, 0), 32);
  uint32_t type = arg(c, 1), size = arg(c, 2);
  if (size == 0 || (size & 0xfff)) return ret(c, SCE_KERNEL_ERROR_INVALID_ARGUMENT);
  uint32_t base = c.mem().alloc(size, "sceKernelAllocMemBlock");
  if (!base) return ret(c, SCE_KERNEL_ERROR_NO_MEMORY);
  std::lock_guard lk(kernel_lock());
  uint32_t uid = new_uid();
  memblocks[uid] = {name, type, base, size};
  HLE_NOTE("sceKernelAllocMemBlock(\"%s\", type 0x%08x, 0x%x) -> uid 0x%08x base 0x%08x\n", name.c_str(), type, size,
           uid, base);
  ret(c, uid);
}

void GetMemBlockBase(Cpu& c) {
  std::lock_guard lk(kernel_lock());
  auto it = memblocks.find(arg(c, 0));
  if (it == memblocks.end()) return ret(c, SCE_KERNEL_ERROR_UNKNOWN_UID);
  c.mem().write<uint32_t>(arg(c, 1), it->second.base);
  ret(c, 0);
}

void FreeMemBlock(Cpu& c) {
  std::lock_guard lk(kernel_lock());
  auto it = memblocks.find(arg(c, 0));
  if (it == memblocks.end()) return ret(c, SCE_KERNEL_ERROR_UNKNOWN_UID);
  HLE_NOTE("sceKernelFreeMemBlock(\"%s\"): address space not reused yet\n", it->second.name.c_str());
  memblocks.erase(it);
  ret(c, 0);
}

// --- misc ----------------------------------------------------------------------------------------------------
// SceUInt64 sceKernelGetProcessTimeWide(void): microseconds since process start.
void GetProcessTimeWide(Cpu& c) {
  auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
  ret64(c, uint64_t(us));
}

void ClibMemset(Cpu& c) {
  uint32_t dst = arg(c, 0), n = arg(c, 2);
  if (!c.mem().valid(dst, n)) return c.set_fault("sceClibMemset: bad range");
  std::memset(c.mem().ptr(dst), int(arg(c, 1)), n);
  ret(c, dst);
}

void Idiv0(Cpu& c) { c.set_fault("integer division by zero (__sce_aeabi_idiv0)"); }

// Crash-dump registration and power ticks have nothing to do here.
void Coredump(Cpu& c) { ret(c, 0); }
void PowerTick(Cpu& c) { ret(c, 0); }

// SceLibstdcxx's throw helpers (std::length_error / std::out_of_range): the game has no handler, so this ends it.
void XlengthError(Cpu& c) { c.set_fault("std::length_error: " + str(c, arg(c, 0), 128)); }
void XoutOfRange(Cpu& c) { c.set_fault("std::out_of_range: " + str(c, arg(c, 0), 128)); }

}  // namespace

void register_kernel() {
  REG(sceKernelLoadStartModule, LoadStartModule);
  REG(sceKernelAllocMemBlock, AllocMemBlock);
  REG(sceKernelGetMemBlockBase, GetMemBlockBase);
  REG(sceKernelFreeMemBlock, FreeMemBlock);
  REG(sceKernelGetProcessTimeWide, GetProcessTimeWide);
  REG(sceClibMemset, ClibMemset);
  REG(__sce_aeabi_idiv0, Idiv0);
  REG(sceCoredumpRegisterCoredumpHandler, Coredump);
  REG(sceCoredumpWriteUserData, Coredump);
  REG(sceKernelPowerTick, PowerTick);
  REG(_ZSt14_Xlength_errorPKc, XlengthError);
  REG(_ZSt14_Xout_of_rangePKc, XoutOfRange);
}

}  // namespace hle
