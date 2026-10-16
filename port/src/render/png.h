// Minimal PNG writer (stored deflate blocks, RGB8). Screenshots only.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dcp::render {
// rgb: w*h*3 bytes, top row first.
bool write_png(const std::string& path, int w, int h, const std::vector<uint8_t>& rgb);
}  // namespace dcp::render
