// Declaration oracle for self-hosting stage 4a. Records use creation-order IDs.
// Parity: tests/selfhost_modules_test.cpp and selfhost/module_dump.snx.
#include "synx/compile.hpp"
#include "synx/axon.hpp"
#include "synx/layout.hpp"
#include "synx/regalloc.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>
#include <unordered_map>

namespace synx {
namespace {
using namespace sema;
std::string quote(const std::string& text) {
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string out = "\"";
  for (unsigned char c : text) {
    if (c < 32 || c > 126 || c == '"' || c == '\\') {
      out += "\\x"; out += hex[c >> 4]; out += hex[c & 15];
    } else out += static_cast<char>(c);
  }
  return out + '"';
}
struct Dump {
  const Program& p;
  std::ostringstream out;
  std::unordered_map<const void*, std::string> ids;
  template<class T> void index(const std::vector<std::unique_ptr<T>>& values, const char* kind) {
    for (size_t i = 0; i < values.size(); ++i) ids[values[i].get()] = std::string(kind) + '#' + std::to_string(i);
  }
  explicit Dump(const Program& program) : p(program) {
    index(p.modules, "module"); index(p.structs, "struct"); index(p.enums, "enum");
    index(p.traits, "trait"); index(p.impls, "impl"); index(p.functions, "fn"); index(p.consts, "const");
    for (auto& e : p.enums) for (auto& v : e->variants) ids[&v] = id(e.get()) + ".variant#" + std::to_string(v.index);
  }
  std::string id(const void* ptr) const { return ptr ? ids.at(ptr) : "nil"; }
  void span(Span s) { out << " @" << s.start << '-' << s.end << ':' << s.file; }
  // Parameter identities in pre-order (inner, elems, ret): names alone cannot
  // distinguish a method's T from its owner's T.
  void params(const Type* t, std::string& text) const {
    if (!t) return;
    if (t->kind == TypeKind::Param) { text += (text.empty() ? "" : " ") + id(t->owner) + ':' + std::to_string(t->index); return; }
    params(t->inner, text);
    for (auto* e : t->elems) params(e, text);
    params(t->ret, text);
  }
  void type(const Type* t) {
    out << ' ' << (t ? quote(t->to_string()) : "nil");
    std::string text; params(t, text);
    if (!text.empty()) out << '{' << text << '}';
  }
  // A bound prints as (trait type...); an unresolved trait prints nil.
  void bound(const TraitRef& b) {
    out << '(' << id(b.trait); for (auto* a : b.args) type(a); out << ')';
  }
  void generics(const std::vector<GenericParamDef>& gs) {
    out << " [";
    for (auto& g : gs) {
      out << '(' << quote(g.name); span(g.span); type(g.type);
      out << " ["; for (auto& b : g.bounds) bound(b); out << "])";
    }
    out << ']';
  }
  void types(const std::vector<const Type*>& ts) { out << " ["; for (auto* t : ts) type(t); out << ']'; }
  void fields(const std::vector<FieldDef>& fs) {
    out << " [";
    for (auto& f : fs) { out << '(' << quote(f.name); span(f.span); type(f.type); out << ' ' << f.is_pub << ')'; }
    out << ']';
  }
  void methods(const std::vector<FunctionDef*>& ms) {
    out << " ["; for (auto* f : ms) out << id(f) << ' '; out << ']';
  }
  void symbols(const std::map<std::string, Symbol>& table) {
    out << " [\n";
    for (auto& [name, s] : table) {
      out << "  " << quote(name) << ' ' << static_cast<int>(s.kind) << ' '
          << id(s.struct_def) << ' ' << id(s.enum_def) << ' ' << id(s.trait_def) << ' '
          << id(s.fn_def) << ' ' << id(s.const_def) << ' ' << id(s.variant) << ' ' << id(s.module_def)
          << ' ' << s.is_pub; span(s.span); out << '\n';
    }
    out << "]\n";
  }
  std::string run(const Resolver* resolver, const Checker* checker) {
    out << "modules " << id(p.root_module) << ' ' << id(p.active_module) << '\n';
    for (auto& m : p.modules) {
      out << id(m.get()) << ' ' << quote(m->name) << ' ' << m->file << ' ' << m->is_prelude
          << ' ' << id(m->namespace_owner) << ' ' << id(m->base_module); symbols(m->symbols);
    }
    auto nominal = [&](const auto& values, int kind) {
      for (auto& d : values) {
        out << id(d.get()) << ' ' << quote(d->name); span(d->span);
        out << ' ' << id(d->module) << ' ' << d->is_pub;
        if constexpr (requires { d->is_builtin_string; }) out << ' ' << d->is_builtin_string << ' ' << d->is_builtin_vec;
        type(d->self_type); generics(d->generics); methods(d->methods);
        if constexpr (requires { d->fields; }) fields(d->fields);
        out << '\n';
        if constexpr (requires { d->variants; }) for (auto& v : d->variants) {
          out << id(&v) << ' ' << quote(v.name); span(v.span);
          out << ' ' << static_cast<int>(v.kind) << ' ' << id(v.owner); types(v.tuple_types); fields(v.fields); out << '\n';
        }
      }
      (void)kind;
    };
    nominal(p.structs, 0); nominal(p.enums, 1); nominal(p.traits, 2);
    for (auto& d : p.impls) {
      out << id(d.get()); span(d->span); out << ' ' << id(d->module);
      type(d->self_type); out << ' '; bound(d->trait); generics(d->generics); methods(d->methods); out << '\n';
    }
    for (auto& d : p.functions) {
      out << id(d.get()) << ' ' << quote(d->name); span(d->span);
      out << ' ' << id(d->module) << ' ' << d->is_pub << ' ' << d->is_builtin << ' ' << d->is_prelude
          << ' ' << static_cast<int>(d->owner) << ' ' << id(d->owner_struct) << ' ' << id(d->owner_enum)
          << ' ' << id(d->owner_trait) << ' ' << id(d->owner_impl) << ' ' << d->ret_declared
          << ' ' << d->checked << ' ' << d->checking;
      type(d->self_type); type(d->ret); generics(d->generics);
      out << " [";
      for (auto& a : d->params) {
        out << '(' << quote(a.name); span(a.span); out << ' ' << static_cast<int>(a.self_kind); type(a.type); out << ')';
      }
      out << "]\n";
    }
    for (auto& d : p.consts) {
      out << id(d.get()) << ' ' << quote(d->name); span(d->span);
      out << ' ' << id(d->module) << ' ' << d->is_pub << ' ' << d->checked << ' ' << d->checking; type(d->type); out << '\n';
    }
    out << "globals"; symbols(p.globals);
    out << "prelude " << id(p.option_def) << ' ' << id(p.result_def) << ' ' << id(p.string_def) << ' ' << id(p.vec_def)
        << ' ' << id(p.range_def) << ' ' << id(p.range_inclusive_def) << ' ' << id(p.task_def)
        << ' ' << id(p.send_task_def) << ' ' << id(p.waker_def) << ' ' << id(p.poll_def) << '\n';
    for (auto& [name, trait] : p.core_traits) out << "core " << quote(name) << ' ' << id(trait) << '\n';
    auto pending = [&](const char* name, const auto& values) {
      out << "pending " << name << " [";
      for (auto* value : values) out << id(value) << ' ';
      out << "]\n";
    };
    if (resolver) {
      pending("structs", resolver->pending_structs()); pending("enums", resolver->pending_enums());
      pending("traits", resolver->pending_traits()); pending("impls", resolver->pending_impls());
      pending("functions", resolver->pending_functions()); pending("consts", resolver->pending_consts());
    }
    if (checker) pending("str_methods", checker->builtin_str_methods());
    return out.str();
  }
  // Checker tables in insertion order. Nodes print as kind and span; the
  // order identifies nodes whose kind and span coincide.
  void node(const ast::Expr* e) { out << static_cast<int>(e->kind); span(e->span); }
  void locals(const std::vector<LocalVar>& values) {
    out << " [";
    for (auto& l : values) { out << '(' << quote(l.name); type(l.type); out << ' ' << l.is_const; span(l.span); out << ')'; }
    out << ']';
  }
  void numbers(const std::vector<uint32_t>& values) { out << " ["; for (auto v : values) out << v << ' '; out << ']'; }
  template <class Map, class Key, class Value>
  void table(const char* name, const Map& map, Key key, Value value) {
    out << name << ' ' << map.size() << '\n';
    for (auto& [k, v] : map) { out << "  "; key(k); value(v); out << '\n'; }
  }
  std::string check(const CheckResult& r) {
    auto expr = [&](const ast::Expr* e) { node(e); };
    auto pattern = [&](const ast::Pattern* p) { out << static_cast<int>(p->kind); span(p->span); };
    table("expr_types", r.expr_types, expr, [&](const Type* t) { type(t); });
    table("refs", r.refs, expr, [&](const NameRef& ref) {
      out << ' ' << static_cast<int>(ref.kind) << ' ' << ref.local_id << ' ' << id(ref.fn) << ' ' << id(ref.impl)
          << ' ' << id(ref.trait) << ' ' << id(ref.variant) << ' ' << id(ref.const_def) << ' ' << id(ref.struct_def)
          << ' ' << id(ref.enum_def) << ' ' << ref.field_index << ' ' << ref.auto_derefs;
      types(ref.type_args); types(ref.owner_args);
    });
    table("coercions", r.coercions, expr, [&](const Coercion& c) {
      out << ' ' << static_cast<int>(c.kind) << ' ' << c.borrow_first; type(c.target);
    });
    table("decl_locals", r.decl_locals, [&](const ast::VarDeclStmt* s) { span(s->span); }, [&](uint32_t v) { out << ' ' << v; });
    table("pattern_locals", r.pattern_locals, pattern, [&](uint32_t v) { out << ' ' << v; });
    table("pattern_derefs", r.pattern_derefs, pattern, [&](int v) { out << ' ' << v; });
    table("fn_locals", r.fn_locals, [&](const FunctionDef* f) { out << id(f); }, [&](const auto& v) { locals(v); });
    table("const_locals", r.const_locals, [&](const ConstDef* c) { out << id(c); }, [&](const auto& v) { locals(v); });
    table("closure_params", r.closure_params, expr, [&](const auto& v) { numbers(v); });
    table("closure_captures", r.closure_captures, expr, [&](const auto& v) { numbers(v); });
    table("closure_capture_modes", r.closure_capture_modes, expr, [&](const auto& modes) {
      out << " ["; for (auto& [local, mode] : modes) out << '(' << local << ' ' << static_cast<int>(mode) << ')'; out << ']';
    });
    table("closure_locals", r.closure_locals, expr, [&](const auto& v) { locals(v); });
    table("shorthand_locals", r.shorthand_locals, [&](const ast::FieldInit* f) { span(f->span); }, [&](uint32_t v) { out << ' ' << v; });
    return out.str();
  }

