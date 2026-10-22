#include "synx/axon.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <set>

#include "synx/x64.hpp"
#include "synx/regalloc.hpp"

namespace synx::codegen {

using ir::Function;
using ir::Operand;
using ir::Place;
using ir::ProjKind;
using ir::Rvalue;
using ir::RvalueKind;
using ir::Statement;
using ir::Terminator;
using sema::Type;
using sema::TypeKind;

Target host_target() {
#if defined(_WIN32)
  return Target{TargetOs::Windows};
#elif defined(__APPLE__)
  return Target{TargetOs::MacOs};
#else
  return Target{TargetOs::Linux};
#endif
}

std::string Axon::symbol_for(const std::string& ir_name) {
  std::string s = "synx_";
  for (char c : ir_name) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) s += c;
    else if (c == '.') s += "_";
    else s += "_";
  }
  // Disambiguate names that differ only in punctuation.
  uint32_t h = 2166136261u;
  for (unsigned char c : ir_name) { h ^= c; h *= 16777619u; }
  char buf[16];
  std::snprintf(buf, sizeof buf, "_%08x", h);
  return s + buf;
}

Axon::Axon(sema::Program& program, sema::TypeContext& ctx, ir::Module& module, DiagnosticSink& sink, Target target)
    : prog_(program), ctx_(ctx), module_(module), sink_(sink), target_(target), layouts_(program, ctx) {}

std::string Axon::drop_symbol(const Type* type) {
  auto found = drop_symbols_.find(type);
  if (found != drop_symbols_.end()) return found->second;
  // Register before generating the body: recursive owners refer back here.
  std::string name = "synx_drop_" + std::to_string(drop_worklist_.size());
  drop_symbols_[type] = name;
  drop_worklist_.push_back(type);
  return name;
}

namespace {

// Runtime functions the generated code may call. Each has a C signature with
// scalar parameters only; struct results use an out pointer.
struct RuntimeFn {
  const char* name;
  bool returns_struct;  // result through out pointer
};

const std::map<std::string, RuntimeFn>& runtime_table() {
  static const std::map<std::string, RuntimeFn> table = {
      {"print", {"synx_rt_print", false}},
      {"panic", {"synx_rt_panic", false}},
      {"string_new", {"synx_rt_string_new", true}},
      {"string_push_str", {"synx_rt_string_push_str", false}},
      {"string_drop", {"synx_rt_string_drop", false}},
      {"vec_drop", {"synx_rt_vec_drop", false}},
      {"fmt_int", {"synx_rt_fmt_int", false}},
      {"fmt_uint", {"synx_rt_fmt_uint", false}},
      {"fmt_double", {"synx_rt_fmt_double", false}},
      {"fmt_bool", {"synx_rt_fmt_bool", false}},
      {"fmt_char", {"synx_rt_fmt_char", false}},
      {"str_cmp", {"synx_rt_str_cmp", false}},
      {"str_len", {"synx_rt_str_len", false}},
      {"str_parse_int", {"synx_rt_str_parse_int", true}},
      {"alloc", {"synx_rt_alloc", false}},
      {"realloc", {"synx_rt_realloc", false}},
      {"free", {"synx_rt_free", false}},
      {"memcpy", {"synx_rt_memcpy", false}},
      {"exit", {"synx_rt_exit", false}},
  };
  return table;
}

bool is_signed_type(const Type* t) {
  return t->kind == TypeKind::Prim && (t->prim == ast::PrimKind::Short || t->prim == ast::PrimKind::Int || t->prim == ast::PrimKind::Long);
}
bool is_float_type(const Type* t) { return t->kind == TypeKind::Prim && (t->prim == ast::PrimKind::Float || t->prim == ast::PrimKind::Double); }
bool is_f32(const Type* t) { return t->kind == TypeKind::Prim && t->prim == ast::PrimKind::Float; }

}  // namespace

// ---------- per function generation ----------

struct FunctionGen {
  Axon& ax;
  sema::Program& prog;
  sema::TypeContext& ctx;
  LayoutTable& lay;
  Function& f;
  Target target;
  Assembler a;
  DiagnosticSink& sink;
  ObjectFile& obj;
  bool ok = true;

  std::vector<int32_t> slot_off;   // rbp relative offset of each local
  RegisterAllocation allocation;
  std::vector<int32_t> saved_off;
  std::vector<int> block_labels;
  uint32_t max_out = 0;            // outgoing argument area size
  bool big_ret = false;            // result through hidden pointer
  int32_t scratch_off = 0;
  uint32_t scratch_slots = 14;     // C ABI: six integer and eight SSE pieces
  int epilogue_label = 0;

  FunctionGen(Axon& axon, sema::Program& p, sema::TypeContext& c, LayoutTable& l, Function& fn, Target t,
              DiagnosticSink& s, ObjectFile& o)
      : ax(axon), prog(p), ctx(c), lay(l), f(fn), target(t), sink(s), obj(o) {}

  void error(Span span, const std::string& msg) {
    sink.error(span, msg, "SNX0601");
    ok = false;
  }

  static uint64_t round8(uint64_t v) { return (v + 7) / 8 * 8; }

  // ----- types of places -----

  struct PlaceInfo {
    const Type* type = nullptr;   // type of the value at the address
    bool fat_len_in_rdx = false;  // for unsized values: length is in rdx
  };

  const Type* place_type(const Place& p) {
    const Type* t = f.locals[p.local].type;
    uint32_t variant = 0;
    for (auto& pr : p.proj) {
      switch (pr.kind) {
        case ProjKind::Deref: t = t->inner; break;
        case ProjKind::Index: t = t->inner; break;
        case ProjKind::Downcast: variant = pr.index; break;
        case ProjKind::Field:
          if (t->kind == TypeKind::Tuple) t = t->elems[pr.index];
          else if (t->kind == TypeKind::Struct) t = lay.struct_field_types(t)[pr.index];
          else if (t->kind == TypeKind::Enum) t = lay.variant_field_types(t, variant)[pr.index];
          break;
      }
    }
    return t;
  }

  const Type* operand_type(const Operand& o) {
    if (o.kind == Operand::Kind::Const) return o.constant.type;
    return place_type(o.place);
  }

  // Computes the address of a place into `out`. Uses r10 and r11 as temps.
  // Returns info about the value. For a value reached through a fat pointer
  // (str or slice), its length is left in rdx when fat_len_in_rdx is set.
  PlaceInfo addr(const Place& p, Reg out) {
    PlaceInfo info;
    const Type* t = f.locals[p.local].type;
    if (allocation.registers[p.local] >= 0) {
      error(f.locals[p.local].span, "internal code generation error: register local has no address");
      info.type = place_type(p);
      a.mov_ri(out, 0);
      return info;
    }
    uint32_t variant = 0;
    // Base local.
    if (p.local == 0 && big_ret) {
      a.load(out, RBP, 16, 8, false);  // hidden pointer
    } else {
      a.lea(out, RBP, slot_off[p.local]);
    }
    for (auto& pr : p.proj) {
      switch (pr.kind) {
        case ProjKind::Deref: {
          bool fat = t->inner->is_unsized();
          if (fat) {
            a.load(RDX, out, 8, 8, false);  // length
            info.fat_len_in_rdx = true;
          } else {
            info.fat_len_in_rdx = false;
          }
          a.load(out, out, 0, 8, false);
          t = t->inner;
          break;
        }
        case ProjKind::Index: {
          // Indexing a raw pointer steps through the elements it points to,
          // as in C. Arrays and slices index in place.
          if (t->kind == TypeKind::Ptr) a.load(out, out, 0, 8, false);
          const Type* elem = t->inner;
          uint64_t esz = lay.size_of(elem);
          if (allocation.registers[pr.index] >= 0)
            a.mov_rr(R10, static_cast<Reg>(allocation.registers[pr.index]));
          else a.load(R10, RBP, slot_off[pr.index], 8, false);
          if (esz != 1) {
            a.mov_ri(R11, static_cast<int64_t>(esz));
            a.imul_rr(R10, R11);
          }
          a.alu_rr(Alu::Add, out, R10);
          t = elem;
          info.fat_len_in_rdx = false;
          break;
        }
        case ProjKind::Downcast:
          variant = pr.index;
          break;
        case ProjKind::Field: {
          uint64_t off = 0;
          const Type* ft = nullptr;
          if (t->kind == TypeKind::Tuple) {
            off = lay.field_offset(t, pr.index);
            ft = t->elems[pr.index];
          } else if (t->kind == TypeKind::Struct) {
            off = lay.field_offset(t, pr.index);
            ft = lay.struct_field_types(t)[pr.index];
          } else if (t->kind == TypeKind::Enum) {
            Layout pl = lay.variant_payload(t, variant);
            off = lay.of(t).payload_offset + pl.field_offsets[pr.index];
            ft = lay.variant_field_types(t, variant)[pr.index];
          } else {
            error(Span{}, "field projection on a value of type '" + t->to_string() + "'");
            return info;
          }
          if (off) a.alu_ri(Alu::Add, out, static_cast<int32_t>(off));
          t = ft;
          info.fat_len_in_rdx = false;
          break;
        }
      }
    }
    info.type = t;
    return info;
  }

