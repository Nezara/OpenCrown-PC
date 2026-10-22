// The overlay's two shaders, built with our SPIR-V builder (no shader compiler at run time).
//   vertex:   in vec4 pos_uv (x, y, u, v) @0, vec4 color @1, float inner @2; push constant vec2 window size.
//             gl_Position = (x, y) / size * 2 - 1; out vec4 shape (u, v, inner, 0) @0, vec4 color @1.
//   fragment: r = length(u, v), w = fwidth(r); coverage = clamp((1 - r) / w) * clamp((r - inner) / w);
//             out = color * vec4(1, 1, 1, coverage), blended over the picture (src alpha, 1 - src alpha).
#include "gpu/overlay_shaders.h"

#include "gpu/spirv/builder.h"

namespace gpu {

using namespace spirv;

std::vector<uint32_t> overlay_vertex_spirv() {
  Builder b;
  b.capability(spv::CapabilityShader);
  const Id t_void = b.type_void(), t_f = b.type_float(), t_v2 = b.type_vector(t_f, 2), t_v4 = b.type_vector(t_f, 4);
  const Id p_in_v4 = b.type_pointer(spv::StorageClassInput, t_v4), p_in_f = b.type_pointer(spv::StorageClassInput, t_f);
  const Id p_out_v4 = b.type_pointer(spv::StorageClassOutput, t_v4);

  const Id in_pos = b.variable(p_in_v4, spv::StorageClassInput), in_col = b.variable(p_in_v4, spv::StorageClassInput);
  const Id in_inner = b.variable(p_in_f, spv::StorageClassInput);
  b.decorate(in_pos, spv::DecorationLocation, {0});
  b.decorate(in_col, spv::DecorationLocation, {1});
  b.decorate(in_inner, spv::DecorationLocation, {2});
  const Id out_pos = b.variable(p_out_v4, spv::StorageClassOutput);
  b.decorate(out_pos, spv::DecorationBuiltIn, {spv::BuiltInPosition});
  const Id out_shape = b.variable(p_out_v4, spv::StorageClassOutput), out_col = b.variable(p_out_v4, spv::StorageClassOutput);
  b.decorate(out_shape, spv::DecorationLocation, {0});
  b.decorate(out_col, spv::DecorationLocation, {1});

  const Id t_pc = b.type_struct({t_v2});
  b.decorate(t_pc, spv::DecorationBlock);
  b.member_decorate(t_pc, 0, spv::DecorationOffset, {0});
  const Id pc = b.variable(b.type_pointer(spv::StorageClassPushConstant, t_pc), spv::StorageClassPushConstant);

  const Id fn = b.begin_function(t_void, b.type_function(t_void));
  b.label();
  const Id pos = b.load(t_v4, in_pos);
  const Id size = b.load(t_v2, b.access_chain(b.type_pointer(spv::StorageClassPushConstant, t_v2), pc, {b.const_int(0)}));
  const Id xy = b.vector_shuffle(t_v2, pos, pos, {0, 1});
  const Id one = b.const_float(1), zero = b.const_float(0);
  const Id ndc = b.fsub(t_v2, b.vec_times_scalar(t_v2, b.fdiv(t_v2, xy, size), b.const_float(2)),
                        b.const_composite(t_v2, {one, one}));
  b.store(out_pos, b.composite_construct(t_v4, {b.composite_extract(t_f, ndc, {0}), b.composite_extract(t_f, ndc, {1}),
                                                zero, one}));
  b.store(out_shape, b.composite_construct(t_v4, {b.composite_extract(t_f, pos, {2}), b.composite_extract(t_f, pos, {3}),
                                                  b.load(t_f, in_inner), zero}));
  b.store(out_col, b.load(t_v4, in_col));
  b.ret();
  b.end_function();
  b.entry_point(spv::ExecutionModelVertex, fn, "main", {in_pos, in_col, in_inner, out_pos, out_shape, out_col});
  return b.finish();
}

std::vector<uint32_t> overlay_fragment_spirv() {
  Builder b;
  b.capability(spv::CapabilityShader);
  const Id t_void = b.type_void(), t_f = b.type_float(), t_v2 = b.type_vector(t_f, 2), t_v4 = b.type_vector(t_f, 4);
  const Id p_in_v4 = b.type_pointer(spv::StorageClassInput, t_v4), p_out_v4 = b.type_pointer(spv::StorageClassOutput, t_v4);
  const Id in_shape = b.variable(p_in_v4, spv::StorageClassInput), in_col = b.variable(p_in_v4, spv::StorageClassInput);
  b.decorate(in_shape, spv::DecorationLocation, {0});
  b.decorate(in_col, spv::DecorationLocation, {1});
  const Id out = b.variable(p_out_v4, spv::StorageClassOutput);
  b.decorate(out, spv::DecorationLocation, {0});

  const Id fn = b.begin_function(t_void, b.type_function(t_void));
  b.label();
  const Id shape = b.load(t_v4, in_shape), col = b.load(t_v4, in_col);
  const Id r = b.ext(t_f, GLSLstd450Length, {b.vector_shuffle(t_v2, shape, shape, {0, 1})});
  const Id w = b.fmax(t_f, b.unop(spv::OpFwidth, t_f, r), b.const_float(1e-4f));
  const Id zero = b.const_float(0), one = b.const_float(1);
  const Id outer = b.fclamp(t_f, b.fdiv(t_f, b.fsub(t_f, one, r), w), zero, one);
  const Id hole = b.fclamp(t_f, b.fdiv(t_f, b.fsub(t_f, r, b.composite_extract(t_f, shape, {2})), w), zero, one);
  const Id cov = b.fmul(t_f, outer, hole);
  b.store(out, b.composite_insert(t_v4, b.fmul(t_f, b.composite_extract(t_f, col, {3}), cov), col, {3}));
  b.ret();
  b.end_function();
  b.entry_point(spv::ExecutionModelFragment, fn, "main", {in_shape, in_col, out});
  b.execution_mode(fn, spv::ExecutionModeOriginUpperLeft);
  return b.finish();
}

}  // namespace gpu