  // ----- Dendrite -----
  // Functions are f#index and vtables v#index in module order. A place prints
  // as _local followed by .fN field, [_N] index, .* deref, and .vN downcast.
  std::unordered_map<const void*, std::string> ir_ids;
  std::string ir_id(const void* p) const { return p ? ir_ids.at(p) : "nil"; }
  void place(const ir::Place& where) {
    if (where.is_none()) { out << "none"; return; }
    out << '_' << where.local;
    for (auto& pr : where.proj) {
      switch (pr.kind) {
        case ir::ProjKind::Field: out << ".f" << pr.index; break;
        case ir::ProjKind::Index: out << "[_" << pr.index << ']'; break;
        case ir::ProjKind::Deref: out << ".*"; break;
        case ir::ProjKind::Downcast: out << ".v" << pr.index; break;
      }
    }
  }
  void constant(const ir::Constant& k) {
    uint64_t bits = 0;
    static_assert(sizeof bits == sizeof k.float_value);
    std::memcpy(&bits, &k.float_value, sizeof bits);
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(bits));
    out << "(k" << static_cast<int>(k.kind) << ' ' << k.int_value << ' ' << hex << ' ' << quote(k.str_value) << ' ' << ir_id(k.fn);
    type(k.type); out << ')';
  }
  void operand(const ir::Operand& o) {
    switch (o.kind) {
      case ir::Operand::Kind::Copy: out << "copy "; place(o.place); break;
      case ir::Operand::Kind::Move: out << "move "; place(o.place); break;
      case ir::Operand::Kind::Const: out << "const "; constant(o.constant); break;
    }
  }
  void operands(const std::vector<ir::Operand>& values) {
    out << " ["; for (auto& o : values) { out << '('; operand(o); out << ')'; } out << ']';
  }
  void rvalue(const ir::Rvalue& r) {
    out << "rv" << static_cast<int>(r.kind) << " a=("; operand(r.a); out << ") b=("; operand(r.b); out << ") place=";
    place(r.place);
    out << " const=" << r.is_const << " bop=" << static_cast<int>(r.bop) << " uop=" << static_cast<int>(r.uop) << " type=";
    type(r.type);
    out << " agg=" << static_cast<int>(r.agg) << " variant=" << r.variant << " elems="; operands(r.elems);
    out << " captures="; operands(r.captures); out << " vtable=" << ir_id(r.vtable);
  }
  void terminator(const ir::Terminator& t) {
    out << "    term" << static_cast<int>(t.kind) << " target=" << t.target << " discr=("; operand(t.discr);
    out << ") cases=["; for (auto& [value, block] : t.cases) out << '(' << value << ' ' << block << ')';
    out << "] otherwise=" << t.otherwise << " callee=(" << static_cast<int>(t.callee.kind) << ' ' << ir_id(t.callee.fn) << ' '
        << quote(t.callee.builtin);
    types(t.callee.type_args); out << " ("; operand(t.callee.fn_operand); out << ") " << t.callee.vtable_slot << ") args=";
    operands(t.args);
    out << " dest="; place(t.dest);
    out << " diverges=" << t.diverges << " cond=("; operand(t.cond); out << ") message=" << quote(t.message); span(t.span); out << '\n';
  }
  void numbers(const std::set<uint32_t>& values) { out << " ["; for (auto v : values) out << v << ' '; out << ']'; }
  void function(const ir::Function& f) {
    out << ir_id(&f) << ' ' << quote(f.name) << " def=" << id(f.def) << " owner="; types(f.owner_args);
    out << " args="; types(f.type_args); out << " ret="; type(f.ret);
    out << " argc=" << f.arg_count << " lowered=" << f.lowered << " closure=";
    if (f.closure) node(f.closure); else out << "nil";
    out << " closure_type="; type(f.closure_type); out << " env="; type(f.environment_type);
    out << " async=" << f.async_body << ' ' << ir_id(f.async_poll) << ' ' << ir_id(f.async_cancel) << ' ' << f.async_waker << '\n';
    out << "  suspends ["; for (auto& [a, b] : f.async_suspends) out << '(' << a << ' ' << b << ')'; out << "]\n";
    out << "  drop_locals"; numbers(f.async_drop_locals); out << '\n';
    out << "  drop_flags ["; for (auto& [a, b] : f.async_drop_flags) out << '(' << a << ' ' << b << ')'; out << "]\n";
    out << "  live ["; for (auto& [b, locals] : f.async_live) { out << '(' << b; numbers(locals); out << ')'; } out << "]\n";
    out << "  borrows [";
    for (auto& [b, pairs] : f.async_borrows) { out << '(' << b << " ["; for (auto& [x, y] : pairs) out << '(' << x << ' ' << y << ')'; out << "])"; }
    out << "]\n";
    for (size_t i = 0; i < f.locals.size(); ++i) {
      auto& l = f.locals[i];
      out << "  _" << i; type(l.type); out << ' ' << quote(l.name) << " arg=" << l.is_arg << " escape=" << l.escape_boundary; span(l.span); out << '\n';
    }
    for (size_t b = 0; b < f.blocks.size(); ++b) {
      out << "  bb" << b << '\n';
      for (auto& s : f.blocks[b].stmts) {
        out << "    s" << static_cast<int>(s.kind) << ' '; place(s.place);
        if (s.kind == ir::Statement::Kind::Assign) { out << " = "; rvalue(s.rvalue); }
        if (s.kind == ir::Statement::Kind::DropIf) out << " if _" << s.flag_local;
        span(s.span); out << '\n';
      }
      terminator(f.blocks[b].term);
    }
  }
  std::string ir(const ir::Module& m) {
    for (size_t i = 0; i < m.functions.size(); ++i) ir_ids[m.functions[i].get()] = "f#" + std::to_string(i);
    for (size_t i = 0; i < m.vtables.size(); ++i) ir_ids[m.vtables[i].get()] = "v#" + std::to_string(i);
    out << "ir " << m.functions.size() << " entry=" << ir_id(m.entry) << '\n';
    for (auto& f : m.functions) function(*f);
    for (auto& v : m.vtables) {
      out << ir_id(v.get()) << ' ' << quote(v->name); type(v->concrete); type(v->dyn_type);
      out << " ["; for (auto* f : v->methods) out << ir_id(f) << ' '; out << "]\n";
    }
    // Drop tables are lookups keyed by interned types; print them by type text.
    std::vector<std::pair<std::string, std::string>> impls;
    for (auto& [t, f] : m.drop_impls) impls.push_back({t->to_string(), ir_id(f)});
    std::sort(impls.begin(), impls.end());
    for (auto& [t, f] : impls) out << "drop_impl " << quote(t) << ' ' << f << '\n';
    std::vector<std::string> drops;
    for (auto* t : m.drop_types) drops.push_back(t->to_string());
    std::sort(drops.begin(), drops.end());
    for (auto& t : drops) out << "drop_type " << quote(t) << '\n';
    return out.str();
  }
};
}
std::string dump_ir_strict(const Compilation& c) { return Dump(c.program).ir(c.ir_module); }
std::string dump_machine(Compilation& c) {
  codegen::LayoutTable lay(c.program, c.types);
  std::ostringstream out;
  // size align payload_offset fat [field offsets]
  auto layout = [&](const codegen::Layout& l) {
    out << ' ' << l.size << ' ' << l.align << ' ' << l.payload_offset << ' ' << l.is_fat_pointer << " [";
    for (auto offset : l.field_offsets) out << offset << ' ';
    out << ']';
  };
  out << "machine " << c.ir_module.functions.size() << '\n';
  for (size_t i = 0; i < c.ir_module.functions.size(); ++i) {
    const auto& f = *c.ir_module.functions[i];
    auto allocation = codegen::allocate_registers(f, lay);
    out << "f#" << i << " saved=[";
    for (auto reg : allocation.saved) out << static_cast<int>(reg) << ' ';
    out << "]\n";
    for (size_t l = 0; l < f.locals.size(); ++l) {
      const auto* t = f.locals[l].type;
      out << "  _" << l << " reg=" << allocation.registers[l];
      if (!t) { out << " nil\n"; continue; }
      layout(lay.of(t));
      if (t->kind == TypeKind::Enum) {
        out << " variants=[";
        for (uint32_t v = 0; v < t->enum_def->variants.size(); ++v) { out << '('; layout(lay.variant_payload(t, v)); out << ')'; }
        out << ']';
      }
      out << '\n';
    }
  }
  for (auto& v : c.ir_module.vtables) { out << "vtable " << quote(v->name); layout(lay.of(v->concrete)); out << '\n'; }
  return out.str();
}
std::string dump_objects(Compilation& c, bool register_allocation, bool checked_arithmetic) {
  std::ostringstream out;
  for (auto os : {codegen::TargetOs::Linux, codegen::TargetOs::Windows, codegen::TargetOs::MacOs}) {
    codegen::Target target{os, register_allocation, checked_arithmetic};
    codegen::Axon axon(c.program, c.types, c.ir_module, c.sink, target);
    codegen::ObjectFile obj;
    bool ok = axon.generate(obj);
    std::vector<uint8_t> bytes;
    if (ok) bytes = codegen::write_object(obj, target.format());
    uint32_t hash = 2166136261u;
    for (uint8_t b : bytes) { hash ^= b; hash *= 16777619u; }
    out << "object " << target.name() << ' ' << ok << ' ' << bytes.size() << ' ' << hash << '\n';
  }
  return out.str();
}
std::string dump_check(const Compilation& c) {
  // Parse failures stop before the checker exists; print its empty tables.
  static const CheckResult empty;
  return "check\n" + Dump(c.program).check(c.checker ? c.checker->result() : empty);
}
std::string dump_declarations(const Compilation& c) {
  auto text = Dump(c.program).run(c.resolver.get(), c.checker.get());
  std::ostringstream out;
  out << text << "diagnostics " << c.sink.error_count() << '\n';
  for (auto& d : c.sink.all()) {
    out << static_cast<int>(d.severity) << " @" << d.span.start << '-' << d.span.end << ':' << d.span.file
        << ' ' << quote(d.code) << ' ' << quote(d.message) << '\n';
  }
  return out.str();
}
}  // namespace synx