  // ----- loads and stores of scalars -----

  uint8_t scalar_size(const Type* t) {
    uint64_t s = lay.size_of(t);
    return static_cast<uint8_t>(s > 8 ? 8 : s);
  }

  // Loads a scalar of type t from [base+disp] into rax, extended to 64 bits.
  void load_scalar(const Type* t, Reg base, int32_t disp) {
    uint64_t size = lay.size_of(t);
    if (size == 0) { a.mov_ri(RAX, 0); return; }
    if (size == 1 || size == 2 || size == 4 || size == 8) {
      a.load(RAX, base, disp, static_cast<uint8_t>(size), is_signed_type(t) && !is_float_type(t));
      return;
    }
    // Odd sizes (3, 5, 6, 7): assemble from bytes.
    a.mov_ri(RAX, 0);
    for (int i = static_cast<int>(size) - 1; i >= 0; --i) {
      a.load(R10, base, disp + i, 1, false);
      if (i != static_cast<int>(size) - 1) {
        a.mov_ri(RCX, 8);
        a.shl_cl(RAX);
      }
      a.alu_rr(Alu::Or, RAX, R10);
    }
  }

  // Stores the low `size` bytes of rax to [base+disp].
  void store_scalar(uint64_t size, Reg base, int32_t disp) {
    if (size == 0) return;
    if (size == 1 || size == 2 || size == 4 || size == 8) {
      a.store(base, disp, RAX, static_cast<uint8_t>(size));
      return;
    }
    for (uint64_t i = 0; i < size; ++i) {
      a.store(base, disp + static_cast<int32_t>(i), RAX, 1);
      if (i + 1 < size) {
        a.mov_ri(RCX, 8);
        a.shr_cl(RAX);
      }
    }
  }

  // Registers use the same sign/zero extension as a stack store followed by
  // load_scalar. This includes call results and narrow arithmetic.
  void write_scalar(const Place& p) {
    const Type* t = place_type(p);
    int reg = allocation.registers[p.local];
    if (p.is_local() && reg >= 0) {
      Reg dest = static_cast<Reg>(reg);
      switch (lay.size_of(t)) {
        case 1: if (is_signed_type(t)) a.movsx8(dest, RAX); else a.movzx8(dest, RAX); break;
        case 2: if (is_signed_type(t)) a.movsx16(dest, RAX); else a.movzx16(dest, RAX); break;
        case 4: if (is_signed_type(t)) a.movsx32(dest, RAX); else a.mov32(dest, RAX); break;
        default: a.mov_rr(dest, RAX); break;
      }
    } else {
      addr(p, RDI);
      store_scalar(lay.size_of(t), RDI, 0);
    }
  }

  // Copies `size` bytes from [rsi] to [rdi]. Clobbers rax, rcx, rsi, rdi.
  void copy_mem(uint64_t size) {
    if (size == 0) return;
    if (size <= 64) {
      uint64_t off = 0;
      while (size - off >= 8) { a.load(RAX, RSI, static_cast<int32_t>(off), 8, false); a.store(RDI, static_cast<int32_t>(off), RAX, 8); off += 8; }
      if (size - off >= 4) { a.load(RAX, RSI, static_cast<int32_t>(off), 4, false); a.store(RDI, static_cast<int32_t>(off), RAX, 4); off += 4; }
      if (size - off >= 2) { a.load(RAX, RSI, static_cast<int32_t>(off), 2, false); a.store(RDI, static_cast<int32_t>(off), RAX, 2); off += 2; }
      if (size - off >= 1) { a.load(RAX, RSI, static_cast<int32_t>(off), 1, false); a.store(RDI, static_cast<int32_t>(off), RAX, 1); }
      return;
    }
    a.mov_ri(RCX, static_cast<int64_t>(size));
    a.rep_movsb();
  }

  // ----- constants -----

  uint64_t add_rodata(const std::string& bytes) {
    uint64_t off = obj.rodata.size();
    obj.rodata.insert(obj.rodata.end(), bytes.begin(), bytes.end());
    obj.rodata.push_back(0);
    while (obj.rodata.size() % 8) obj.rodata.push_back(0);
    return off;
  }

  // Materializes a constant: scalars in rax, fat pointers in rax:rdx,
  // doubles as bits in rax.
  void load_const(const ir::Constant& k) {
    switch (k.kind) {
      case ir::Constant::Kind::Int: case ir::Constant::Kind::Bool: case ir::Constant::Kind::Char:
        a.mov_ri(RAX, static_cast<int64_t>(k.int_value));
        break;
      case ir::Constant::Kind::Float: {
        if (is_f32(k.type)) {
          float fv = static_cast<float>(k.float_value);
          uint32_t bits;
          std::memcpy(&bits, &fv, 4);
          a.mov_ri(RAX, bits);
        } else {
          uint64_t bits;
          std::memcpy(&bits, &k.float_value, 8);
          a.mov_ri(RAX, static_cast<int64_t>(bits));
        }
        break;
      }
      case ir::Constant::Kind::Str: {
        uint64_t off = add_rodata(k.str_value);
        a.lea_rip(RAX, "synx_rodata", static_cast<int64_t>(off));
        a.mov_ri(RDX, static_cast<int64_t>(k.str_value.size()));
        break;
      }
      case ir::Constant::Kind::Unit:
        break;
      case ir::Constant::Kind::Fn:
        a.lea_rip(RAX, Axon::symbol_for(k.fn->name), 0);
        if (k.type->kind == TypeKind::Fn) a.mov_ri(RDX, 0);
        break;
    }
  }

  // Writes the value of an operand into the memory at [rdi]. The operand's
  // type decides the size. Clobbers rax, rcx, rdx, rsi, r10, r11; keeps rdi.
  void store_operand_to_rdi(const Operand& o) {
    const Type* t = operand_type(o);
    uint64_t size = lay.size_of(t);
    if (o.kind != Operand::Kind::Const && o.place.is_local() && allocation.registers[o.place.local] >= 0) {
      a.mov_rr(RAX, static_cast<Reg>(allocation.registers[o.place.local]));
      store_scalar(size, RDI, 0);
      return;
    }
    if (o.kind == Operand::Kind::Const) {
      if (o.constant.kind == ir::Constant::Kind::Unit) return;
      load_const(o.constant);
      if (o.constant.kind == ir::Constant::Kind::Str || t->kind == TypeKind::Fn) {
        a.store(RDI, 0, RAX, 8);
        a.store(RDI, 8, RDX, 8);
      } else {
        store_scalar(size, RDI, 0);
      }
      return;
    }
    // Place: compute its address into rsi (preserving rdi), then copy.
    PlaceInfo pi = addr(o.place, RSI);
    (void)pi;
    copy_mem(size);
  }

