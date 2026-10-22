#include "guest/module.h"

#include <cstdio>
#include <map>
#include <set>

namespace guest {

constexpr uint32_t NID_MODULE_START = 0x935cd196;
constexpr uint32_t NID_PROCESS_PARAM = 0x70fba1e7;

static void repair_hooked_imports(const Memory& mem, Module& mod);

bool map_image(Memory& mem, const Image& img, std::string& err) {
  for (const Segment& s : img.segments) {
    if (!mem.map(s.vaddr, s.memsz) || !mem.copy_in(s.vaddr, s.data.data(), uint32_t(s.data.size())))
      return err = "cannot map segment at " + std::to_string(s.vaddr), false;
  }
  return true;
}

bool parse_module(const Memory& mem, const Image& img, Module& out, std::string& err) {
  uint32_t seg = img.entry >> 30;
  if (seg >= img.segments.size()) return err = "module info segment out of range", false;
  uint32_t mi = img.segments[seg].vaddr + (img.entry & 0x3fffffff);
  if (!mem.mapped(mi)) return err = "module info not mapped", false;
  out = {};
  out.module_info = mi;
  for (const Segment& sg : img.segments) out.segments.push_back(sg.vaddr);
  out.name = mem.cstr(mi + 4, 27);
  // sceModuleInfo offsets are relative to the segment holding it.
  uint32_t segbase = img.segments[seg].vaddr;
  uint32_t exp_top = segbase + mem.read<uint32_t>(mi + 0x24), exp_end = segbase + mem.read<uint32_t>(mi + 0x28);
  uint32_t imp_top = segbase + mem.read<uint32_t>(mi + 0x2c), imp_end = segbase + mem.read<uint32_t>(mi + 0x30);

  for (uint32_t e = exp_top; e < exp_end;) {
    uint8_t size = mem.read<uint8_t>(e);
    if (size < 0x20) return err = "bad export entry", false;
    uint16_t nf = mem.read<uint16_t>(e + 6), nv = mem.read<uint16_t>(e + 8);
    uint32_t nids = mem.read<uint32_t>(e + 0x18), ents = mem.read<uint32_t>(e + 0x1c);
    for (uint32_t i = 0; i < uint32_t(nf) + nv; i++) {
      uint32_t nid = mem.read<uint32_t>(nids + 4 * i), ent = mem.read<uint32_t>(ents + 4 * i);
      if (nid == NID_MODULE_START) out.module_start = ent;
      if (nid == NID_PROCESS_PARAM) out.process_param = ent;
    }
    e += size;
  }
  if (!out.module_start) return err = "no module_start export", false;

  for (uint32_t e = imp_top; e < imp_end;) {
    uint16_t size = mem.read<uint16_t>(e);
    uint16_t nf = mem.read<uint16_t>(e + 6), nv = mem.read<uint16_t>(e + 8), ntls = mem.read<uint16_t>(e + 10);
    uint32_t lib_nid, lib_name, fnids, fents, vnids, vents;
    if (size == 0x34) {
      lib_nid = mem.read<uint32_t>(e + 0x10);
      lib_name = mem.read<uint32_t>(e + 0x14);
      fnids = mem.read<uint32_t>(e + 0x1c);
      fents = mem.read<uint32_t>(e + 0x20);
      vnids = mem.read<uint32_t>(e + 0x24);
      vents = mem.read<uint32_t>(e + 0x28);
    } else if (size == 0x24) {
      lib_nid = mem.read<uint32_t>(e + 0x0c);
      lib_name = mem.read<uint32_t>(e + 0x10);
      fnids = mem.read<uint32_t>(e + 0x14);
      fents = mem.read<uint32_t>(e + 0x18);
      vnids = mem.read<uint32_t>(e + 0x1c);
      vents = mem.read<uint32_t>(e + 0x20);
    } else {
      return err = "unknown import entry size " + std::to_string(size), false;
    }
    std::string lib = lib_name ? mem.cstr(lib_name, 64) : "?";
    for (uint32_t i = 0; i < nf; i++)
      out.imports.push_back({lib, lib_nid, mem.read<uint32_t>(fnids + 4 * i), mem.read<uint32_t>(fents + 4 * i)});
    for (uint32_t i = 0; i < nv; i++)
      out.vars.push_back({lib, mem.read<uint32_t>(vnids + 4 * i), mem.read<uint32_t>(vents + 4 * i)});
    out.num_import_vars += nv + ntls;
    e += size;
  }
  repair_hooked_imports(mem, out);
  return true;
}

// Some dumpers hook imports in the dumped file: the stub becomes `movw r12, #p ; movt r12, #p ; ldr r12, [r12] ;
// bx r12` (through a pointer their plugin fills) and the import's table entry is redirected to another import's
// stub. Seen in the user's PCSE00019 dump for sceIoOpen, sceIoGetstat, sceAppUtilDrmOpen and sceAppUtilDrmClose
// (all four pointing at __sce_aeabi_idiv0's stub). Give each redirected import back its hooked stub: the slot right
// after its predecessor's stub in the same library, else the library's leftover hooked slots in table order.
static void repair_hooked_imports(const Memory& mem, Module& mod) {
  auto hooked = [&](uint32_t a) {
    return mem.mapped(a) && mem.mapped(a + 15) && (mem.read<uint32_t>(a) & 0xfff0f000) == 0xe300c000 &&
           (mem.read<uint32_t>(a + 4) & 0xfff0f000) == 0xe340c000 && mem.read<uint32_t>(a + 8) == 0xe59cc000 &&
           mem.read<uint32_t>(a + 12) == 0xe12fff1c;
  };
  std::map<uint32_t, int> users;
  for (const ImportFunc& f : mod.imports) users[f.stub]++;
  std::set<uint32_t> free_hooks;
  if (!users.empty())
    for (uint32_t a = users.begin()->first & ~15u; a <= users.rbegin()->first; a += 16)
      if (!users.count(a) && hooked(a)) free_hooks.insert(a);
  if (free_hooks.empty()) return;

  std::vector<size_t> todo;
  for (size_t i = 0; i < mod.imports.size(); i++) {
    ImportFunc& f = mod.imports[i];
    if (users[f.stub] < 2) continue;
    uint32_t want = i && mod.imports[i - 1].lib == f.lib ? mod.imports[i - 1].stub + 16 : 0;
    if (free_hooks.erase(want)) {
      users[f.stub]--;
      f.stub = want;
    } else {
      todo.push_back(i);
    }
  }
  // Leftovers: a run of hooked slots belongs to the library whose stubs it touches; assign the run in address
  // order to that library's redirected imports in table order.
  for (size_t i : todo) {
    ImportFunc& f = mod.imports[i];
    if (users[f.stub] < 2) continue;  // the last remaining user keeps the stub: it is the genuine owner
    std::set<uint32_t> lib_stubs;
    for (const ImportFunc& g : mod.imports)
      if (g.lib == f.lib && users[g.stub] < 2) lib_stubs.insert(g.stub);
    std::set<uint32_t> run;
    for (bool grew = true; grew;) {
      grew = false;
      for (uint32_t h : free_hooks)
        if (!run.count(h) && (lib_stubs.count(h + 16) || lib_stubs.count(h - 16) || run.count(h + 16) ||
                              run.count(h - 16)))
          grew = run.insert(h).second;
    }
    if (run.empty()) continue;
    users[f.stub]--;
    f.stub = *run.begin();
    free_hooks.erase(f.stub);
  }
  for (const ImportFunc& f : mod.imports)
    if (users[f.stub] > 1) {
      std::fprintf(stderr, "[load] warning: import 0x%08x (%s) still shares stub 0x%08x\n", f.nid, f.lib.c_str(),
                   f.stub);
    }
}

bool patch_import_stubs(Memory& mem, const Module& mod, std::string& err) {
  if (mod.imports.size() > 0xffffff) return err = "too many imports", false;
  for (size_t i = 0; i < mod.imports.size(); i++) {
    uint32_t stub = mod.imports[i].stub;
    if (!stub || (stub & 3) || !mem.mapped(stub) || !mem.mapped(stub + 7)) return err = "bad import stub address", false;
    mem.write<uint32_t>(stub, 0xef000000u | uint32_t(i));  // svc #i
    mem.write<uint32_t>(stub + 4, 0xe12fff1eu);           // bx lr
  }
  return true;
}

}  // namespace guest

