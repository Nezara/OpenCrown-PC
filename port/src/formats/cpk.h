// CRI CPK archive reader. Opens a user-supplied archive in place; nothing is copied or cached.
#pragma once
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace dcp::formats {

struct CpkEntry {
    std::string dir, name;
    uint64_t offset = 0;        // absolute offset in the archive
    uint32_t stored_size = 0;   // FileSize: bytes on disk
    uint32_t extract_size = 0;  // ExtractSize: bytes after decompression
    uint32_t id = 0;
    std::string path() const { return dir.empty() ? name : dir + "/" + name; }
    bool compressed() const { return stored_size != extract_size; }
};

class CpkArchive {
public:
    explicit CpkArchive(const std::filesystem::path& path);  // throws on failure
    ~CpkArchive();
    CpkArchive(const CpkArchive&) = delete;
    CpkArchive& operator=(const CpkArchive&) = delete;

    const std::vector<CpkEntry>& entries() const { return entries_; }
    const CpkEntry* find(const std::string& path) const;
    uint64_t file_size() const { return file_size_; }
    uint32_t align() const { return align_; }

    std::vector<uint8_t> read_raw(const CpkEntry& e) const;  // stored bytes
    std::vector<uint8_t> read(const CpkEntry& e) const;      // decompressed if needed
    // `size` bytes at `off` within an uncompressed entry, without loading the rest (e.g. AWB headers).
    std::vector<uint8_t> read_range(const CpkEntry& e, uint64_t off, size_t size) const;

private:
    std::vector<uint8_t> read_at(uint64_t off, size_t size) const;
    std::vector<uint8_t> read_packet(uint64_t off, const char magic[4]) const;

    std::FILE* f_ = nullptr;
    uint64_t file_size_ = 0;
    uint32_t align_ = 0;
    std::vector<CpkEntry> entries_;
};

}  // namespace dcp::formats