  // Loads a scalar operand (size <= 8, not fat) into rax.
  void load_operand_rax(const Operand& o) {
    if (o.kind == Operand::Kind::Const) { load_const(o.constant); return; }
    if (o.place.is_local() && allocation.registers[o.place.local] >= 0) {
      a.mov_rr(RAX, static_cast<Reg>(allocation.registers[o.place.local]));
      return;
    }
    PlaceInfo pi = addr(o.place, RSI);
    load_scalar(pi.type, RSI, 0);
  }

  // Loads a floating point operand into xmm0 as a double.
  void load_operand_xmm0(const Operand& o) {
    const Type* t = operand_type(o);
    if (o.kind == Operand::Kind::Const) {
      double d = o.constant.float_value;
      uint64_t bits;
      std::memcpy(&bits, &d, 8);
      a.mov_ri(RAX, static_cast<int64_t>(bits));
      a.movq_xr(XMM0, RAX);
      return;
    }
    PlaceInfo pi = addr(o.place, RSI);
    (void)pi;
    if (is_f32(t)) { a.movss_load(XMM0, RSI, 0); a.cvtss2sd(XMM0, XMM0); }
    else a.movsd_load(XMM0, RSI, 0);
  }

  // Stores the double in xmm0 to [rdi] with the type's precision.
  void store_xmm0_to_rdi(const Type* t) {
    if (is_f32(t)) { a.cvtsd2ss(XMM0, XMM0); a.movss_store(RDI, 0, XMM0); }
    else a.movsd_store(RDI, 0, XMM0);
  }

  // ----- statements -----

  void gen_assign(const Statement& s) {
    const Rvalue& r = s.rvalue;
    const Type* dest_t = place_type(s.place);
    uint64_t dest_size = lay.size_of(dest_t);
    switch (r.kind) {
      case RvalueKind::Use: {
        if (dest_size == 0) return;
        if (s.place.is_local() && allocation.registers[s.place.local] >= 0) {
          load_operand_rax(r.a);
          write_scalar(s.place);
          return;
        }
        addr(s.place, RDI);
        store_operand_to_rdi(r.a);
        return;
      }
      case RvalueKind::Ref: {
        PlaceInfo pi = addr(r.place, RAX);
        bool fat = pi.type && pi.type->is_unsized();
        if (fat && !pi.fat_len_in_rdx) {
          error(s.span, "cannot take a reference to an unsized value here");
          return;
        }
        a.mov_rr(RSI, RAX);  // keep address
        addr(s.place, RDI);
        a.store(RDI, 0, RSI, 8);
        if (fat) a.store(RDI, 8, RDX, 8);
        return;
      }
      case RvalueKind::StrFromString:
      case RvalueKind::VecToSlice: {
        // Operand is a reference to String or Vec: {data, len, cap}.
        load_operand_rax(r.a);          // rax = pointer to the struct
        a.load(RDX, RAX, 8, 8, false);  // len
        a.load(RAX, RAX, 0, 8, false);  // data
        a.mov_rr(RSI, RAX);
        addr(s.place, RDI);
        a.store(RDI, 0, RSI, 8);
        a.store(RDI, 8, RDX, 8);
        return;
      }
      case RvalueKind::ToDyn: {
        if (!r.vtable) { error(s.span, "missing trait object vtable"); return; }
        load_operand_rax(r.a);
        a.mov_rr(RSI, RAX);
        a.lea_rip(RAX, Axon::symbol_for(r.vtable->name));
        a.mov_rr(RCX, RAX);
        addr(s.place, RDI);
        a.store(RDI, 0, RSI, 8);
        a.store(RDI, 8, RCX, 8);
        return;
      }
      case RvalueKind::ArrayToSlice: {
        const Type* rt = operand_type(r.a);  // &T[N]
        uint64_t n = rt->inner->length;
        load_operand_rax(r.a);
        a.mov_rr(RSI, RAX);
        addr(s.place, RDI);
        a.store(RDI, 0, RSI, 8);
        a.mov_ri(RAX, static_cast<int64_t>(n));
        a.store(RDI, 8, RAX, 8);
        return;
      }
      case RvalueKind::Binary: gen_binary(s); return;
      case RvalueKind::Unary: gen_unary(s); return;
      case RvalueKind::Cast: gen_cast(s); return;
      case RvalueKind::Aggregate: gen_aggregate(s); return;
      case RvalueKind::Discriminant: {
        addr(r.place, RSI);
        a.load(RAX, RSI, 0, 8, false);
        addr(s.place, RDI);
        a.store(RDI, 0, RAX, 8);
        return;
      }
      case RvalueKind::Len: {
        PlaceInfo pi = addr(r.place, RSI);
        if (pi.type->kind == TypeKind::Array) {
          a.mov_ri(RAX, static_cast<int64_t>(pi.type->length));
        } else if (pi.type->kind == TypeKind::Struct) {
          a.load(RAX, RSI, 8, 8, false);  // Vec len
        } else if (pi.fat_len_in_rdx) {
          a.mov_rr(RAX, RDX);
        } else {
          error(s.span, "cannot take the length of '" + pi.type->to_string() + "'");
          return;
        }
        addr(s.place, RDI);
        a.store(RDI, 0, RAX, 8);
        return;
      }
    }
  }

  // Spills rax to a scratch slot and reloads; used around address computation.
  int32_t scratch_disp(int slot) {
    if (slot < 0 || static_cast<uint32_t>(slot) >= scratch_slots) {
      error(Span{}, "internal code generation error: scratch slot is outside the frame");
      return scratch_off;
    }
    return scratch_off + 8 * slot;
  }
  void save_rax(int slot) { a.store(RBP, scratch_disp(slot), RAX, 8); }
  void load_rax(int slot) { a.load(RAX, RBP, scratch_disp(slot), 8, false); }
  void save_reg(Reg r, int slot) { a.store(RBP, scratch_disp(slot), r, 8); }
  void load_reg(Reg r, int slot) { a.load(r, RBP, scratch_disp(slot), 8, false); }

  // The failure path exits the process. It cannot clobber values on the
  // successful path, including scalar locals held in caller-save registers.
  void arithmetic_guard(Cond failure, Span span, const char* message = "integer overflow") {
    int good = a.new_label();
    a.jcc(static_cast<Cond>(static_cast<uint8_t>(failure) ^ 1), good);
    emit_panic(message, span);
    a.bind(good);
  }

  void check_narrow_result(const Type* t, Span span) {
    switch (lay.size_of(t)) {
      case 1: if (is_signed_type(t)) a.movsx8(RDX, RAX); else a.movzx8(RDX, RAX); break;
      case 2: if (is_signed_type(t)) a.movsx16(RDX, RAX); else a.movzx16(RDX, RAX); break;
      case 4: if (is_signed_type(t)) a.movsx32(RDX, RAX); else a.mov32(RDX, RAX); break;
      default: return;
    }
    a.alu_rr(Alu::Cmp, RAX, RDX);
    arithmetic_guard(Cond::NE, span);
  }

