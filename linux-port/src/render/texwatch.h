// Write tracking for guest texture memory, so the GL texture cache can tell when the game rewrote a texture (CPU-updated
// textures such as the libSceFont glyph atlas). After an upload the backend makes the texture's pages read-only; the first
// write from any game thread faults once, a SIGSEGV handler records the write and restores write access. Host APIs that
// write guest memory (read() in the file shim) call host_write() first. Pages stay read-only only until written.
#pragma once
#include <cstdint>

namespace dcp::texwatch {

// Write-protects [base, base+size) (whole pages). Returns false if the memory is not plain committed read-write memory
// (then the caller must fall back to content hashing). Returns the current write sequence in *seq.
bool protect(uint64_t base, uint64_t size, uint64_t* seq);
// True if any byte of the range may have been written (or the memory remapped) since protect() returned seq.
bool written_since(uint64_t base, uint64_t size, uint64_t seq);
// Call before a host API writes [p, p+n) of guest memory: records the write and makes tracked pages writable.
void host_write(const void* p, uint64_t n);
// Current write sequence: a texture found unwritten now can move its seq here (cheap later checks).
uint64_t seq_now();
// Call before guest memory is unmapped: tracked pages in the range count as written and are forgotten.
void forget(uint64_t base, uint64_t size);

}  // namespace dcp::texwatch
