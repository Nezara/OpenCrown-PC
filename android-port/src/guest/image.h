// Reads the user's Vita executable: a decrypted SELF (SCE header kept, plain or zlib-compressed segments) or a
// plain ELF.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace guest {

enum : uint16_t {
  ET_SCE_EXEC = 0xfe00,     // fixed address
  ET_SCE_RELEXEC = 0xfe04,  // relocatable: not supported yet
};

struct Segment {
  uint32_t vaddr = 0;
  uint32_t memsz = 0;
  uint32_t flags = 0;  // ELF p_flags: 1 X, 2 W, 4 R
  std::vector<uint8_t> data;  // p_filesz bytes; the rest of memsz is zero
};

struct Image {
  uint16_t type = 0;
  uint32_t entry = 0;  // Vita: segment index in bits 31:30, offset of sceModuleInfo in that segment in bits 29:0
  std::vector<Segment> segments;  // PT_LOAD only, in file order
};

// false + err on anything we can't use (encrypted / compressed / relocatable / not ARM).
bool load_image(const std::string& path, Image& out, std::string& err);

}  // namespace guest
