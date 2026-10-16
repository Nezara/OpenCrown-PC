// SHA-1 and the PS4 symbol NID scheme (11-char modified base64 of the first 8 SHA-1 bytes of name + fixed suffix).
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace dcp::loader {

std::array<uint8_t, 20> sha1(const void* data, size_t size);
std::string nid_from_name(std::string_view name);

}  // namespace dcp::loader
