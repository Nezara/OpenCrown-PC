// Read-only view of the user's eboot: a decrypted (fake-signed) PS4 SELF or a plain ELF. Gives the loaded bytes at a
// virtual address, so the port can take constant tables from the user's own copy instead of shipping them.
#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>

namespace dcp::formats {

class EbootImage {
public:
    explicit EbootImage(const std::filesystem::path& path);  // throws if unreadable, encrypted or not an ELF/SELF
    // `size` bytes at virtual address `vaddr`; throws if the range is not inside one file-backed segment.
    std::vector<uint8_t> read(uint64_t vaddr, size_t size) const;

private:
    struct Segment { uint64_t vaddr, filesz; std::vector<uint8_t> bytes; };
    std::vector<Segment> segs_;
};

}  // namespace dcp::formats
