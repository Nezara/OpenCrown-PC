// Builds representative translator-shaped shaders with our SPIR-V Builder and validates them with SPIRV-Tools
// (dev option DCP_SPIRV_TOOLS; the runtime never links the validator). Usage: spirv_test [out_dir]
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <spirv-tools/libspirv.hpp>

#include "gpu/compute_shaders.h"
#include "gpu/overlay_shaders.h"
#include "gpu/spirv/builder.h"

using namespace spirv;
using Words = std::vector<uint32_t>;

namespace {

// Vertex: position * (uniform matrix rows), push-constant scale, register file as a Function array of uint.
Words build_vertex() {
  Builder b;
  b.capability(spv::CapabilityShader);
  const Id t_void = b.type_void(), t_f = b.type_float(), t_u = b.type_uint(), t_v4 = b.type_vector(t_f, 4);
  const Id t_fn = b.type_function(t_void);

  const Id t_mat = b.type_array(t_v4, 4, 16);  // mat as vec4[4], std140
  const Id t_ubo = b.type_struct({t_mat});
  b.decorate(t_ubo, spv::DecorationBlock);
  b.member_decorate(t_ubo, 0, spv::DecorationOffset, {0});
  b.name(t_ubo, "Uniforms");
  b.member_name(t_ubo, 0, "mvp");
  const Id ubo = b.variable(b.type_pointer(spv::StorageClassUniform, t_ubo), spv::StorageClassUniform);
  b.decorate(ubo, spv::DecorationDescriptorSet, {0});
  b.decorate(ubo, spv::DecorationBinding, {0});

  const Id t_pc = b.type_struct({t_v4});
  b.decorate(t_pc, spv::DecorationBlock);
  b.member_decorate(t_pc, 0, spv::DecorationOffset, {0});
  const Id pc = b.variable(b.type_pointer(spv::StorageClassPushConstant, t_pc), spv::StorageClassPushConstant);

  const Id p_in_v4 = b.type_pointer(spv::StorageClassInput, t_v4);
  const Id p_out_v4 = b.type_pointer(spv::StorageClassOutput, t_v4);
  const Id in_pos = b.variable(p_in_v4, spv::StorageClassInput);
  b.decorate(in_pos, spv::DecorationLocation, {0});
  const Id out_pos = b.variable(p_out_v4, spv::StorageClassOutput);
  b.decorate(out_pos, spv::DecorationBuiltIn, {spv::BuiltInPosition});
  const Id out_var = b.variable(p_out_v4, spv::StorageClassOutput);
  b.decorate(out_var, spv::DecorationLocation, {0});

  const Id main_fn = b.begin_function(t_void, t_fn);
  b.name(main_fn, "main");
  b.label();
  const Id t_regs = b.type_array(t_u, 8);
  const Id regs = b.local_variable(b.type_pointer(spv::StorageClassFunction, t_regs));

  const Id p_u = b.type_pointer(spv::StorageClassFunction, t_u);
  const Id p_ubo_v4 = b.type_pointer(spv::StorageClassUniform, t_v4);
  const Id pos = b.load(t_v4, in_pos);
  // Spill x into r0 as raw bits and read it back, the way translated instructions will.
  const Id x = b.composite_extract(t_f, pos, {0});
  b.store(b.access_chain(p_u, regs, {b.const_uint(0)}), b.bitcast(t_u, x));
  const Id x2 = b.bitcast(t_f, b.load(t_u, b.access_chain(p_u, regs, {b.const_uint(0)})));
  const Id pos2 = b.composite_insert(t_v4, x2, pos, {0});

  Id col[4];
  for (uint32_t i = 0; i < 4; ++i) {
    const Id row = b.load(t_v4, b.access_chain(p_ubo_v4, ubo, {b.const_uint(0), b.const_uint(i)}));
    col[i] = b.dot(t_f, pos2, row);
  }
  const Id clip = b.composite_construct(t_v4, {col[0], col[1], col[2], col[3]});
  b.store(out_pos, clip);
  const Id scale = b.load(t_v4, b.access_chain(b.type_pointer(spv::StorageClassPushConstant, t_v4), pc, {b.const_uint(0)}));
  b.store(out_var, b.fmul(t_v4, pos, scale));
  b.ret();
  b.end_function();

  b.entry_point(spv::ExecutionModelVertex, main_fn, "main", {in_pos, out_pos, out_var});
  return b.finish();
}

// Fragment: sampled colour, kill on negative alpha, otherwise half round-trip and GLSL maths.
Words build_fragment() {
  Builder b;
  b.capability(spv::CapabilityShader);
  const Id t_void = b.type_void(), t_f = b.type_float(), t_u = b.type_uint(), t_bool = b.type_bool();
  const Id t_v2 = b.type_vector(t_f, 2), t_v4 = b.type_vector(t_f, 4);
  const Id t_fn = b.type_function(t_void);

  const Id in_var = b.variable(b.type_pointer(spv::StorageClassInput, t_v4), spv::StorageClassInput);
  b.decorate(in_var, spv::DecorationLocation, {0});
  const Id t_samp = b.type_sampler2d();
  const Id samp = b.variable(b.type_pointer(spv::StorageClassUniformConstant, t_samp), spv::StorageClassUniformConstant);
  b.decorate(samp, spv::DecorationDescriptorSet, {0});
  b.decorate(samp, spv::DecorationBinding, {1});
  const Id out_var = b.variable(b.type_pointer(spv::StorageClassOutput, t_v4), spv::StorageClassOutput);
  b.decorate(out_var, spv::DecorationLocation, {0});

  const Id main_fn = b.begin_function(t_void, t_fn);
  b.name(main_fn, "main");
  b.label();
  const Id p_v4 = b.type_pointer(spv::StorageClassFunction, t_v4);
  const Id res = b.local_variable(p_v4, b.const_null(t_v4));

  const Id c = b.load(t_v4, in_var);
  const Id uv = b.vector_shuffle(t_v2, c, c, {0, 1});
  const Id texel = b.image_sample(t_v4, b.load(t_samp, samp), uv);
  const Id alpha = b.composite_extract(t_f, c, {3});
  const Id neg = b.cmp(spv::OpFOrdLessThan, t_bool, alpha, b.const_float(0.0f));

  const Id then_l = b.id(), else_l = b.id(), merge_l = b.id();
  b.selection_merge(merge_l);
  b.branch_cond(neg, then_l, else_l);
  b.label(then_l);
  b.kill();
  b.label(else_l);
  const Id xy = b.vector_shuffle(t_v2, texel, texel, {0, 1});
  const Id half_rt = b.unpack_half2x16(t_v2, b.pack_half2x16(t_u, xy));
  const Id powd = b.pow(t_f, b.fabs(t_f, b.composite_extract(t_f, texel, {2})), b.const_float(2.2f));
  const Id mixed = b.fmix(t_v2, half_rt, b.fmul(t_v2, xy, b.const_composite(t_v2, {b.const_float(0.5f), b.const_float(0.5f)})),
                          b.composite_construct(t_v2, {powd, powd}));
  const Id w = b.fclamp(t_f, b.sqrt(t_f, alpha), b.const_float(0.0f), b.const_float(1.0f));
  b.store(res, b.composite_construct(t_v4, {b.composite_extract(t_f, mixed, {0}), b.composite_extract(t_f, mixed, {1}),
                                            b.composite_extract(t_f, texel, {2}), w}));
  b.branch(merge_l);
  b.label(merge_l);
  b.store(out_var, b.load(t_v4, res));
  b.ret();
  b.end_function();

  b.entry_point(spv::ExecutionModelFragment, main_fn, "main", {in_var, out_var});
  b.execution_mode(main_fn, spv::ExecutionModeOriginUpperLeft);
  return b.finish();
}

}  // namespace