  void gen_binary(const Statement& s) {
    const Rvalue& r = s.rvalue;
    const Type* lt = operand_type(r.a);
    const Type* dest_t = place_type(s.place);
    bool is_cmp = r.bop == ast::BinaryOp::Eq || r.bop == ast::BinaryOp::Ne || r.bop == ast::BinaryOp::Lt ||
                  r.bop == ast::BinaryOp::Gt || r.bop == ast::BinaryOp::Le || r.bop == ast::BinaryOp::Ge;

    if (is_float_type(lt)) {
      load_operand_xmm0(r.b);
      a.movq_rx(RAX, XMM0);
      save_rax(0);
      load_operand_xmm0(r.a);
      load_rax(0);
      a.movq_xr(XMM1, RAX);
      if (is_cmp) {
        // Compare so that the "above" conditions give the right answer.
        switch (r.bop) {
          case ast::BinaryOp::Lt: a.ucomisd(XMM1, XMM0); a.setcc(Cond::A, RAX); break;
          case ast::BinaryOp::Le: a.ucomisd(XMM1, XMM0); a.setcc(Cond::AE, RAX); break;
          case ast::BinaryOp::Gt: a.ucomisd(XMM0, XMM1); a.setcc(Cond::A, RAX); break;
          case ast::BinaryOp::Ge: a.ucomisd(XMM0, XMM1); a.setcc(Cond::AE, RAX); break;
          case ast::BinaryOp::Eq:
            a.ucomisd(XMM0, XMM1); a.setcc(Cond::E, RAX); a.setcc(Cond::NP, RCX); a.alu_rr(Alu::And, RAX, RCX); break;
          default:
            a.ucomisd(XMM0, XMM1); a.setcc(Cond::NE, RAX); a.setcc(Cond::P, RCX); a.alu_rr(Alu::Or, RAX, RCX); break;
        }
        write_scalar(s.place);
        return;
      }
      switch (r.bop) {
        case ast::BinaryOp::Add: a.addsd(XMM0, XMM1); break;
        case ast::BinaryOp::Sub: a.subsd(XMM0, XMM1); break;
        case ast::BinaryOp::Mul: a.mulsd(XMM0, XMM1); break;
        case ast::BinaryOp::Div: a.divsd(XMM0, XMM1); break;
        default: error(s.span, "unsupported float operator"); return;
      }
      a.movq_rx(RAX, XMM0);
      save_rax(0);
      addr(s.place, RDI);
      load_rax(0);
      a.movq_xr(XMM0, RAX);
      store_xmm0_to_rdi(dest_t);
      return;
    }

    bool is_signed = is_signed_type(lt);
    bool checked = target.checked_arithmetic && lt->is_integer();
    // Keep rhs in rcx while the scalar lhs is loaded into rax.
    load_operand_rax(r.b);
    a.mov_rr(RCX, RAX);
    load_operand_rax(r.a);
    switch (r.bop) {
      case ast::BinaryOp::Add: case ast::BinaryOp::Sub:
        a.alu_rr(r.bop == ast::BinaryOp::Add ? Alu::Add : Alu::Sub, RAX, RCX);
        if (checked) { arithmetic_guard(is_signed ? Cond::O : Cond::B, s.span); check_narrow_result(lt, s.span); }
        break;
      case ast::BinaryOp::Mul:
        if (checked && !is_signed) {
          a.mul(RCX); a.test_rr(RDX, RDX); arithmetic_guard(Cond::NE, s.span);
        } else {
          a.imul_rr(RAX, RCX);
          if (checked) arithmetic_guard(Cond::O, s.span);
        }
        if (checked) check_narrow_result(lt, s.span);
        break;
      case ast::BinaryOp::Div: case ast::BinaryOp::Rem: {
        // Division by zero panics.
        int nonzero = a.new_label();
        a.test_rr(RCX, RCX);
        a.jcc(Cond::NE, nonzero);
        emit_panic("division by zero", s.span);
        a.bind(nonzero);
        int done = a.new_label();
        if (is_signed) {
          // Avoid the hardware exception for MIN / -1. Release wraps the
          // quotient and returns zero for the remainder at every width.
          int normal = a.new_label();
          unsigned bits = static_cast<unsigned>(lay.size_of(lt) * 8);
          int64_t minimum = bits == 64 ? std::numeric_limits<int64_t>::min() : -(int64_t(1) << (bits - 1));
          a.mov_ri(RDX, minimum); a.alu_rr(Alu::Cmp, RAX, RDX); a.jcc(Cond::NE, normal);
          a.alu_ri(Alu::Cmp, RCX, -1); a.jcc(Cond::NE, normal);
          if (checked) emit_panic("integer overflow", s.span);
          else { if (r.bop == ast::BinaryOp::Rem) a.mov_ri(RAX, 0); a.jmp(done); }
          a.bind(normal);
          a.cqo(); a.idiv(RCX);
        }
        else { a.mov_ri(RDX, 0); a.div(RCX); }
        if (r.bop == ast::BinaryOp::Rem) a.mov_rr(RAX, RDX);
        a.bind(done);
        break;
      }
      case ast::BinaryOp::BitAnd: a.alu_rr(Alu::And, RAX, RCX); break;
      case ast::BinaryOp::BitOr: a.alu_rr(Alu::Or, RAX, RCX); break;
      case ast::BinaryOp::BitXor: a.alu_rr(Alu::Xor, RAX, RCX); break;
      case ast::BinaryOp::Shl: case ast::BinaryOp::Shr: {
        int32_t bits = static_cast<int32_t>(lay.size_of(lt) * 8);
        if (checked) { a.alu_ri(Alu::Cmp, RCX, bits); arithmetic_guard(Cond::AE, s.span, "shift count out of bounds"); }
        else a.alu_ri(Alu::And, RCX, bits - 1);
        if (r.bop == ast::BinaryOp::Shl) a.shl_cl(RAX);
        else if (is_signed) a.sar_cl(RAX); else a.shr_cl(RAX);
        break;
      }
      case ast::BinaryOp::Eq: a.alu_rr(Alu::Cmp, RAX, RCX); a.setcc(Cond::E, RAX); break;
      case ast::BinaryOp::Ne: a.alu_rr(Alu::Cmp, RAX, RCX); a.setcc(Cond::NE, RAX); break;
      case ast::BinaryOp::Lt: a.alu_rr(Alu::Cmp, RAX, RCX); a.setcc(is_signed ? Cond::L : Cond::B, RAX); break;
      case ast::BinaryOp::Le: a.alu_rr(Alu::Cmp, RAX, RCX); a.setcc(is_signed ? Cond::LE : Cond::BE, RAX); break;
      case ast::BinaryOp::Gt: a.alu_rr(Alu::Cmp, RAX, RCX); a.setcc(is_signed ? Cond::G : Cond::A, RAX); break;
      case ast::BinaryOp::Ge: a.alu_rr(Alu::Cmp, RAX, RCX); a.setcc(is_signed ? Cond::GE : Cond::AE, RAX); break;
      case ast::BinaryOp::And: a.alu_rr(Alu::And, RAX, RCX); break;
      case ast::BinaryOp::Or: a.alu_rr(Alu::Or, RAX, RCX); break;
    }
    write_scalar(s.place);
  }

  void gen_unary(const Statement& s) {
    const Rvalue& r = s.rvalue;
    const Type* t = operand_type(r.a);
    const Type* dest_t = place_type(s.place);
    if (is_float_type(t)) {
      load_operand_xmm0(r.a);
      a.xorpd(XMM1, XMM1);
      a.subsd(XMM1, XMM0);
      a.movq_rx(RAX, XMM1);
      save_rax(0);
      addr(s.place, RDI);
      load_rax(0);
      a.movq_xr(XMM0, RAX);
      store_xmm0_to_rdi(dest_t);
      return;
    }
    load_operand_rax(r.a);
    switch (r.uop) {
      case ast::UnaryOp::Neg:
        a.neg(RAX);
        if (target.checked_arithmetic && t->is_integer()) { arithmetic_guard(Cond::O, s.span); check_narrow_result(t, s.span); }
        break;
      case ast::UnaryOp::Not:
        if (t->is_bool()) a.alu_ri(Alu::Xor, RAX, 1); else a.not_(RAX);
        break;
      case ast::UnaryOp::BitNot: a.not_(RAX); break;
      case ast::UnaryOp::Deref: error(s.span, "deref as rvalue"); return;
    }
    write_scalar(s.place);
  }

