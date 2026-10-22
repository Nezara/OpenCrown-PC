// The loaded eboot's own tables: sceModuleInfo, its exports (module_start, process param) and its imports.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "guest/image.h"
#include "guest/memory.h"

namespace guest {

struct ImportFunc {
  std::string lib;       // library name from the import entry, e.g. "SceLibKernel"
  uint32_t lib_nid = 0;
  uint32_t nid = 0;      // function NID
  uint32_t stub = 0;     // guest address of the 16-byte ARM stub the game calls
};

struct ImportVar {
  std::string lib;
  uint32_t nid = 0;
  uint32_t refs = 0;     // guest address of its reference list (header word + relocations)
};

struct Module {
  std::string name;
  uint32_t module_info = 0;
  uint32_t module_start = 0;   // export NID 0x935cd196; bit 0 = Thumb
  uint32_t process_param = 0;  // export NID 0x70fba1e7 (SceProcessParam), 0 if absent
  uint32_t num_import_vars = 0;  // variable + TLS imports
  std::vector<ImportFunc> imports;
  std::vector<ImportVar> vars;    // variable imports (TLS imports are not supported)
  std::vector<uint32_t> segments; // load addresses, for segment-relative relocations
};

// Maps every segment of `img` and copies it in.
bool map_image(Memory& mem, const Image& img, std::string& err);
// Reads the tables from the mapped image.
bool parse_module(const Memory& mem, const Image& img, Module& out, std::string& err);
// Rewrites import stub i as `svc #i ; bx lr` (ARM), so a call lands in CallSVC with the import index.
bool patch_import_stubs(Memory& mem, const Module& mod, std::string& err);
// Points every reference to variable import i at address(i) (relocations from its reference list). Variables
// whose address() is 0 are left unbound and named in `unbound`.
bool bind_import_vars(Memory& mem, const Module& mod, const std::function<uint32_t(const ImportVar&)>& address,
                      std::vector<std::string>& unbound, std::string& err);

}  // namespace guest