int main(int argc, char** argv) {
  const struct { const char* name; Words (*build)(); } cases[] = {{"vertex", build_vertex}, {"fragment", build_fragment},
                  {"overlay_vertex", gpu::overlay_vertex_spirv}, {"overlay_fragment", gpu::overlay_fragment_spirv},
                  {"bc_decode", gpu::bc_decode_spirv}};
  const std::filesystem::path out_dir = argc > 1 ? argv[1] : "";
  if (!out_dir.empty()) std::filesystem::create_directories(out_dir);

  spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_1);
  int modules = 0, failures = 0;
  std::string details;
  tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t& pos, const char* msg) {
    details += std::string("  ") + msg + " (word " + std::to_string(pos.index) + ")\n";
  });
  for (const auto& c : cases) {
    ++modules;
    const Words w = c.build();
    if (!out_dir.empty()) {
      std::ofstream f(out_dir / (std::string(c.name) + ".spv"), std::ios::binary);
      f.write(reinterpret_cast<const char*>(w.data()), static_cast<std::streamsize>(w.size() * 4));
    }
    details += std::string(c.name) + ":\n";
    const size_t mark = details.size();
    if (!tools.Validate(w.data(), w.size())) {
      ++failures;
      std::string dis;
      tools.Disassemble(w, &dis, SPV_BINARY_TO_TEXT_OPTION_NO_HEADER);
      details += dis;
    } else {
      details.resize(mark - (std::string(c.name).size() + 2));  // valid: drop the header again
    }
  }
  if (failures) std::fputs(details.c_str(), stderr);
  std::printf("%d modules, %d failures\n", modules, failures);
  return failures ? 1 : 0;
}
