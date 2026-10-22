// Our minimal SPIR-V module builder, the back end of the USSE -> SPIR-V shader translator.
//
// Scope is what the translator emits for Vulkan 1.1: straight-line shaders with structured if/else only (no loops),
// plus what the backend's own compute shaders need (storage buffers / images),
// registers as Function arrays of uint words, float math, GLSL.std.450, vec4 stage inputs/outputs, std140 uniform
// blocks, push constants, combined image samplers. Opcode / enum values come from the Khronos SPIRV-Headers.
//
// Every id is a uint32_t. Types and constants are de-duplicated (structs are not: two blocks with the same members
// may carry different decorations). Instructions append to the current block; function-local variables are hoisted
// to the top of the function's first block, as the spec requires. finish() assembles the logical-layout sections.
#pragma once
#include <cstdint>
#include <initializer_list>
#include <map>
#include <span>
#include <string_view>
#include <vector>

#include <spirv/unified1/GLSL.std.450.h>
#include <spirv/unified1/spirv.hpp>

namespace spirv {

using Id = uint32_t;
using Ids = std::initializer_list<Id>;

class Builder {
public:
  // SPIR-V 1.3 is the newest version every Vulkan 1.1 driver must accept; before 1.4 an entry point lists only its
  // Input / Output variables.
  explicit Builder(uint32_t version = 0x00010300);

  Id id() { return next_id_++; }  // a fresh id (e.g. a label to be placed later)

  // Module level.
  void capability(spv::Capability c);
  Id glsl450() const { return glsl_; }  // GLSL.std.450 import, always present
  void entry_point(spv::ExecutionModel model, Id fn, std::string_view name, std::span<const Id> interface);
  void entry_point(spv::ExecutionModel model, Id fn, std::string_view name, Ids interface) {
    entry_point(model, fn, name, std::span<const Id>(interface.begin(), interface.size()));
  }
  void execution_mode(Id fn, spv::ExecutionMode mode, std::initializer_list<uint32_t> literals = {});
  void name(Id target, std::string_view n);
  void member_name(Id type, uint32_t member, std::string_view n);
  void decorate(Id target, spv::Decoration d, std::initializer_list<uint32_t> literals = {});
  void member_decorate(Id type, uint32_t member, spv::Decoration d, std::initializer_list<uint32_t> literals = {});

  // Types.
  Id type_void();
  Id type_bool();
  Id type_int(bool is_signed);
  Id type_uint() { return type_int(false); }
  Id type_sint() { return type_int(true); }
  Id type_float();
  Id type_vector(Id component, uint32_t count);
  // Array of `length` elements. A non-zero `stride` adds ArrayStride (explicit layout, for Uniform / PushConstant
  // blocks); such arrays are distinct types from unlaid-out ones, which Function / Private storage needs.
  Id type_array(Id element, uint32_t length, uint32_t stride = 0);
  // Runtime-sized array (the last member of a storage buffer block), always with an ArrayStride.
  Id type_runtime_array(Id element, uint32_t stride);
  Id type_struct(std::span<const Id> members);  // never de-duplicated
  Id type_struct(Ids members) { return type_struct(std::span<const Id>(members.begin(), members.size())); }
  Id type_pointer(spv::StorageClass sc, Id pointee);
  Id type_function(Id ret, std::span<const Id> params);
  Id type_function(Id ret, Ids params = {}) {
    return type_function(ret, std::span<const Id>(params.begin(), params.size()));
  }
  Id type_image(Id sampled_type, spv::Dim dim, uint32_t depth = 0, bool arrayed = false, bool ms = false,
                uint32_t sampled = 1, spv::ImageFormat format = spv::ImageFormatUnknown);
  Id type_sampled_image(Id image);
  Id type_sampler2d() { return type_sampled_image(type_image(type_float(), spv::Dim2D)); }

  // Constants (de-duplicated; floats by bit pattern, so -0.0 and 0.0 stay distinct).
  Id const_bool(bool v);
  Id const_int(int32_t v);
  Id const_uint(uint32_t v);
  Id const_float(float v);
  Id const_composite(Id type, std::span<const Id> parts);
  Id const_composite(Id type, Ids parts) {
    return const_composite(type, std::span<const Id>(parts.begin(), parts.size()));
  }
  Id const_null(Id type);

  // Module-scope variable; `ptr_type` must be a pointer of storage class `sc`.
  Id variable(Id ptr_type, spv::StorageClass sc, Id initializer = 0);

  // Functions and blocks. begin_function() does not open a block: call label() next.
  Id begin_function(Id ret, Id fn_type, spv::FunctionControlMask control = spv::FunctionControlMaskNone);
  Id param(Id type);
  Id label(Id lbl = 0);  // starts a block; pass an id from id() to place a forward-referenced label
  Id local_variable(Id ptr_type, Id initializer = 0);  // Function storage, hoisted to the first block
  void end_function();

