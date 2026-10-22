// The Vulkan backend's compute shaders (see compute_shaders.h), built with our SPIR-V builder.
#include "gpu/compute_shaders.h"

#include "gpu/spirv/builder.h"

namespace gpu {

using namespace spirv;

std::vector<uint32_t> bc_decode_spirv() {
  Builder b;
  b.capability(spv::CapabilityShader);
  const Id t_void = b.type_void(), t_bool = b.type_bool(), t_u = b.type_uint(), t_i = b.type_sint();
  const Id t_f = b.type_float(), t_u3 = b.type_vector(t_u, 3), t_i2 = b.type_vector(t_i, 2), t_v4 = b.type_vector(t_f, 4);

  // set 0 binding 0: uint data[]
  const Id t_words = b.type_runtime_array(t_u, 4);
  const Id t_sb = b.type_struct({t_words});
  b.decorate(t_sb, spv::DecorationBlock);
  b.member_decorate(t_sb, 0, spv::DecorationOffset, {0});
  b.member_decorate(t_sb, 0, spv::DecorationNonWritable);
  const Id sb = b.variable(b.type_pointer(spv::StorageClassStorageBuffer, t_sb), spv::StorageClassStorageBuffer);
  b.decorate(sb, spv::DecorationDescriptorSet, {0});
  b.decorate(sb, spv::DecorationBinding, {0});
  // set 0 binding 1: rgba8 storage image
  const Id t_img = b.type_image(t_f, spv::Dim2D, 0, false, false, 2, spv::ImageFormatRgba8);
  const Id img = b.variable(b.type_pointer(spv::StorageClassUniformConstant, t_img), spv::StorageClassUniformConstant);
  b.decorate(img, spv::DecorationDescriptorSet, {0});
  b.decorate(img, spv::DecorationBinding, {1});
  b.decorate(img, spv::DecorationNonReadable);
  // push constants: BcDecodePush
  const Id t_pc = b.type_struct({t_u, t_u, t_u, t_u, t_u, t_u, t_u, t_u});
  b.decorate(t_pc, spv::DecorationBlock);
  for (uint32_t m = 0; m < 8; m++) b.member_decorate(t_pc, m, spv::DecorationOffset, {4 * m});
  const Id pc = b.variable(b.type_pointer(spv::StorageClassPushConstant, t_pc), spv::StorageClassPushConstant);
  const Id gid_var = b.variable(b.type_pointer(spv::StorageClassInput, t_u3), spv::StorageClassInput);
  b.decorate(gid_var, spv::DecorationBuiltIn, {spv::BuiltInGlobalInvocationId});

  const Id fn = b.begin_function(t_void, b.type_function(t_void));
  b.label();
  const Id p_pc_u = b.type_pointer(spv::StorageClassPushConstant, t_u), p_sb_u = b.type_pointer(spv::StorageClassStorageBuffer, t_u);
  auto push = [&](int m) { return b.load(t_u, b.access_chain(p_pc_u, pc, {b.const_int(m)})); };
  auto word = [&](Id index) { return b.load(t_u, b.access_chain(p_sb_u, sb, {b.const_int(0), index})); };
  auto k = [&](uint32_t v) { return b.const_uint(v); };
  auto add = [&](Id a, Id c) { return b.iadd(t_u, a, c); };
  auto sub = [&](Id a, Id c) { return b.isub(t_u, a, c); };
  auto mul = [&](Id a, Id c) { return b.imul(t_u, a, c); };
  auto div = [&](Id a, Id c) { return b.udiv(t_u, a, c); };
  auto band = [&](Id a, Id c) { return b.band(t_u, a, c); };
  auto bor = [&](Id a, Id c) { return b.bor(t_u, a, c); };
  auto shl = [&](Id a, Id c) { return b.shl(t_u, a, c); };
  auto shr = [&](Id a, Id c) { return b.shr(t_u, a, c); };
  auto sel = [&](Id c, Id a, Id d) { return b.select(t_u, c, a, d); };
  auto eq = [&](Id a, Id c) { return b.cmp(spv::OpIEqual, t_bool, a, c); };
  auto lt = [&](Id a, Id c) { return b.cmp(spv::OpULessThan, t_bool, a, c); };
  auto gt = [&](Id a, Id c) { return b.cmp(spv::OpUGreaterThan, t_bool, a, c); };
  auto umin = [&](Id a, Id c) { return b.ext(t_u, GLSLstd450UMin, {a, c}); };
  auto umax = [&](Id a, Id c) { return b.ext(t_u, GLSLstd450UMax, {a, c}); };
  auto spread = [&](Id v) {  // bit i -> bit 2i
    v = band(v, k(0xffff));
    v = band(bor(v, shl(v, k(8))), k(0x00ff00ffu));
    v = band(bor(v, shl(v, k(4))), k(0x0f0f0f0fu));
    v = band(bor(v, shl(v, k(2))), k(0x33333333u));
    return band(bor(v, shl(v, k(1))), k(0x55555555u));
  };

  const Id gid = b.load(t_u3, gid_var);
  const Id x = b.composite_extract(t_u, gid, {0}), y = b.composite_extract(t_u, gid, {1});
  const Id src = push(0), width = push(1), height = push(2), grid_w = push(3), n = push(4), wide = push(5);
  const Id mode = push(6), swizzled = push(7);
  const Id inside = b.logical_and(t_bool, lt(x, width), lt(y, height));
  const Id body = b.id(), merge = b.id();
  b.selection_merge(merge);
  b.branch_cond(inside, body, merge);
  b.label(body);

  // Block index: Morton inside the stored power-of-two grid (as texture.cpp morton_axes), or row-major.
  const Id bx = shr(x, k(2)), by = shr(y, k(2));
  const Id low = sub(shl(k(1), n), k(1)), n2 = add(n, n), is_wide = b.cmp(spv::OpINotEqual, t_bool, wide, k(0));
  const Id mx = bor(shl(spread(band(bx, low)), k(1)), sel(is_wide, shl(shr(bx, n), n2), k(0)));
  const Id my = bor(spread(band(by, low)), sel(is_wide, k(0), shl(shr(by, n), n2)));
  const Id blk = sel(b.cmp(spv::OpINotEqual, t_bool, swizzled, k(0)), bor(mx, my), add(mul(by, grid_w), bx));
  const Id is_bc1 = eq(mode, k(1));
  const Id base = add(src, mul(blk, sel(is_bc1, k(2), k(4))));
  const Id cbase = add(base, sel(is_bc1, k(0), k(2)));
  const Id cw = word(cbase), ci = word(add(cbase, k(1)));
  const Id i = add(mul(band(y, k(3)), k(4)), band(x, k(3)));  // texel inside the block

  // Colour: RGB565 endpoints, 2 interpolated entries (BC1 c0 <= c1: midpoint + transparent black).
  const Id c0 = band(cw, k(0xffff)), c1 = shr(cw, k(16));
  const Id four = b.logical_or(t_bool, b.logical_not(t_bool, is_bc1), gt(c0, c1));
  const Id s = band(shr(ci, add(i, i)), k(3));
  Id rgb[3];
  const uint32_t shift[3] = {11, 5, 0}, mask[3] = {31, 63, 31};
  for (int c = 0; c < 3; c++) {
    auto expand = [&](Id v) { return div(mul(band(shr(v, k(shift[c])), k(mask[c])), k(255)), k(mask[c])); };
    const Id p0 = expand(c0), p1 = expand(c1);
    const Id p2 = sel(four, div(add(add(p0, p0), p1), k(3)), shr(add(p0, p1), k(1)));
    const Id p3 = sel(four, div(add(p0, add(p1, p1)), k(3)), k(0));
    rgb[c] = sel(eq(s, k(0)), p0, sel(eq(s, k(1)), p1, sel(eq(s, k(2)), p2, p3)));
  }
  const Id alpha1 = sel(b.logical_and(t_bool, b.logical_not(t_bool, four), eq(s, k(3))), k(0), k(255));
  // BC2: explicit 4-bit alpha, texels 0-7 in word 0, 8-15 in word 1.
  const Id aw = word(add(base, shr(i, k(3))));
  const Id alpha2 = mul(band(shr(aw, mul(band(i, k(7)), k(4))), k(15)), k(17));
  // BC3: two 8-bit endpoints + 16 3-bit indices from bit 16 of the block's first 64 bits.
  const Id w0 = word(base), w1 = word(add(base, k(1)));
  const Id a0 = band(w0, k(255)), a1 = band(shr(w0, k(8)), k(255));
  const Id bit = add(k(16), mul(i, k(3)));
  const Id s_lo = umin(bit, k(31));
  const Id lo = bor(shr(w0, s_lo), shl(w1, sub(k(32), s_lo)));
  const Id hi = shr(w1, sub(umax(bit, k(32)), k(32)));
  const Id ai = band(sel(lt(bit, k(32)), lo, hi), k(7));
  const Id eight = gt(a0, a1);
  const Id interp8 = div(add(mul(sub(k(8), ai), a0), mul(sub(ai, k(1)), a1)), k(7));
  const Id interp6 = div(add(mul(sub(k(6), ai), a0), mul(sub(ai, k(1)), a1)), k(5));
  const Id six = sel(eq(ai, k(6)), k(0), sel(eq(ai, k(7)), k(255), interp6));
  const Id alpha3 = sel(eq(ai, k(0)), a0, sel(eq(ai, k(1)), a1, sel(eight, interp8, six)));
  const Id alpha = sel(is_bc1, alpha1, sel(eq(mode, k(2)), alpha2, alpha3));

  const Id inv = b.const_float(1.0f / 255.0f);
  auto unorm = [&](Id v) { return b.fmul(t_f, b.u2f(t_f, v), inv); };
  const Id texel = b.composite_construct(t_v4, {unorm(rgb[0]), unorm(rgb[1]), unorm(rgb[2]), unorm(alpha)});
  const Id coord = b.composite_construct(t_i2, {b.bitcast(t_i, x), b.bitcast(t_i, y)});
  b.image_write(b.load(t_img, img), coord, texel);
  b.branch(merge);
  b.label(merge);
  b.ret();
  b.end_function();
  b.entry_point(spv::ExecutionModelGLCompute, fn, "main", {gid_var});
  b.execution_mode(fn, spv::ExecutionModeLocalSize, {8, 8, 1});
  return b.finish();
}

}  // namespace gpu
