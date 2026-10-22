#include "guest/memory.h"

#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace guest {

constexpr uint64_t SPACE = 1ull << 32;
// Never-committed tail past 4 GiB: the JIT's fastmem forms base + 32-bit address, so a wide access at the very top
// (e.g. a 64-bit load at 0xfffffffc) runs past the space and must still land in our no-access reservation.
constexpr uint64_t TAIL = 0x10000;

bool Memory::init(std::string& err) {
#ifdef _WIN32
  base_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, SPACE + TAIL, MEM_RESERVE, PAGE_NOACCESS));
#else
  void* p = mmap(nullptr, SPACE + TAIL, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  base_ = p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
#endif
  if (!base_) return err = "cannot reserve the 4 GiB guest address space", false;
  table_ = std::make_unique<PageTable>();
  table_->fill(nullptr);
  return true;
}

Memory::~Memory() {
  if (!base_) return;
#ifdef _WIN32
  VirtualFree(base_, 0, MEM_RELEASE);
#else
  munmap(base_, SPACE + TAIL);
#endif
}

bool Memory::map(uint32_t va, uint32_t size) {
  if (size == 0) return true;
  uint64_t lo = va & ~uint64_t(PAGE - 1), hi = (uint64_t(va) + size + PAGE - 1) & ~uint64_t(PAGE - 1);
  if (hi > SPACE) return false;
#ifdef _WIN32
  if (!VirtualAlloc(base_ + lo, hi - lo, MEM_COMMIT, PAGE_READWRITE)) return false;
#else
  if (mprotect(base_ + lo, hi - lo, PROT_READ | PROT_WRITE) != 0) return false;
#endif
  for (uint64_t a = lo; a < hi; a += PAGE) (*table_)[a >> 12] = base_ + a;
  return true;
}

uint32_t Memory::alloc(uint32_t size, const char* what) {
  size = (size + PAGE - 1) & ~(PAGE - 1);
  std::lock_guard lk(alloc_mu_);
  uint32_t va = next_alloc_;
  if (uint64_t(va) + size > 0xF0000000ull || !map(va, size)) {
    std::fprintf(stderr, "[mem] out of guest memory allocating %s (0x%x)\n", what, size);
    return 0;
  }
  next_alloc_ += size + PAGE;  // one unmapped guard page between blocks
  return va;
}

bool Memory::valid(uint32_t va, uint32_t n) const {
  if (n == 0) return true;
  if (uint64_t(va) + n > SPACE) return false;
  for (uint64_t a = va & ~uint64_t(PAGE - 1); a < uint64_t(va) + n; a += PAGE)
    if (!(*table_)[a >> 12]) return false;
  return true;
}

bool Memory::copy_in(uint32_t va, const void* src, uint32_t n) {
  if (!valid(va, n)) return false;
  std::memcpy(base_ + va, src, n);  // contiguous host reservation: a mapped range is one host range
  return true;
}

std::string Memory::cstr(uint32_t va, size_t max) const {
  std::string s;
  for (size_t i = 0; i < max; i++) {
    const uint8_t* p = ptr(va + uint32_t(i));
    if (!p || !*p) break;
    s += char(*p);
  }
  return s;
}

}  // namespace guest