  // Memory.
  Id load(Id type, Id ptr);
  void store(Id ptr, Id value);
  Id access_chain(Id ptr_type, Id base, std::span<const Id> indices);
  Id access_chain(Id ptr_type, Id base, Ids indices) {
    return access_chain(ptr_type, base, std::span<const Id>(indices.begin(), indices.size()));
  }

  // Generic forms: any unary / binary value instruction (arithmetic, bitwise, conversion, comparison, logical).
  Id unop(spv::Op op, Id type, Id a);
  Id binop(spv::Op op, Id type, Id a, Id b);

  Id fneg(Id t, Id a) { return unop(spv::OpFNegate, t, a); }
  Id fadd(Id t, Id a, Id b) { return binop(spv::OpFAdd, t, a, b); }
  Id fsub(Id t, Id a, Id b) { return binop(spv::OpFSub, t, a, b); }
  Id fmul(Id t, Id a, Id b) { return binop(spv::OpFMul, t, a, b); }
  Id fdiv(Id t, Id a, Id b) { return binop(spv::OpFDiv, t, a, b); }
  Id fmod(Id t, Id a, Id b) { return binop(spv::OpFMod, t, a, b); }
  Id vec_times_scalar(Id t, Id v, Id s) { return binop(spv::OpVectorTimesScalar, t, v, s); }
  Id ineg(Id t, Id a) { return unop(spv::OpSNegate, t, a); }
  Id iadd(Id t, Id a, Id b) { return binop(spv::OpIAdd, t, a, b); }
  Id isub(Id t, Id a, Id b) { return binop(spv::OpISub, t, a, b); }
  Id imul(Id t, Id a, Id b) { return binop(spv::OpIMul, t, a, b); }
  Id udiv(Id t, Id a, Id b) { return binop(spv::OpUDiv, t, a, b); }
  Id sdiv(Id t, Id a, Id b) { return binop(spv::OpSDiv, t, a, b); }
  Id umod(Id t, Id a, Id b) { return binop(spv::OpUMod, t, a, b); }
  Id band(Id t, Id a, Id b) { return binop(spv::OpBitwiseAnd, t, a, b); }
  Id bor(Id t, Id a, Id b) { return binop(spv::OpBitwiseOr, t, a, b); }
  Id bxor(Id t, Id a, Id b) { return binop(spv::OpBitwiseXor, t, a, b); }
  Id bnot(Id t, Id a) { return unop(spv::OpNot, t, a); }
  Id shl(Id t, Id a, Id n) { return binop(spv::OpShiftLeftLogical, t, a, n); }
  Id shr(Id t, Id a, Id n) { return binop(spv::OpShiftRightLogical, t, a, n); }
  Id sar(Id t, Id a, Id n) { return binop(spv::OpShiftRightArithmetic, t, a, n); }
  Id bitcast(Id t, Id a) { return unop(spv::OpBitcast, t, a); }
  Id f2u(Id t, Id a) { return unop(spv::OpConvertFToU, t, a); }
  Id f2s(Id t, Id a) { return unop(spv::OpConvertFToS, t, a); }
  Id u2f(Id t, Id a) { return unop(spv::OpConvertUToF, t, a); }
  Id s2f(Id t, Id a) { return unop(spv::OpConvertSToF, t, a); }
  // Comparisons: result type is bool (or a bool vector of the operands' width).
  Id cmp(spv::Op op, Id result_t, Id a, Id b) { return binop(op, result_t, a, b); }
  Id logical_and(Id t, Id a, Id b) { return binop(spv::OpLogicalAnd, t, a, b); }
  Id logical_or(Id t, Id a, Id b) { return binop(spv::OpLogicalOr, t, a, b); }
  Id logical_not(Id t, Id a) { return unop(spv::OpLogicalNot, t, a); }
  Id select(Id t, Id cond, Id a, Id b);

  // Composites.
  Id composite_construct(Id t, std::span<const Id> parts);
  Id composite_construct(Id t, Ids parts) {
    return composite_construct(t, std::span<const Id>(parts.begin(), parts.size()));
  }
  Id composite_extract(Id t, Id composite, std::initializer_list<uint32_t> indices);
  Id composite_insert(Id t, Id object, Id composite, std::initializer_list<uint32_t> indices);
  Id vector_shuffle(Id t, Id a, Id b, std::initializer_list<uint32_t> components);  // 0xffffffff = undefined
  Id dot(Id t, Id a, Id b) { return binop(spv::OpDot, t, a, b); }