  void gen_cast(const Statement& s) {
    const Rvalue& r = s.rvalue;
    const Type* from = operand_type(r.a);
    const Type* to = r.type;
    bool from_f = is_float_type(from), to_f = is_float_type(to);
    if (from_f && to_f) {
      load_operand_xmm0(r.a);
      a.movq_rx(RAX, XMM0);
      save_rax(0);
      addr(s.place, RDI);
      load_rax(0);
      a.movq_xr(XMM0, RAX);
      store_xmm0_to_rdi(to);
      return;
    }
    if (from_f) {
      load_operand_xmm0(r.a);
      a.cvttsd2si(RAX, XMM0);
      write_scalar(s.place);
      return;
    }
    if (to_f) {
      load_operand_rax(r.a);  // sign or zero extended per source type
      a.cvtsi2sd(XMM0, RAX);
      a.movq_rx(RAX, XMM0);
      save_rax(0);
      addr(s.place, RDI);
      load_rax(0);
      a.movq_xr(XMM0, RAX);
      store_xmm0_to_rdi(to);
      return;
    }
    load_operand_rax(r.a);
    write_scalar(s.place);
  }

  void gen_aggregate(const Statement& s) {
    const Rvalue& r = s.rvalue;
    const Type* t = r.type;
    std::vector<uint64_t> offsets;
    uint64_t base_off = 0;
    switch (r.agg) {
      case ir::AggregateKind::Struct: case ir::AggregateKind::Tuple:
        offsets = lay.of(t).field_offsets;
        break;
      case ir::AggregateKind::Array: {
        uint64_t esz = lay.size_of(t->inner);
        for (size_t i = 0; i < r.elems.size(); ++i) offsets.push_back(i * esz);
        break;
      }
      case ir::AggregateKind::Variant: {
        Layout pl = lay.variant_payload(t, r.variant);
        base_off = lay.of(t).payload_offset;
        offsets = pl.field_offsets;
        // Tag.
        addr(s.place, RDI);
        a.mov_ri(RAX, r.variant);
        a.store(RDI, 0, RAX, 8);
        break;
      }
    }
    for (size_t i = 0; i < r.elems.size() && i < offsets.size(); ++i) {
      const Operand& e = r.elems[i];
      if (lay.size_of(operand_type(e)) == 0) continue;
      addr(s.place, RDI);
      a.alu_ri(Alu::Add, RDI, static_cast<int32_t>(base_off + offsets[i]));
      store_operand_to_rdi(e);
    }
  }

  // ----- drops -----

  Function* drop_impl_for(const Type* t) {
    auto it = ax.module_.drop_impls.find(t);
    return it == ax.module_.drop_impls.end() ? nullptr : it->second;
  }

  bool type_needs_drop(const Type* t) {
    return ax.module_.drop_types.count(t) != 0;
  }

  // Calls one complete cleanup function. Its frame owns all child state.
  void drop_at_slot(const Type* t, int slot, Span span) {
    if (!type_needs_drop(t)) return;
    (void)span;
    load_reg(RAX, slot);
    a.store(RSP, 0, RAX, 8);
    a.call_sym(ax.drop_symbol(t));
  }

  // Emits the body of one cleanup function. Child values call their own
  // functions rather than expanding another cleanup tree in this frame.
  void drop_body_at_slot(const Type* t, int slot, Span span) {
    switch (t->kind) {
      case TypeKind::Fn: {
        int done = a.new_label();
        load_reg(RAX, slot);
        a.load(RAX, RAX, 8, 8, false);  // owned environment, null for a plain function
        a.test_rr(RAX, RAX);
        a.jcc(Cond::E, done);
        a.store(RSP, 0, RAX, 8);        // destructor's environment argument
        a.load(RAX, RAX, 0, 8, false);  // destructor code pointer
        a.call_reg(RAX);
        a.bind(done);
        return;
      }
      case TypeKind::Struct: {
        if (Function* d = drop_impl_for(t)) {
          // Call Type.Drop.drop(&self) with the internal convention.
          load_reg(RAX, slot);
          a.store(RSP, 0, RAX, 8);
          a.call_sym(Axon::symbol_for(d->name));
        }
        if (t->struct_def == prog.string_def) {
          load_reg(RAX, slot);
          c_call_ptr_arg("string_drop");
          return;
        }
        if (t->struct_def == prog.vec_def) {
          const Type* elem = t->elems[0];
          if (type_needs_drop(elem)) {
            // for i in 0..len: drop data[i]
            int loop = a.new_label(), done = a.new_label();
            load_reg(RAX, slot);
            a.load(RCX, RAX, 8, 8, false);  // len
            save_reg(RCX, slot + 1);        // remaining
            a.load(RAX, RAX, 0, 8, false);  // data
            save_rax(slot + 2);             // cursor
            a.bind(loop);
            load_reg(RCX, slot + 1);
            a.test_rr(RCX, RCX);
            a.jcc(Cond::E, done);
            drop_at_slot(elem, slot + 2, span);
            load_reg(RAX, slot + 2);
            a.mov_ri(RCX, lay.size_of(elem));
            a.alu_rr(Alu::Add, RAX, RCX);
            save_rax(slot + 2);
            load_reg(RCX, slot + 1);
            a.alu_ri(Alu::Sub, RCX, 1);
            save_reg(RCX, slot + 1);
            a.jmp(loop);
            a.bind(done);
          }
          load_reg(RAX, slot);
          c_call_ptr_arg("vec_drop");
          return;
        }
        auto fields = lay.struct_field_types(t);
        for (size_t i = 0; i < fields.size(); ++i) {
          if (!type_needs_drop(fields[i])) continue;
          load_reg(RAX, slot);
          a.alu_ri(Alu::Add, RAX, static_cast<int32_t>(lay.field_offset(t, static_cast<uint32_t>(i))));
          save_rax(slot + 1);
          drop_at_slot(fields[i], slot + 1, span);
        }
        return;
      }
      case TypeKind::Enum: {
        if (Function* d = drop_impl_for(t)) {
          load_reg(RAX, slot); a.store(RSP, 0, RAX, 8);
          a.call_sym(Axon::symbol_for(d->name));
        }
        int done = a.new_label();
        for (uint32_t v = 0; v < t->enum_def->variants.size(); ++v) {
          auto fields = lay.variant_field_types(t, v);
          bool any = false;
          for (auto* ft : fields) if (type_needs_drop(ft)) any = true;
          if (!any) continue;
          int next = a.new_label();
          load_reg(RAX, slot);
          a.load(RCX, RAX, 0, 8, false);
          a.mov_ri(RAX, v);
          a.alu_rr(Alu::Cmp, RCX, RAX);
          a.jcc(Cond::NE, next);
          Layout pl = lay.variant_payload(t, v);
          for (size_t i = 0; i < fields.size(); ++i) {
            if (!type_needs_drop(fields[i])) continue;
            load_reg(RAX, slot);
            a.alu_ri(Alu::Add, RAX, static_cast<int32_t>(lay.of(t).payload_offset + pl.field_offsets[i]));
            save_rax(slot + 1);
            drop_at_slot(fields[i], slot + 1, span);
          }
          a.jmp(done);
          a.bind(next);
        }
        a.bind(done);
        return;
      }
      case TypeKind::Tuple: {
        for (size_t i = 0; i < t->elems.size(); ++i) {
          if (!type_needs_drop(t->elems[i])) continue;
          load_reg(RAX, slot);
          a.alu_ri(Alu::Add, RAX, static_cast<int32_t>(lay.field_offset(t, static_cast<uint32_t>(i))));
          save_rax(slot + 1);
          drop_at_slot(t->elems[i], slot + 1, span);
        }
        return;
      }
      case TypeKind::Array: {
        uint64_t esz = lay.size_of(t->inner);
        int loop = a.new_label(), done = a.new_label();
        a.mov_ri(RAX, static_cast<int64_t>(t->length)); save_rax(slot + 1);
        load_reg(RAX, slot); save_rax(slot + 2);
        a.bind(loop);
        load_reg(RCX, slot + 1); a.test_rr(RCX, RCX); a.jcc(Cond::E, done);
        drop_at_slot(t->inner, slot + 2, span);
        load_reg(RAX, slot + 2); a.mov_ri(RCX, static_cast<int64_t>(esz));
        a.alu_rr(Alu::Add, RAX, RCX); save_rax(slot + 2);
        load_reg(RCX, slot + 1); a.alu_ri(Alu::Sub, RCX, 1); save_reg(RCX, slot + 1);
        a.jmp(loop); a.bind(done);
        return;
      }
      default: return;
    }
  }

