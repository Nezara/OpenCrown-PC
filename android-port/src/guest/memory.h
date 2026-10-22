// The guest's 32-bit address space: one 4 GiB host reservation, committed per page on map. Every mapped page
// also sits in Dynarmic's page table, so guest loads and stores are plain host accesses; anything unmapped goes to
// the CPU callbacks and is reported as a fault. The reservation doubles as Dynarmic's fastmem region (guest X at
// base()+X): unmapped pages stay no-access, so a fastmem fault falls back to the callbacks the same way.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

namespace guest {

constexpr uint32_t PAGE = 0x1000;

class Memory {
 public:
  using PageTable = std::array<uint8_t*, 1u << 20>;

  bool init(std::string& err);
  ~Memory();

  // Commits [va, va+size) (page-rounded) read/write and enters it in the page table. Already-mapped pages stay.
  bool map(uint32_t va, uint32_t size);
  bool mapped(uint32_t va) const { return table_ && (*table_)[va >> 12] != nullptr; }
  // Host pointer for va, or nullptr when the page is unmapped. Valid up to the end of that page.
  uint8_t* ptr(uint32_t va) const { return mapped(va) ? base_ + va : nullptr; }
  uint8_t* base() const { return base_; }
  PageTable* table() const { return table_.get(); }

  // Guest bump allocator for runtime-owned blocks (stacks, TLS, trampolines), page-aligned, never freed for now.
  // Thread-safe.
  uint32_t alloc(uint32_t size, const char* what);

  // True when every byte of [va, va+n) is mapped; then ptr(va) addresses all n bytes (one host range).
  bool valid(uint32_t va, uint32_t n) const;
  bool copy_in(uint32_t va, const void* src, uint32_t n);
  template <typename T>
  T read(uint32_t va) const {
    T v{};
    if (const uint8_t* p = ptr(va)) std::memcpy(&v, p, sizeof(T));
    return v;
  }
  template <typename T>
  void write(uint32_t va, T v) {
    if (uint8_t* p = ptr(va)) std::memcpy(p, &v, sizeof(T));
  }
  std::string cstr(uint32_t va, size_t max = 256) const;

 private:
  uint8_t* base_ = nullptr;
  std::unique_ptr<PageTable> table_;
  std::mutex alloc_mu_;
  uint32_t next_alloc_ = 0x82000000;  // above the eboot (1.09: 0x81000000..0x81d0c000)
};

}  // namespace guest
