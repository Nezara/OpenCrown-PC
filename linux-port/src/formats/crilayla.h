// CRILAYLA: CRI's backwards LZ compression used for packed CPK entries.
#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace dcp::formats {

bool is_crilayla(std::span<const uint8_t> in);

// Returns the 0x100-byte raw prefix followed by the decompressed body. Throws on malformed input.
std::vector<uint8_t> crilayla_decompress(std::span<const uint8_t> in);

}  // namespace dcp::formats