  void gen_drop(const Statement& s) {
    const Type* t = place_type(s.place);
    if (!type_needs_drop(t)) return;
    int skip = -1;
    if (s.kind == Statement::Kind::DropIf) {
      skip = a.new_label();
      load_operand_rax(Operand::copy(Place::of(s.flag_local)));
      a.test_rr(RAX, RAX);
      a.jcc(Cond::E, skip);
    }
    addr(s.place, RAX);
    save_rax(0);
    drop_at_slot(t, 0, s.span);
    if (skip >= 0) a.bind(skip);
  }

  // ----- C ABI calls -----

  Reg c_arg_reg(int i) {
    if (target.win64_abi()) {
      static const Reg regs[] = {RCX, RDX, R8, R9};
      return regs[i];
    }
    static const Reg regs[] = {RDI, RSI, RDX, RCX, R8, R9};
    return regs[i];
  }

  // Calls a runtime function taking one pointer argument held in rax.
  void c_call_ptr_arg(const char* name) {
    a.mov_rr(c_arg_reg(0), RAX);
    a.call_sym(runtime_table().at(name).name);
  }

  void emit_panic(const std::string& message, Span span) {
    (void)span;
    uint64_t off = add_rodata(message);
    a.lea_rip(c_arg_reg(0), "synx_rodata", static_cast<int64_t>(off));
    a.mov_ri(c_arg_reg(1), static_cast<int64_t>(message.size()));
    a.call_sym(runtime_table().at("panic").name);
    a.ud2();
  }

  // Generates a call to a runtime or extern C function. Arguments are
  // scalars or fat pointers; a struct result goes through an out pointer.
  void gen_c_call(const Terminator& t, const std::string& symbol, bool returns_struct) {
    struct Piece { enum { Int, Float, Ptr } kind; };
    // Evaluate each argument into scratch slots first (two per fat pointer).
    std::vector<std::pair<int, bool>> pieces;  // scratch slot, is_float
    int slot = 0;
    const Type* dest_t = t.dest.is_none() ? ctx.void_type() : place_type(t.dest);
    bool out_ptr = returns_struct || (!t.dest.is_none() && lay.size_of(dest_t) > 8) ||
                   (!t.dest.is_none() && dest_t->kind != TypeKind::Prim && dest_t->kind != TypeKind::Ptr &&
                    dest_t->kind != TypeKind::Ref && dest_t->kind != TypeKind::Fn && lay.size_of(dest_t) > 0 && !is_float_type(dest_t));
    if (out_ptr) {
      addr(t.dest, RAX);
      save_rax(slot);
      pieces.push_back({slot++, false});
    }
    for (auto& arg : t.args) {
      const Type* at = operand_type(arg);
      uint64_t size = lay.size_of(at);
      if (slot + (lay.of(at).is_fat_pointer ? 2 : 1) > 14) {
        error(t.span, "too many arguments for a C call");
        return;
      }
      if (lay.of(at).is_fat_pointer) {
        if (arg.kind == Operand::Kind::Const) {
          load_const(arg.constant);
          save_rax(slot); pieces.push_back({slot++, false});
          a.mov_rr(RAX, RDX);
          save_rax(slot); pieces.push_back({slot++, false});
        } else {
          addr(arg.place, RSI);
          a.load(RAX, RSI, 0, 8, false);
          save_rax(slot); pieces.push_back({slot++, false});
          a.load(RAX, RSI, 8, 8, false);
          save_rax(slot); pieces.push_back({slot++, false});
        }
        continue;
      }
      if (size > 8) {
        error(t.span, "cannot pass a value of type '" + at->to_string() + "' to a C function");
        return;
      }
      if (is_float_type(at)) {
        load_operand_xmm0(arg);
        a.movq_rx(RAX, XMM0);
        save_rax(slot); pieces.push_back({slot++, true});
      } else {
        load_operand_rax(arg);
        save_rax(slot); pieces.push_back({slot++, false});
      }
    }
    int int_regs = 0, float_regs = 0;
    int max_int = target.win64_abi() ? 4 : 6;
    for (size_t i = 0; i < pieces.size(); ++i) {
      auto [sl, is_f] = pieces[i];
      if (target.win64_abi()) {
        // Position based: argument i uses register i (int or xmm).
        if (i >= 4) { error(t.span, "more than four C arguments are not supported on Windows yet"); return; }
        load_rax(sl);
        if (is_f) a.movq_xr(static_cast<Xmm>(i), RAX); else a.mov_rr(c_arg_reg(static_cast<int>(i)), RAX);
      } else {
        load_rax(sl);
        if (is_f) {
          if (float_regs >= 8) { error(t.span, "too many float arguments"); return; }
          a.movq_xr(static_cast<Xmm>(float_regs++), RAX);
        } else {
          if (int_regs >= max_int) { error(t.span, "too many integer arguments"); return; }
          a.mov_rr(c_arg_reg(int_regs++), RAX);
        }
      }
    }
    if (!target.win64_abi()) a.mov_ri(RAX, float_regs);  // al = number of vector registers (varargs rule)
    a.call_sym(symbol);
    if (t.diverges) { a.ud2(); return; }
    if (out_ptr || t.dest.is_none() || lay.size_of(dest_t) == 0) return;
    if (is_float_type(dest_t)) {
      a.movq_rx(RAX, XMM0);
      save_rax(0);
      addr(t.dest, RDI);
      load_rax(0);
      a.movq_xr(XMM0, RAX);
      store_xmm0_to_rdi(dest_t);
      return;
    }
    write_scalar(t.dest);
  }

  // ----- intrinsics generated inline -----

  bool gen_intrinsic(const Terminator& t) {
    const std::string& name = t.callee.builtin;
    if (name == "drop_in_place") {
      if (t.callee.type_args.size() != 1 || t.args.size() != 1) { error(t.span, "invalid drop_in_place arguments"); return true; }
      const Type* type = t.callee.type_args[0];
      if (type_needs_drop(type)) {
        load_operand_rax(t.args[0]); save_rax(0);
        drop_at_slot(type, 0, t.span);
      }
      return true;
    }
    if (name == "size_of") {
      if (t.callee.type_args.empty()) { error(t.span, "size_of needs a type argument"); return true; }
      a.mov_ri(RAX, static_cast<int64_t>(lay.size_of(t.callee.type_args[0])));
      write_scalar(t.dest);
      return true;
    }
    if (name == "str_from_raw") {
      load_operand_rax(t.args[1]);
      save_rax(1);
      load_operand_rax(t.args[0]);
      save_rax(0);
      addr(t.dest, RDI);
      load_rax(0);
      a.store(RDI, 0, RAX, 8);
      load_rax(1);
      a.store(RDI, 8, RAX, 8);
      return true;
    }
    if (name == "str_len" || name == "str_as_ptr") {
      const Operand& s = t.args[0];
      if (s.kind == Operand::Kind::Const) {
        load_const(s.constant);
        if (name == "str_len") a.mov_rr(RAX, RDX);
      } else {
        addr(s.place, RSI);
        a.load(RAX, RSI, name == "str_len" ? 8 : 0, 8, false);
      }
      write_scalar(t.dest);
      return true;
    }
    return false;
  }