  // GLSL.std.450.
  Id ext(Id t, GLSLstd450 inst, std::span<const Id> args);
  Id ext(Id t, GLSLstd450 inst, Ids args) { return ext(t, inst, std::span<const Id>(args.begin(), args.size())); }
  Id fabs(Id t, Id a) { return ext(t, GLSLstd450FAbs, {a}); }
  Id fsign(Id t, Id a) { return ext(t, GLSLstd450FSign, {a}); }
  Id floor(Id t, Id a) { return ext(t, GLSLstd450Floor, {a}); }
  Id ceil(Id t, Id a) { return ext(t, GLSLstd450Ceil, {a}); }
  Id fract(Id t, Id a) { return ext(t, GLSLstd450Fract, {a}); }
  Id sqrt(Id t, Id a) { return ext(t, GLSLstd450Sqrt, {a}); }
  Id rsqrt(Id t, Id a) { return ext(t, GLSLstd450InverseSqrt, {a}); }
  Id exp2(Id t, Id a) { return ext(t, GLSLstd450Exp2, {a}); }
  Id log2(Id t, Id a) { return ext(t, GLSLstd450Log2, {a}); }
  Id pow(Id t, Id a, Id b) { return ext(t, GLSLstd450Pow, {a, b}); }
  Id fmin(Id t, Id a, Id b) { return ext(t, GLSLstd450FMin, {a, b}); }
  Id fmax(Id t, Id a, Id b) { return ext(t, GLSLstd450FMax, {a, b}); }
  Id fclamp(Id t, Id x, Id lo, Id hi) { return ext(t, GLSLstd450FClamp, {x, lo, hi}); }
  Id fmix(Id t, Id x, Id y, Id a) { return ext(t, GLSLstd450FMix, {x, y, a}); }
  Id sin(Id t, Id a) { return ext(t, GLSLstd450Sin, {a}); }
  Id cos(Id t, Id a) { return ext(t, GLSLstd450Cos, {a}); }
  Id pack_half2x16(Id uint_t, Id v2) { return ext(uint_t, GLSLstd450PackHalf2x16, {v2}); }
  Id unpack_half2x16(Id vec2_t, Id u) { return ext(vec2_t, GLSLstd450UnpackHalf2x16, {u}); }
  Id pack_unorm4x8(Id uint_t, Id v4) { return ext(uint_t, GLSLstd450PackUnorm4x8, {v4}); }
  Id unpack_unorm4x8(Id vec4_t, Id u) { return ext(vec4_t, GLSLstd450UnpackUnorm4x8, {u}); }
  Id pack_snorm4x8(Id uint_t, Id v4) { return ext(uint_t, GLSLstd450PackSnorm4x8, {v4}); }
  Id unpack_snorm4x8(Id vec4_t, Id u) { return ext(vec4_t, GLSLstd450UnpackSnorm4x8, {u}); }

  // Images: `sampled_image` is a loaded OpTypeSampledImage value. Implicit LOD is fragment-only.
  Id image_sample(Id t, Id sampled_image, Id coord);
  Id image_sample_lod(Id t, Id sampled_image, Id coord, Id lod);
  // Storage image store: `image` is a loaded OpTypeImage value (sampled = 2), `coord` an integer vector.
  void image_write(Id image, Id coord, Id texel);

  // Control flow. selection_merge() must directly precede the block's branch_cond().
  void selection_merge(Id merge, spv::SelectionControlMask control = spv::SelectionControlMaskNone);
  void branch(Id target);
  void branch_cond(Id cond, Id if_true, Id if_false);
  void kill();
  void ret();
  void ret_value(Id v);

  // The finished module (header + sections in logical-layout order); the builder should not be used afterwards.
  std::vector<uint32_t> finish();

private:
  using Words = std::vector<uint32_t>;
  static void emit(Words& w, spv::Op op, std::initializer_list<uint32_t> ops, std::span<const uint32_t> tail = {});
  static void emit_str(Words& w, spv::Op op, std::initializer_list<uint32_t> head, std::string_view s,
                       std::span<const uint32_t> tail = {});
  Id typedecl(spv::Op op, std::initializer_list<uint32_t> ops, std::span<const uint32_t> tail = {},
              uint32_t key_extra = 0);
  Id constant(spv::Op op, Id type, std::initializer_list<uint32_t> ops, std::span<const uint32_t> tail = {});
  Id value(spv::Op op, Id type, std::initializer_list<uint32_t> ops, std::span<const uint32_t> tail = {});

  uint32_t version_;
  Id next_id_ = 1;
  Id glsl_ = 0;
  std::vector<uint32_t> caps_;
  Words ext_imports_, entries_, modes_, debug_, annotations_, globals_, functions_;
  std::map<std::vector<uint32_t>, Id> dedup_;  // {opcode, operands..., extra} -> id, for types and constants

  // The function being built: header + params, then blocks; locals are spliced in after the first label.
  Words fn_, fn_locals_;
  size_t fn_first_block_ = 0;  // index in fn_ just past the first OpLabel, 0 = no block yet
  bool in_function_ = false;
};

}  // namespace spirv
