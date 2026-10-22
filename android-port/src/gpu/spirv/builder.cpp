#include "gpu/spirv/builder.h"

#include <algorithm>
#include <cstring>

namespace spirv {

namespace {
constexpr uint32_t kMagic = 0x07230203;

size_t string_words(std::string_view s) { return s.size() / 4 + 1; }  // the nul terminator always needs room

std::span<const uint32_t> span_of(std::initializer_list<uint32_t> l) { return {l.begin(), l.size()}; }
}  // namespace

void Builder::emit(Words& w, spv::Op op, std::initializer_list<uint32_t> ops, std::span<const uint32_t> tail) {
  w.push_back(static_cast<uint32_t>((1 + ops.size() + tail.size()) << 16) | static_cast<uint32_t>(op));
  w.insert(w.end(), ops.begin(), ops.end());
  w.insert(w.end(), tail.begin(), tail.end());
}

void Builder::emit_str(Words& w, spv::Op op, std::initializer_list<uint32_t> head, std::string_view s,
                       std::span<const uint32_t> tail) {
  const size_t sw = string_words(s);
  w.push_back(static_cast<uint32_t>((1 + head.size() + sw + tail.size()) << 16) | static_cast<uint32_t>(op));
  w.insert(w.end(), head.begin(), head.end());
  const size_t at = w.size();
  w.resize(at + sw, 0);  // zero padding doubles as the terminator
  std::memcpy(&w[at], s.data(), s.size());  // little-endian host: byte order matches SPIR-V's packing
  w.insert(w.end(), tail.begin(), tail.end());
}

Builder::Builder(uint32_t version) : version_(version) {
  glsl_ = id();
  emit_str(ext_imports_, spv::OpExtInstImport, {glsl_}, "GLSL.std.450");
}

void Builder::capability(spv::Capability c) {
  if (std::find(caps_.begin(), caps_.end(), static_cast<uint32_t>(c)) == caps_.end())
    caps_.push_back(static_cast<uint32_t>(c));
}

void Builder::entry_point(spv::ExecutionModel model, Id fn, std::string_view name, std::span<const Id> interface) {
  emit_str(entries_, spv::OpEntryPoint, {static_cast<uint32_t>(model), fn}, name, interface);
}

void Builder::execution_mode(Id fn, spv::ExecutionMode mode, std::initializer_list<uint32_t> literals) {
  emit(modes_, spv::OpExecutionMode, {fn, static_cast<uint32_t>(mode)}, span_of(literals));
}

void Builder::name(Id target, std::string_view n) { emit_str(debug_, spv::OpName, {target}, n); }

void Builder::member_name(Id type, uint32_t member, std::string_view n) {
  emit_str(debug_, spv::OpMemberName, {type, member}, n);
}

void Builder::decorate(Id target, spv::Decoration d, std::initializer_list<uint32_t> literals) {
  emit(annotations_, spv::OpDecorate, {target, static_cast<uint32_t>(d)}, span_of(literals));
}

void Builder::member_decorate(Id type, uint32_t member, spv::Decoration d, std::initializer_list<uint32_t> literals) {
  emit(annotations_, spv::OpMemberDecorate, {type, member, static_cast<uint32_t>(d)}, span_of(literals));
}

// Types and constants -------------------------------------------------------------------------------------------

Id Builder::typedecl(spv::Op op, std::initializer_list<uint32_t> ops, std::span<const uint32_t> tail,
                     uint32_t key_extra) {
  std::vector<uint32_t> body(ops);
  body.insert(body.end(), tail.begin(), tail.end());
  std::vector<uint32_t> key{static_cast<uint32_t>(op)};
  key.insert(key.end(), body.begin(), body.end());
  key.push_back(key_extra);
  auto [it, fresh] = dedup_.try_emplace(std::move(key), 0);
  if (!fresh) return it->second;
  it->second = id();
  emit(globals_, op, {it->second}, body);
  return it->second;
}

Id Builder::constant(spv::Op op, Id type, std::initializer_list<uint32_t> ops, std::span<const uint32_t> tail) {
  std::vector<uint32_t> body(ops);
  body.insert(body.end(), tail.begin(), tail.end());
  std::vector<uint32_t> key{static_cast<uint32_t>(op), type};
  key.insert(key.end(), body.begin(), body.end());
  auto [it, fresh] = dedup_.try_emplace(std::move(key), 0);
  if (!fresh) return it->second;
  it->second = id();
  emit(globals_, op, {type, it->second}, body);
  return it->second;
}

Id Builder::type_void() { return typedecl(spv::OpTypeVoid, {}); }
Id Builder::type_bool() { return typedecl(spv::OpTypeBool, {}); }
Id Builder::type_int(bool is_signed) { return typedecl(spv::OpTypeInt, {32, is_signed ? 1u : 0u}); }
Id Builder::type_float() { return typedecl(spv::OpTypeFloat, {32}); }
Id Builder::type_vector(Id component, uint32_t count) { return typedecl(spv::OpTypeVector, {component, count}); }

Id Builder::type_array(Id element, uint32_t length, uint32_t stride) {
  const Id len = const_uint(length);
  const Id before = next_id_;
  const Id t = typedecl(spv::OpTypeArray, {element, len}, {}, stride);
  if (stride && next_id_ != before) decorate(t, spv::DecorationArrayStride, {stride});  // only on first creation
  return t;
}

Id Builder::type_runtime_array(Id element, uint32_t stride) {
  const Id before = next_id_;
  const Id t = typedecl(spv::OpTypeRuntimeArray, {element}, {}, stride);
  if (next_id_ != before) decorate(t, spv::DecorationArrayStride, {stride});
  return t;
}

Id Builder::type_struct(std::span<const Id> members) {
  const Id t = id();
  emit(globals_, spv::OpTypeStruct, {t}, members);
  return t;
}

Id Builder::type_pointer(spv::StorageClass sc, Id pointee) {
  return typedecl(spv::OpTypePointer, {static_cast<uint32_t>(sc), pointee});
}

Id Builder::type_function(Id ret, std::span<const Id> params) { return typedecl(spv::OpTypeFunction, {ret}, params); }

Id Builder::type_image(Id sampled_type, spv::Dim dim, uint32_t depth, bool arrayed, bool ms, uint32_t sampled,
                       spv::ImageFormat format) {
  return typedecl(spv::OpTypeImage, {sampled_type, static_cast<uint32_t>(dim), depth, arrayed ? 1u : 0u,
                                     ms ? 1u : 0u, sampled, static_cast<uint32_t>(format)});
}

Id Builder::type_sampled_image(Id image) { return typedecl(spv::OpTypeSampledImage, {image}); }

Id Builder::const_bool(bool v) {
  return constant(v ? spv::OpConstantTrue : spv::OpConstantFalse, type_bool(), {});
}
Id Builder::const_int(int32_t v) { return constant(spv::OpConstant, type_sint(), {static_cast<uint32_t>(v)}); }
Id Builder::const_uint(uint32_t v) { return constant(spv::OpConstant, type_uint(), {v}); }

Id Builder::const_float(float v) {
  uint32_t bits;
  std::memcpy(&bits, &v, 4);
  return constant(spv::OpConstant, type_float(), {bits});
}

Id Builder::const_composite(Id type, std::span<const Id> parts) {
  return constant(spv::OpConstantComposite, type, {}, parts);
}

Id Builder::const_null(Id type) { return constant(spv::OpConstantNull, type, {}); }

Id Builder::variable(Id ptr_type, spv::StorageClass sc, Id initializer) {
  const Id v = id();
  const Id init[1] = {initializer};
  emit(globals_, spv::OpVariable, {ptr_type, v, static_cast<uint32_t>(sc)},
       std::span<const Id>(init, initializer ? 1 : 0));
  return v;
}

// Functions -----------------------------------------------------------------------------------------------------

Id Builder::value(spv::Op op, Id type, std::initializer_list<uint32_t> ops, std::span<const uint32_t> tail) {
  const Id r = id();
  std::vector<uint32_t> body(ops);
  body.insert(body.end(), tail.begin(), tail.end());
  emit(fn_, op, {type, r}, body);
  return r;
}

Id Builder::begin_function(Id ret, Id fn_type, spv::FunctionControlMask control) {
  const Id f = id();
  fn_.clear();
  fn_locals_.clear();
  fn_first_block_ = 0;
  in_function_ = true;
  emit(fn_, spv::OpFunction, {ret, f, static_cast<uint32_t>(control), fn_type});
  return f;
}

Id Builder::param(Id type) { return value(spv::OpFunctionParameter, type, {}); }

Id Builder::label(Id lbl) {
  if (!lbl) lbl = id();
  emit(fn_, spv::OpLabel, {lbl});
  if (!fn_first_block_) fn_first_block_ = fn_.size();
  return lbl;
}

Id Builder::local_variable(Id ptr_type, Id initializer) {
  const Id v = id();
  const Id init[1] = {initializer};
  emit(fn_locals_, spv::OpVariable, {ptr_type, v, static_cast<uint32_t>(spv::StorageClassFunction)},
       std::span<const Id>(init, initializer ? 1 : 0));
  return v;
}

void Builder::end_function() {
  emit(fn_, spv::OpFunctionEnd, {});
  if (fn_first_block_)
    fn_.insert(fn_.begin() + static_cast<std::ptrdiff_t>(fn_first_block_), fn_locals_.begin(), fn_locals_.end());
  functions_.insert(functions_.end(), fn_.begin(), fn_.end());
  fn_.clear();
  fn_locals_.clear();
  fn_first_block_ = 0;
  in_function_ = false;
}

// Instructions --------------------------------------------------------------------------------------------------

Id Builder::load(Id type, Id ptr) { return value(spv::OpLoad, type, {ptr}); }
void Builder::store(Id ptr, Id v) { emit(fn_, spv::OpStore, {ptr, v}); }

Id Builder::access_chain(Id ptr_type, Id base, std::span<const Id> indices) {
  return value(spv::OpAccessChain, ptr_type, {base}, indices);
}

Id Builder::unop(spv::Op op, Id type, Id a) { return value(op, type, {a}); }
Id Builder::binop(spv::Op op, Id type, Id a, Id b) { return value(op, type, {a, b}); }
Id Builder::select(Id t, Id cond, Id a, Id b) { return value(spv::OpSelect, t, {cond, a, b}); }

Id Builder::composite_construct(Id t, std::span<const Id> parts) {
  return value(spv::OpCompositeConstruct, t, {}, parts);
}

Id Builder::composite_extract(Id t, Id composite, std::initializer_list<uint32_t> indices) {
  return value(spv::OpCompositeExtract, t, {composite}, span_of(indices));
}

Id Builder::composite_insert(Id t, Id object, Id composite, std::initializer_list<uint32_t> indices) {
  return value(spv::OpCompositeInsert, t, {object, composite}, span_of(indices));
}

Id Builder::vector_shuffle(Id t, Id a, Id b, std::initializer_list<uint32_t> components) {
  return value(spv::OpVectorShuffle, t, {a, b}, span_of(components));
}

Id Builder::ext(Id t, GLSLstd450 inst, std::span<const Id> args) {
  return value(spv::OpExtInst, t, {glsl_, static_cast<uint32_t>(inst)}, args);
}

Id Builder::image_sample(Id t, Id sampled_image, Id coord) {
  return value(spv::OpImageSampleImplicitLod, t, {sampled_image, coord});
}

Id Builder::image_sample_lod(Id t, Id sampled_image, Id coord, Id lod) {
  return value(spv::OpImageSampleExplicitLod, t,
               {sampled_image, coord, static_cast<uint32_t>(spv::ImageOperandsLodMask), lod});
}

void Builder::selection_merge(Id merge, spv::SelectionControlMask control) {
  emit(fn_, spv::OpSelectionMerge, {merge, static_cast<uint32_t>(control)});
}
void Builder::branch(Id target) { emit(fn_, spv::OpBranch, {target}); }
void Builder::branch_cond(Id cond, Id t, Id f) { emit(fn_, spv::OpBranchConditional, {cond, t, f}); }
void Builder::image_write(Id image, Id coord, Id texel) { emit(fn_, spv::OpImageWrite, {image, coord, texel}); }
void Builder::kill() { emit(fn_, spv::OpKill, {}); }
void Builder::ret() { emit(fn_, spv::OpReturn, {}); }
void Builder::ret_value(Id v) { emit(fn_, spv::OpReturnValue, {v}); }

std::vector<uint32_t> Builder::finish() {
  Words caps;
  for (uint32_t c : caps_) emit(caps, spv::OpCapability, {c});
  Words mm;
  emit(mm, spv::OpMemoryModel,
       {static_cast<uint32_t>(spv::AddressingModelLogical), static_cast<uint32_t>(spv::MemoryModelGLSL450)});

  std::vector<uint32_t> out{kMagic, version_, 0, next_id_, 0};
  for (const Words* s : {&caps, &ext_imports_, &mm, &entries_, &modes_, &debug_, &annotations_, &globals_, &functions_})
    out.insert(out.end(), s->begin(), s->end());
  return out;
}

}  // namespace spirv