  // ----- Synx calls -----

  void gen_synx_call(const Terminator& t) {
    const Type* dest_t = t.dest.is_none() ? ctx.void_type() : place_type(t.dest);
    uint64_t ret_size = lay.size_of(dest_t);
    bool hidden = ret_size > 16;
    uint32_t off = 0;
    if (hidden) {
      addr(t.dest, RAX);
      a.store(RSP, 0, RAX, 8);
      off = 8;
    }
    for (size_t i = 0; i < t.args.size(); ++i) {
      const auto& arg = t.args[i];
      bool erased_receiver = t.callee.kind == ir::Callee::Kind::Virtual && i == 0;
      uint64_t size = erased_receiver ? 8 : lay.size_of(operand_type(arg));
      if (size == 0) continue;
      if (erased_receiver) {
        // The concrete method takes a thin reference to its concrete Self.
        load_operand_rax(arg);
        a.store(RSP, static_cast<int32_t>(off), RAX, 8);
      } else {
        a.lea(RDI, RSP, static_cast<int32_t>(off));
        store_operand_to_rdi(arg);
      }
      off += static_cast<uint32_t>(round8(size));
    }
    switch (t.callee.kind) {
      case ir::Callee::Kind::Direct:
        a.call_sym(Axon::symbol_for(t.callee.fn->name));
        break;
      case ir::Callee::Kind::Virtual:
        assert(!t.args.empty() && t.args[0].kind != Operand::Kind::Const);
        addr(t.args[0].place, RAX);
        a.load(RAX, RAX, 8, 8, false);
        a.load(RAX, RAX, 24 + static_cast<int32_t>(t.callee.vtable_slot * 8), 8, false);
        a.call_reg(RAX);
        break;
      case ir::Callee::Kind::Indirect:
        // Reusable calls borrow the pair; a consuming call moves the pair.
        if (operand_type(t.callee.fn_operand)->kind == TypeKind::Fn) addr(t.callee.fn_operand.place, RAX);
        else load_operand_rax(t.callee.fn_operand);
        a.load(R10, RAX, 8, 8, false);
        a.load(RAX, RAX, 0, 8, false);
        a.call_reg(RAX);
        break;
      default: break;
    }
    if (t.diverges) { a.ud2(); return; }
    if (hidden || ret_size == 0 || t.dest.is_none()) return;
    if (ret_size <= 8) {
      write_scalar(t.dest);
      return;
    }
    save_rax(0);
    if (ret_size > 8) save_reg(RDX, 1);
    addr(t.dest, RDI);
    load_rax(0);
    if (ret_size <= 8) {
      store_scalar(ret_size, RDI, 0);
    } else {
      a.store(RDI, 0, RAX, 8);
      load_rax(1);
      store_scalar(ret_size - 8, RDI, 8);
    }
  }

  void gen_terminator(const Terminator& t) {
    switch (t.kind) {
      case Terminator::Kind::Goto:
        a.jmp(block_labels[t.target]);
        return;
      case Terminator::Kind::Switch: {
        load_operand_rax(t.discr);
        for (auto& [value, target] : t.cases) {
          a.mov_ri(RCX, static_cast<int64_t>(value));
          a.alu_rr(Alu::Cmp, RAX, RCX);
          a.jcc(Cond::E, block_labels[target]);
        }
        a.jmp(block_labels[t.otherwise]);
        return;
      }
      case Terminator::Kind::Assert: {
        load_operand_rax(t.cond);
        a.test_rr(RAX, RAX);
        a.jcc(Cond::NE, block_labels[t.target]);
        emit_panic(t.message, t.span);
        return;
      }
      case Terminator::Kind::Return:
        a.jmp(epilogue_label);
        return;
      case Terminator::Kind::Unreachable:
        emit_panic("entered unreachable code", t.span);
        return;
      case Terminator::Kind::Call: {
        if (t.callee.kind == ir::Callee::Kind::Builtin && gen_intrinsic(t)) {
          a.jmp(block_labels[t.target]);
          return;
        }
        if (t.callee.kind == ir::Callee::Kind::Builtin) {
          auto it = runtime_table().find(t.callee.builtin);
          std::string symbol = it != runtime_table().end() ? it->second.name : t.callee.builtin;
          bool rs = it != runtime_table().end() && it->second.returns_struct;
          gen_c_call(t, symbol, rs);
        } else {
          gen_synx_call(t);
        }
        if (!t.diverges) a.jmp(block_labels[t.target]);
        return;
      }
    }
  }

  // ----- frame and body -----

  void compute_frame() {
    if (target.register_allocation) allocation = allocate_registers(f, lay);
    else {
      allocation.registers.assign(f.locals.size(), -1);
      allocation.saved.clear();
    }
    size_t n = f.locals.size();
    slot_off.assign(n, std::numeric_limits<int32_t>::min());
    const Type* ret_t = f.locals[0].type;
    big_ret = lay.size_of(ret_t) > 16;
    // Arguments: positive offsets.
    int32_t arg_off = 16 + (big_ret ? 8 : 0);
    for (uint32_t i = 1; i <= f.arg_count; ++i) {
      if (f.closure && i == 1) continue;  // environment arrives in R10
      slot_off[i] = arg_off;
      arg_off += static_cast<int32_t>(round8(lay.size_of(f.locals[i].type)));
    }
    // Everything else: negative offsets.
    int32_t off = 0;
    for (size_t i = 0; i < n; ++i) {
      if (i >= 1 && i <= f.arg_count && !(f.closure && i == 1)) continue;
      if (i == 0 && big_ret) continue;
      if (allocation.registers[i] >= 0) continue;
      uint64_t size = round8(lay.size_of(f.locals[i].type));
      if (size == 0) size = 8;
      off += static_cast<int32_t>(size);
      slot_off[i] = -off;
    }
    for (size_t i = 0; i < allocation.saved.size(); ++i) {
      off += 8;
      saved_off.push_back(-off);
    }
    off += 8 * static_cast<int32_t>(scratch_slots);
    scratch_off = -off;
    // Outgoing area.
    max_out = target.win64_abi() ? 32 : 8;  // drop glue can call a Synx destructor
    for (auto& b : f.blocks) {
      if (b.term.kind != Terminator::Kind::Call) continue;
      uint32_t size = 0;
      const Type* dest_t = b.term.dest.is_none() ? ctx.void_type() : place_type(b.term.dest);
      if (lay.size_of(dest_t) > 16) size += 8;
      for (auto& arg : b.term.args) size += static_cast<uint32_t>(round8(lay.size_of(operand_type(arg))));
      if (size > max_out) max_out = size;
    }
    frame_size = static_cast<uint32_t>(LayoutTable::align_up(static_cast<uint64_t>(off) + max_out, 16));
  }
  uint32_t frame_size = 0;