namespace guest {

// Variable-import reference lists: a header word (bits 4..27 = byte size including the header), then relocations
// in format 1 {u32: format 1, patch segment 4-7, type 8-15, addend 16-31; u32 offset} or format 2 {u32: format 2,
// patch segment, type; u32 offset; u32 addend}. Formats from vitasdk's sce-elf.h (vita-toolchain).
namespace {

void put_thumb_mov(Memory& mem, uint32_t at, uint16_t imm) {
  uint16_t hw1 = mem.read<uint16_t>(at), hw2 = mem.read<uint16_t>(at + 2);
  hw1 = uint16_t((hw1 & 0xFBF0) | ((imm >> 12) & 0xF) | (((imm >> 11) & 1) << 10));
  hw2 = uint16_t((hw2 & 0x8F00) | (((imm >> 8) & 7) << 12) | (imm & 0xFF));
  mem.write<uint16_t>(at, hw1);
  mem.write<uint16_t>(at + 2, hw2);
}
void put_arm_mov(Memory& mem, uint32_t at, uint16_t imm) {
  uint32_t w = mem.read<uint32_t>(at);
  mem.write<uint32_t>(at, (w & 0xFFF0F000u) | ((imm >> 12) & 0xF) << 16 | (imm & 0xFFF));
}

bool apply_reloc(Memory& mem, uint32_t type, uint32_t p, uint32_t value, std::string& err) {
  if (!mem.mapped(p) || !mem.mapped(p + 3)) return err = "relocation target unmapped", false;
  switch (type) {
    case 0x02: mem.write<uint32_t>(p, value); return true;                       // R_ARM_ABS32
    case 0x2B: put_arm_mov(mem, p, uint16_t(value)); return true;                // R_ARM_MOVW_ABS_NC
    case 0x2C: put_arm_mov(mem, p, uint16_t(value >> 16)); return true;          // R_ARM_MOVT_ABS
    case 0x2F: put_thumb_mov(mem, p, uint16_t(value)); return true;              // R_ARM_THM_MOVW_ABS_NC
    case 0x30: put_thumb_mov(mem, p, uint16_t(value >> 16)); return true;        // R_ARM_THM_MOVT_ABS
    default: return err = "unsupported variable relocation type " + std::to_string(type), false;
  }
}

}  // namespace

bool bind_import_vars(Memory& mem, const Module& mod, const std::function<uint32_t(const ImportVar&)>& address,
                      std::vector<std::string>& unbound, std::string& err) {
  for (const ImportVar& v : mod.vars) {
    uint32_t sym = address(v);
    if (!sym) {
      char b[48];
      std::snprintf(b, sizeof b, "%s 0x%08x", v.lib.c_str(), v.nid);
      unbound.push_back(b);
      continue;
    }
    if (!mem.mapped(v.refs)) return err = "variable import reference list unmapped", false;
    uint32_t size = (mem.read<uint32_t>(v.refs) >> 4) & 0xFFFFFF;
    for (uint32_t a = v.refs + 4, end = v.refs + size; a < end;) {
      uint32_t w = mem.read<uint32_t>(a);
      uint32_t format = w & 15, seg = (w >> 4) & 15, type = (w >> 8) & 0xFF, offset = mem.read<uint32_t>(a + 4);
      uint32_t addend;
      if (format == 1) {
        addend = w >> 16;
        a += 8;
      } else if (format == 2) {
        addend = mem.read<uint32_t>(a + 8);
        a += 12;
      } else {
        return err = "unknown variable relocation format " + std::to_string(format), false;
      }
      if (seg >= mod.segments.size()) return err = "variable relocation segment out of range", false;
      if (!apply_reloc(mem, type, mod.segments[seg] + offset, sym + addend, err)) return false;
    }
  }
  return true;
}

}  // namespace guest
