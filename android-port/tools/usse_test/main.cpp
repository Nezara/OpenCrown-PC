// Verify pass of the USSE translator over GXP programs dumped from the user's game (DCP_DUMP_GXP; game files, they
// stay in Work/). Translates each one, validates the SPIR-V with SPIRV-Tools when built with the dev option, and can
// write the combined disassembly listing and the .spv files.
// Usage: usse_test <gxp dir> [--listing <file>] [--spv <dir>] [--dis <program hash>]
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifdef DCP_HAVE_SPIRV_TOOLS
#include <spirv-tools/libspirv.hpp>
#endif

#include "gpu/usse/translator.h"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: usse_test <gxp dir> [--listing <file>] [--spv <dir>]\n");
    return 2;
  }
  std::string listing_path, spv_dir, dis_name;
  for (int i = 2; i + 1 < argc; i += 2) {
    std::string a = argv[i];
    if (a == "--listing") listing_path = argv[i + 1];
    else if (a == "--spv") spv_dir = argv[i + 1];
    else if (a == "--dis") dis_name = argv[i + 1];
  }
  std::vector<fs::path> files;
  for (const auto& e : fs::directory_iterator(argv[1]))
    if (e.path().extension() == ".gxp") files.push_back(e.path());
  std::sort(files.begin(), files.end());

#ifdef DCP_HAVE_SPIRV_TOOLS
  spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
  std::string val_msg;
  tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t& p, const char* m) {
    val_msg += "  word " + std::to_string(p.index) + ": " + m + "\n";
  });
#endif

  std::string listing;
  int failures = 0, validated = 0;
  for (const auto& path : files) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), {});
    gpu::usse::Shader s = gpu::usse::translate(data);
    std::string name = path.stem().string();
    listing += "# " + name + " " + s.listing;
    if (!s.ok) {
      failures++;
      std::printf("FAIL %s: %s\n", name.c_str(), s.error.c_str());
      continue;
    }
#ifdef DCP_HAVE_SPIRV_TOOLS
    val_msg.clear();
    if (!tools.Validate(s.spirv)) {
      failures++;
      std::string dis;
      tools.Disassemble(s.spirv, &dis, SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES);
      std::printf("INVALID %s\n%s%s", name.c_str(), val_msg.c_str(), dis.c_str());
      continue;
    }
    validated++;
    // Vertex programs: the substitute-format fetches (backend's unsupported vertex formats) on every input.
    if (s.stage == gpu::usse::Stage::Vertex) {
      using namespace gpu::usse;
      for (uint8_t mode : {uint8_t(kFetchUint), uint8_t(kFetchSint), uint8_t(kFetchWOne),
                           uint8_t(kFetchUint | kFetchWOne), uint8_t(kFetchSint | kFetchWOne)}) {
        std::vector<uint8_t> fetch(s.inputs.size(), mode);
        Shader v = translate(data, fetch);
        val_msg.clear();
        if (!v.ok || !tools.Validate(v.spirv)) {
          failures++;
          std::printf("INVALID %s fetch %u: %s%s", name.c_str(), mode, v.error.c_str(), val_msg.c_str());
        }
      }
    }
    if (name == dis_name) {
      std::string dis;
      tools.Disassemble(s.spirv, &dis, SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES);
      std::printf("%s", dis.c_str());
    }
#endif
    if (!spv_dir.empty()) {
      std::ofstream o(fs::path(spv_dir) / (name + ".spv"), std::ios::binary);
      o.write(reinterpret_cast<const char*>(s.spirv.data()), std::streamsize(s.spirv.size() * 4));
    }
  }
  if (!listing_path.empty()) std::ofstream(listing_path, std::ios::binary) << listing;
  std::printf("%zu programs, %d failures, %d validated\n", files.size(), failures, validated);
  return failures ? 1 : 0;
}