  bool generate(const Type* drop_body = nullptr) {
    compute_frame();
    if (!ok) return false;
    for (size_t i = 0; i < f.blocks.size(); ++i) block_labels.push_back(a.new_label());
    epilogue_label = a.new_label();

    a.push(RBP);
    a.mov_rr(RBP, RSP);
    a.alu_ri(Alu::Sub, RSP, static_cast<int32_t>(frame_size));
    // Windows commits stack pages through one guard page. Touch each page of
    // a large frame from the top down before any access can skip the guard.
    // RAX holds no argument at entry on either ABI.
    if (target.win64_abi())
      for (uint32_t probe = 4096; probe <= frame_size; probe += 4096)
        a.load(RAX, RBP, -static_cast<int32_t>(probe), 8, false);
    for (size_t i = 0; i < allocation.saved.size(); ++i)
      a.store(RBP, saved_off[i], allocation.saved[i], 8);
    // Spill before adding any prologue calls (including Windows stack probes).
    if (f.closure) a.store(RBP, slot_off[1], R10, 8);
    for (uint32_t i = 1; i <= f.arg_count; ++i) {
      if (allocation.registers[i] < 0) continue;
      load_scalar(f.locals[i].type, RBP, slot_off[i]);
      write_scalar(Place::of(i));
    }

    if (drop_body) {
      a.load(RAX, RBP, 16, 8, false);
      save_rax(0);
      drop_body_at_slot(drop_body, 0, Span{});
    }

    for (size_t b = 0; b < f.blocks.size(); ++b) {
      a.bind(block_labels[b]);
      for (auto& s : f.blocks[b].stmts) {
        switch (s.kind) {
          case Statement::Kind::Assign: gen_assign(s); break;
          case Statement::Kind::Drop: case Statement::Kind::DropIf: gen_drop(s); break;
          case Statement::Kind::StorageDead: case Statement::Kind::Nop: break;
        }
        if (!ok) return false;
      }
      gen_terminator(f.blocks[b].term);
      if (!ok) return false;
    }

    a.bind(epilogue_label);
    const Type* ret_t = f.locals[0].type;
    uint64_t ret_size = lay.size_of(ret_t);
    if (!big_ret && ret_size > 0) {
      a.lea(RSI, RBP, slot_off[0]);
      if (ret_size <= 8) {
        load_scalar(ret_t, RSI, 0);
      } else {
        a.load(RAX, RSI, 0, 8, false);
        a.load(RDX, RSI, 8, 8, false);
      }
    }
    for (size_t i = 0; i < allocation.saved.size(); ++i)
      a.load(allocation.saved[i], RBP, saved_off[i], 8, false);
    a.mov_rr(RSP, RBP);
    a.pop(RBP);
    a.ret();
    a.finish();
    return ok;
  }
};

bool Axon::generate(ObjectFile& out) {
  // Local symbol for the read only data section.
  ObjSymbol rodata;
  rodata.name = "synx_rodata";
  rodata.offset = 0;
  rodata.is_text = false;
  rodata.global = false;
  out.symbols.push_back(rodata);

  // Vtable header: size, alignment, optional complete cleanup; then methods in declaration
  // order. Absolute relocations are applied in the read-only data section.
  auto word = [&](uint64_t value) {
    for (int i = 0; i < 8; ++i) out.rodata.push_back(static_cast<uint8_t>(value >> (i * 8)));
  };
  for (auto& table : module_.vtables) {
    while (out.rodata.size() % 8) out.rodata.push_back(0);
    out.symbols.push_back({symbol_for(table->name), out.rodata.size(), false, false, true});
    word(layouts_.size_of(table->concrete));
    word(layouts_.of(table->concrete).align);
    if (module_.drop_types.count(table->concrete))
      out.relocs.push_back({out.rodata.size(), drop_symbol(table->concrete), RelocKind::Abs64, 0, false});
    word(0);
    for (auto* method : table->methods) {
      out.relocs.push_back({out.rodata.size(), symbol_for(method->name), RelocKind::Abs64, 0, false});
      word(0);
    }
  }

  bool ok = true;
  for (auto& fn : module_.functions) {
    if (!fn->lowered) continue;
    if (fn->def && (fn->def->is_builtin || !fn->def->ast || !fn->def->ast->body)) continue;
    FunctionGen gen(*this, prog_, ctx_, layouts_, *fn, target_, sink_, out);
    if (!gen.generate()) { ok = false; continue; }
    ObjSymbol sym;
    sym.name = symbol_for(fn->name);
    sym.offset = out.text.size();
    sym.is_text = true;
    sym.global = true;
    out.symbols.push_back(sym);
    for (auto& r : gen.a.relocs) out.relocs.push_back({out.text.size() + r.offset, r.symbol, r.kind, r.addend});
    out.text.insert(out.text.end(), gen.a.code.begin(), gen.a.code.end());
    while (out.text.size() % 16) out.text.push_back(0xCC);
  }
  if (!ok) return false;

  for (size_t i = 0; i < drop_worklist_.size(); ++i) {
    const Type* type = drop_worklist_[i];
    ir::Function helper;
    helper.name = "@drop." + type->to_string();
    helper.ret = ctx_.void_type(); helper.arg_count = 1;
    helper.locals.push_back({ctx_.void_type(), "", false, {}});
    helper.locals.push_back({ctx_.ptr(type, false), "value", true, {}});
    FunctionGen gen(*this, prog_, ctx_, layouts_, helper, target_, sink_, out);
    gen.scratch_slots = 3;
    if (!gen.generate(type)) { ok = false; continue; }
    out.symbols.push_back({drop_symbol(type), out.text.size(), true, true, true});
    for (auto& r : gen.a.relocs) out.relocs.push_back({out.text.size() + r.offset, r.symbol, r.kind, r.addend});
    out.text.insert(out.text.end(), gen.a.code.begin(), gen.a.code.end());
    while (out.text.size() % 16) out.text.push_back(0xCC);
  }
  if (!ok) return false;

  // C ABI bridge for a heap-owned, zero-argument consuming closure.
  // The closure cleans its environment; the C boot function frees the pair.
  {
    Assembler a;
    a.push(RBP);
    a.mov_rr(RBP, RSP);
    a.alu_ri(Alu::Sub, RSP, target_.win64_abi() ? 48 : 16);
    if (target_.win64_abi()) {
      a.store(RBP, -8, RSI, 8);
      a.store(RBP, -16, RDI, 8);
    }
    a.mov_rr(RAX, target_.win64_abi() ? RCX : RDI);
    a.load(R10, RAX, 8, 8, false);
    a.load(RAX, RAX, 0, 8, false);
    a.call_reg(RAX);
    if (target_.win64_abi()) {
      a.load(RSI, RBP, -8, 8, false);
      a.load(RDI, RBP, -16, 8, false);
    }
    a.mov_rr(RSP, RBP);
    a.pop(RBP);
    a.ret();
    a.finish();
    out.symbols.push_back({"synx_thread_entry", out.text.size(), true, true, true});
    out.text.insert(out.text.end(), a.code.begin(), a.code.end());
  }

  // Entry stub: int synx_entry(void) calls main and returns its exit code.
  if (module_.entry) {
    Assembler a;
    const Type* ret = module_.entry->ret;
    a.push(RBP);
    a.mov_rr(RBP, RSP);
    a.alu_ri(Alu::Sub, RSP, target_.win64_abi() ? 48 : 32);
    // Synx uses rsi/rdi as scratch registers. Preserve the C caller's
    // nonvolatile registers at the Windows entry boundary.
    if (target_.win64_abi()) {
      a.store(RBP, -8, RSI, 8);
      a.store(RBP, -16, RDI, 8);
    }
    a.call_sym(symbol_for(module_.entry->name));
    if (!ret || ret->is_void()) a.mov_ri(RAX, 0);
    else a.movsx32(RAX, RAX);
    if (target_.win64_abi()) {
      a.load(RSI, RBP, -8, 8, false);
      a.load(RDI, RBP, -16, 8, false);
    }
    a.mov_rr(RSP, RBP);
    a.pop(RBP);
    a.ret();
    a.finish();
    ObjSymbol sym;
    sym.name = "synx_entry";
    sym.offset = out.text.size();
    sym.global = true;
    out.symbols.push_back(sym);
    for (auto& r : a.relocs) out.relocs.push_back({out.text.size() + r.offset, r.symbol, r.kind, r.addend});
    out.text.insert(out.text.end(), a.code.begin(), a.code.end());
  }
  if (out.rodata.empty()) out.rodata.push_back(0);
  return true;
}

}  // namespace synx::codegen
