#include "synx/resolve.hpp"

#include <algorithm>

namespace synx::sema {

static bool has_impl_of(Program& prog, const char* trait_name, const Type* t) {
  TraitDef* trait = prog.core_trait(trait_name);
  if (!trait) return false;
  for (auto& impl : prog.impls) {
    if (impl->trait.trait != trait) continue;
    const Type* s = impl->self_type;
    if (s->kind != t->kind) continue;
    if (t->kind == TypeKind::Struct && s->struct_def == t->struct_def) return true;
    if (t->kind == TypeKind::Enum && s->enum_def == t->enum_def) return true;
  }
  return false;
}

bool is_copy_type(Program& prog, TypeContext& ctx, const Type* t) {
  switch (t->kind) {
    case TypeKind::Prim: case TypeKind::Ptr: case TypeKind::Never: case TypeKind::Error:
      return true;
    case TypeKind::Fn: return false;  // function values can own a closure environment
    case TypeKind::Infer: return t->infer_kind != InferKind::Any;
    case TypeKind::Ref: return t->is_const;
    case TypeKind::Array: return is_copy_type(prog, ctx, t->inner);
    case TypeKind::Tuple:
      for (auto* e : t->elems) if (!is_copy_type(prog, ctx, e)) return false;
      return true;
    case TypeKind::Struct: {
      if (t->struct_def == prog.string_def || t->struct_def == prog.vec_def) return false;
      if (has_impl_of(prog, "Drop", t)) return false;
      Subst s;
      for (size_t i = 0; i < t->struct_def->generics.size() && i < t->elems.size(); ++i) s.bind(t->struct_def->generics[i].type, t->elems[i]);
      for (auto& f : t->struct_def->fields) if (!is_copy_type(prog, ctx, s.apply(ctx, f.type))) return false;
      return true;
    }
    case TypeKind::Enum: {
      if (has_impl_of(prog, "Drop", t)) return false;
      Subst s;
      for (size_t i = 0; i < t->enum_def->generics.size() && i < t->elems.size(); ++i) s.bind(t->enum_def->generics[i].type, t->elems[i]);
      for (auto& v : t->enum_def->variants) {
        for (auto* tt : v.tuple_types) if (!is_copy_type(prog, ctx, s.apply(ctx, tt))) return false;
        for (auto& f : v.fields) if (!is_copy_type(prog, ctx, s.apply(ctx, f.type))) return false;
      }
      return true;
    }
    default: return false;
  }
}

Resolver::Resolver(Program& program, TypeContext& ctx, DiagnosticSink& sink)
    : prog_(program), ctx_(ctx), sink_(sink) {}

void Resolver::resolve_module(const ast::Module& module, bool is_prelude) {
  declare_module(module, prog_.active_module, is_prelude);
  resolve_signatures();
}

void Resolver::declare_module(const ast::Module& module, ModuleDef* scope, bool is_prelude) {
  ModuleScope active(prog_, scope);
  is_prelude_ = is_prelude;
  declare_items(module, is_prelude);
}

bool Resolver::declare_global(const std::string& name, Symbol sym) {
  auto& symbols = is_prelude_ || !prog_.active_module ? prog_.globals : prog_.active_module->symbols;
  switch (sym.kind) {
    case SymbolKind::Function: sym.is_pub = sym.fn_def->is_pub; break;
    case SymbolKind::Struct: sym.is_pub = sym.struct_def->is_pub; break;
    case SymbolKind::Enum: sym.is_pub = sym.enum_def->is_pub; break;
    case SymbolKind::Trait: sym.is_pub = sym.trait_def->is_pub; break;
    case SymbolKind::Const: sym.is_pub = sym.const_def->is_pub; break;
    default: break;
  }
  auto it = symbols.find(name);
  if (it != symbols.end()) {
    sink_.error(sym.span, "'" + name + "' is already defined", "SNX0302");
    sink_.note(it->second.span, "previous definition is here");
    return false;
  }
  symbols[name] = sym;
  return true;
}

const Symbol* Resolver::lookup_path(const ast::Path& path, size_t count, bool report_missing) {
  count = std::min(count, path.segments.size());
  if (!count) return nullptr;
  const Symbol* symbol = prog_.find(path.segments[0].name);
  for (size_t i = 1; symbol && i < count; ++i) {
    if (symbol->kind != SymbolKind::Module) { symbol = nullptr; break; }
    auto* module = symbol->module_def;
    const ModuleDef* declared = nullptr;
    symbol = module->find(path.segments[i].name, &declared);
    if (!symbol) break;
    if (declared != prog_.active_module && declared->namespace_owner != prog_.active_module && !symbol->is_pub) {
      sink_.error(path.segments[i].span, "'" + path.segments[i].name + "' is private to module '" + module->name + "'", "SNX0304");
      return nullptr;
    }
  }
  if (!symbol && report_missing) sink_.error(path.span, "unknown path '" + path.to_string() + "'", "SNX0301");
  return symbol;
}

FunctionDef* Resolver::new_function(const ast::Function& fn, FnOwner owner) {
  auto def = std::make_unique<FunctionDef>();
  def->name = fn.name;
  def->module = prog_.active_module;
  def->span = fn.name_span;
  def->ast = &fn;
  def->is_pub = fn.is_pub;
  def->is_prelude = is_prelude_;
  def->owner = owner;
  def->ret_declared = fn.ret != nullptr;
  FunctionDef* out = def.get();
  prog_.functions.push_back(std::move(def));
  pending_functions_.push_back(out);
  return out;
}

std::vector<GenericParamDef> Resolver::declare_generics(
    const std::vector<ast::GenericParam>& gs, const void* owner) {
  std::vector<GenericParamDef> out;
  for (size_t i = 0; i < gs.size(); ++i) {
    GenericParamDef g;
    g.name = gs[i].name;
    g.span = gs[i].span;
    g.type = ctx_.param(owner, static_cast<uint32_t>(i), gs[i].name);
    for (size_t j = 0; j < i; ++j) {
      if (out[j].name == g.name) sink_.error(g.span, "duplicate type parameter '" + g.name + "'");
    }
    out.push_back(std::move(g));
  }
  return out;
}

void Resolver::declare_items(const ast::Module& module, bool is_prelude) {
  for (auto& item : module.items) {
    switch (item->kind) {
      case ast::ItemKind::Import:
        // Modules are a later phase. Everything in the prelude is global.
        break;
      case ast::ItemKind::Const: {
        auto& c = static_cast<const ast::ConstItem&>(*item);
        auto def = std::make_unique<ConstDef>();
        def->module = prog_.active_module;
        def->name = c.name;
        def->span = c.span;
        def->ast = &c;
        def->is_pub = c.is_pub;
        Symbol sym{SymbolKind::Const};
        sym.const_def = def.get();
        sym.span = c.span;
        if (declare_global(c.name, sym)) pending_consts_.push_back(def.get());
        prog_.consts.push_back(std::move(def));
        break;
      }
      case ast::ItemKind::Function: {
        auto& f = static_cast<const ast::FunctionItem&>(*item);
        FunctionDef* def = new_function(*f.fn, FnOwner::Module);
        Symbol sym{SymbolKind::Function};
        sym.fn_def = def;
        sym.span = f.fn->name_span;
        declare_global(def->name, sym);
        break;
      }
      case ast::ItemKind::Struct: {
        auto& s = static_cast<const ast::StructItem&>(*item);
        auto def = std::make_unique<StructDef>();
        def->module = prog_.active_module;
        def->name = s.name;
        def->span = s.span;
        def->ast = &s;
        def->is_pub = s.is_pub;
        def->generics = declare_generics(s.generics, def.get());
        std::vector<const Type*> args;
        for (auto& g : def->generics) args.push_back(g.type);
        def->self_type = ctx_.struct_type(def.get(), args);
        for (auto& m : s.methods) {
          FunctionDef* fn = new_function(*m, FnOwner::Struct);
          fn->owner_struct = def.get();
          fn->self_type = def->self_type;
          for (auto* other : def->methods) {
            if (other->name == fn->name) sink_.error(fn->span, "duplicate method '" + fn->name + "'");
          }
          def->methods.push_back(fn);
        }
        if (is_prelude && s.name == "String") { def->is_builtin_string = true; prog_.string_def = def.get(); }
        if (is_prelude && s.name == "Vec") { def->is_builtin_vec = true; prog_.vec_def = def.get(); }
        if (is_prelude && s.name == "Range") prog_.range_def = def.get();
        if (is_prelude && s.name == "RangeInclusive") prog_.range_inclusive_def = def.get();
        if (is_prelude && s.name == "Task") prog_.task_def = def.get();
        if (is_prelude && s.name == "SendTask") prog_.send_task_def = def.get();
        if (is_prelude && s.name == "Waker") prog_.waker_def = def.get();
        Symbol sym{SymbolKind::Struct};
        sym.struct_def = def.get();
        sym.span = s.span;
        declare_global(s.name, sym);
        pending_structs_.push_back(def.get());
        prog_.structs.push_back(std::move(def));
        break;
      }
      case ast::ItemKind::Enum: {
        auto& e = static_cast<const ast::EnumItem&>(*item);
        auto def = std::make_unique<EnumDef>();
        def->module = prog_.active_module;
        def->name = e.name;
        def->span = e.span;
        def->ast = &e;
        def->is_pub = e.is_pub;
        def->generics = declare_generics(e.generics, def.get());
        std::vector<const Type*> args;
        for (auto& g : def->generics) args.push_back(g.type);
        def->self_type = ctx_.enum_type(def.get(), args);
        for (size_t i = 0; i < e.variants.size(); ++i) {
          VariantDef v;
          v.name = e.variants[i].name;
          v.span = e.variants[i].span;
          v.index = static_cast<uint32_t>(i);
          v.kind = e.variants[i].kind;
          v.owner = def.get();
          for (auto& prev : def->variants) {
            if (prev.name == v.name) sink_.error(v.span, "duplicate variant '" + v.name + "'");
          }
          def->variants.push_back(std::move(v));
        }
        for (auto& m : e.methods) {
          FunctionDef* fn = new_function(*m, FnOwner::Enum);
          fn->owner_enum = def.get();
          fn->self_type = def->self_type;
          def->methods.push_back(fn);
        }
        if (is_prelude && e.name == "Option") prog_.option_def = def.get();
        if (is_prelude && e.name == "Result") prog_.result_def = def.get();
        if (is_prelude && e.name == "Poll") prog_.poll_def = def.get();
        Symbol sym{SymbolKind::Enum};
        sym.enum_def = def.get();
        sym.span = e.span;
        declare_global(e.name, sym);
        // Prelude variants are visible by bare name: Some, None, Ok, Err.
        if (is_prelude && (e.name == "Option" || e.name == "Result")) {
          for (auto& v : def->variants) {
            Symbol vs{SymbolKind::Variant};
            vs.variant = &v;
            vs.enum_def = def.get();
            vs.span = v.span;
            declare_global(v.name, vs);
          }
        }
        pending_enums_.push_back(def.get());
        prog_.enums.push_back(std::move(def));
        break;
      }
      case ast::ItemKind::Trait: {
        auto& t = static_cast<const ast::TraitItem&>(*item);
        auto def = std::make_unique<TraitDef>();
        def->module = prog_.active_module;
        def->name = t.name;
        def->span = t.span;
        def->ast = &t;
        def->is_pub = t.is_pub;
        def->generics = declare_generics(t.generics, def.get());
        def->self_type = ctx_.param(def.get(), 0xFFFF, "Self");
        for (auto& m : t.methods) {
          FunctionDef* fn = new_function(*m, FnOwner::Trait);
          fn->owner_trait = def.get();
          fn->self_type = def->self_type;
          for (auto* other : def->methods) {
            if (other->name == fn->name) sink_.error(fn->span, "duplicate method '" + fn->name + "'");
          }
          def->methods.push_back(fn);
        }
        if (is_prelude) prog_.core_traits[t.name] = def.get();
        Symbol sym{SymbolKind::Trait};
        sym.trait_def = def.get();
        sym.span = t.span;
        declare_global(t.name, sym);
        pending_traits_.push_back(def.get());
        prog_.traits.push_back(std::move(def));
        break;
      }
      case ast::ItemKind::Impl: {
        auto& i = static_cast<const ast::ImplItem&>(*item);
        auto def = std::make_unique<ImplDef>();
        def->module = prog_.active_module;
        def->span = i.span;
        def->ast = &i;
        def->generics = declare_generics(i.generics, def.get());
        for (auto& m : i.methods) {
          FunctionDef* fn = new_function(*m, FnOwner::Impl);
          fn->owner_impl = def.get();
          def->methods.push_back(fn);
        }
        pending_impls_.push_back(def.get());
        prog_.impls.push_back(std::move(def));
        break;
      }
      case ast::ItemKind::Extern: {
        auto& x = static_cast<const ast::ExternItem&>(*item);
        for (auto& f : x.functions) {
          FunctionDef* def = new_function(*f, FnOwner::Extern);
          Symbol sym{SymbolKind::Function};
          sym.fn_def = def;
          sym.span = f->name_span;
          declare_global(def->name, sym);
        }
        break;
      }
    }
  }
}

const Type* Resolver::resolve_type(const ast::Type& t, const TypeScope& scope) {
  switch (t.kind) {
    case ast::TypeKind::Primitive:
      return ctx_.prim(static_cast<const ast::PrimitiveType&>(t).prim);
    case ast::TypeKind::SelfType: {
      const Type* self = scope.find_self();
      if (!self) {
        sink_.error(t.span, "Self is only valid inside a struct, enum, trait, or impl");
        return ctx_.error();
      }
      return self;
    }
    case ast::TypeKind::Ref: {
      auto& r = static_cast<const ast::RefType&>(t);
      return ctx_.ref(resolve_type(*r.inner, scope), r.is_const);
    }
    case ast::TypeKind::Ptr: {
      auto& p = static_cast<const ast::PtrType&>(t);
      return ctx_.ptr(resolve_type(*p.inner, scope), p.is_const);
    }
    case ast::TypeKind::Array: {
      auto& a = static_cast<const ast::ArrayType&>(t);
      const Type* elem = resolve_type(*a.elem, scope);
      if (a.size->kind != ast::ExprKind::IntLit) {
        sink_.error(a.size->span, "array length must be an integer literal");
        return ctx_.error();
      }
      return ctx_.array(elem, static_cast<const ast::IntLitExpr&>(*a.size).value);
    }
    case ast::TypeKind::Slice:
      return ctx_.slice(resolve_type(*static_cast<const ast::SliceType&>(t).elem, scope));
    case ast::TypeKind::Optional: {
      const Type* inner = resolve_type(*static_cast<const ast::OptionalType&>(t).inner, scope);
      if (!prog_.option_def) {
        sink_.error(t.span, "Option is not available");
        return ctx_.error();
      }
      return ctx_.enum_type(prog_.option_def, {inner});
    }
    case ast::TypeKind::Tuple: {
      std::vector<const Type*> elems;
      for (auto& e : static_cast<const ast::TupleType&>(t).elems) elems.push_back(resolve_type(*e, scope));
      return ctx_.tuple(std::move(elems));
    }
    case ast::TypeKind::Fn: {
      auto& f = static_cast<const ast::FnType&>(t);
      std::vector<const Type*> params;
      for (auto& p : f.params) params.push_back(resolve_type(*p, scope));
      const Type* ret = f.ret ? resolve_type(*f.ret, scope) : ctx_.void_type();
      return ctx_.fn(std::move(params), ret, f.call_mode, f.is_send);
    }
    case ast::TypeKind::Dyn: {
      TraitRef bound = resolve_bound(static_cast<const ast::DynType&>(t).bound, scope);
      if (!bound.trait || bound.args.size() != bound.trait->generics.size()) return ctx_.error();
      return ctx_.dyn_type(bound.trait, std::move(bound.args));
    }
    case ast::TypeKind::Path: {
      auto& p = static_cast<const ast::PathType&>(t);
      const std::string& name = p.path.segments.back().name;
      if (p.path.segments.size() == 1) {
        if (name == "str") {
          if (!p.args.empty()) sink_.error(t.span, "str takes no type arguments");
          return ctx_.str();
        }
        if (const GenericParamDef* g = scope.find(name)) {
          if (!p.args.empty()) sink_.error(t.span, "type parameter '" + name + "' takes no arguments");
          return g->type;
        }
      }
      const Symbol* sym = lookup_path(p.path);
      if (!sym) {
        sink_.error(p.path.span, "unknown type '" + name + "'", "SNX0301");
        return ctx_.error();
      }
      std::vector<const Type*> args;
      for (auto& a : p.args) args.push_back(resolve_type(*a, scope));
      size_t want = 0;
      if (sym->kind == SymbolKind::Struct) want = sym->struct_def->generics.size();
      else if (sym->kind == SymbolKind::Enum) want = sym->enum_def->generics.size();
      else {
        sink_.error(p.path.span, "'" + name + "' is not a type");
        return ctx_.error();
      }
      if (args.size() != want) {
        sink_.error(t.span, "'" + name + "' expects " + std::to_string(want) + " type argument" +
                                (want == 1 ? "" : "s") + ", got " + std::to_string(args.size()));
        return ctx_.error();
      }
      if (sym->kind == SymbolKind::Struct) return ctx_.struct_type(sym->struct_def, std::move(args));
      return ctx_.enum_type(sym->enum_def, std::move(args));
    }
  }
  return ctx_.error();
}

TraitRef Resolver::resolve_bound(const ast::Bound& b, const TypeScope& scope) {
  TraitRef out;
  const std::string& name = b.path.segments.back().name;
  const Symbol* sym = lookup_path(b.path);
  if (!sym || sym->kind != SymbolKind::Trait) {
    sink_.error(b.path.span, "unknown trait '" + name + "'");
    return out;
  }
  out.trait = sym->trait_def;
  for (auto& a : b.args) out.args.push_back(resolve_type(*a, scope));
  if (out.args.size() != out.trait->generics.size()) {
    sink_.error(b.span, "trait '" + name + "' expects " + std::to_string(out.trait->generics.size()) +
                            " type arguments, got " + std::to_string(out.args.size()));
  }
  return out;
}

void Resolver::resolve_function_signature(FunctionDef& fn, const TypeScope& outer) {
  const ast::Function& a = *fn.ast;
  fn.generics = declare_generics(a.generics, &fn);
  TypeScope scope{&fn.generics, fn.self_type, &outer};
  for (size_t i = 0; i < a.generics.size(); ++i) {
    for (auto& b : a.generics[i].bounds) fn.generics[i].bounds.push_back(resolve_bound(b, scope));
  }
  for (auto& p : a.params) {
    ParamDef d;
    d.name = p.name;
    d.span = p.span;
    d.self_kind = p.self_kind;
    if (p.self_kind != ast::SelfKind::None) {
      const Type* self = fn.self_type;
      if (!self) {
        sink_.error(p.span, "self parameter outside a type");
        self = ctx_.error();
      }
      switch (p.self_kind) {
        case ast::SelfKind::Value: d.type = self; break;
        case ast::SelfKind::Ref: d.type = ctx_.ref(self, false); break;
        case ast::SelfKind::ConstRef: d.type = ctx_.ref(self, true); break;
        default: break;
      }
    } else {
      d.type = resolve_type(*p.type, scope);
      if (d.type->is_unsized()) {
        sink_.error(p.span, "parameter of type '" + d.type->to_string() + "' must be a reference, write &const " + d.type->to_string());
      }
    }
    for (auto& prev : fn.params) {
      if (prev.name == d.name) sink_.error(d.span, "duplicate parameter '" + d.name + "'");
    }
    fn.params.push_back(std::move(d));
  }
  if (a.ret) {
    fn.ret = resolve_type(*a.ret, scope);
  } else if (fn.is_pub && fn.owner != FnOwner::Trait) {
    sink_.error(fn.span, "a pub function must write its return type");
    fn.ret = ctx_.error();
    fn.ret_declared = true;
  } else if (fn.owner == FnOwner::Trait || fn.owner == FnOwner::Extern) {
    if (!a.body) {
      sink_.error(fn.span, "a function without a body must write its return type");
      fn.ret = ctx_.error();
      fn.ret_declared = true;
    }
  }
}

void Resolver::resolve_signatures() {
  TypeScope root;

  for (StructDef* s : pending_structs_) {
    ModuleScope active(prog_, s->module);
    TypeScope scope{&s->generics, s->self_type, &root};
    for (size_t i = 0; i < s->ast->generics.size(); ++i) {
      for (auto& b : s->ast->generics[i].bounds) s->generics[i].bounds.push_back(resolve_bound(b, scope));
    }
    for (auto& f : s->ast->fields) {
      FieldDef d;
      d.name = f.name;
      d.span = f.span;
      d.is_pub = f.is_pub;
      d.type = resolve_type(*f.type, scope);
      if (d.type->is_unsized()) sink_.error(f.span, "field cannot have unsized type '" + d.type->to_string() + "'");
      for (auto& prev : s->fields) {
        if (prev.name == d.name) sink_.error(f.span, "duplicate field '" + d.name + "'");
      }
      s->fields.push_back(std::move(d));
    }
  }

  for (EnumDef* e : pending_enums_) {
    ModuleScope active(prog_, e->module);
    TypeScope scope{&e->generics, e->self_type, &root};
    for (size_t i = 0; i < e->ast->generics.size(); ++i) {
      for (auto& b : e->ast->generics[i].bounds) e->generics[i].bounds.push_back(resolve_bound(b, scope));
    }
    for (size_t i = 0; i < e->variants.size(); ++i) {
      const ast::Variant& av = e->ast->variants[i];
      VariantDef& v = e->variants[i];
      for (auto& t : av.tuple_types) v.tuple_types.push_back(resolve_type(*t, scope));
      for (auto& f : av.fields) {
        FieldDef d;
        d.name = f.name;
        d.span = f.span;
        d.type = resolve_type(*f.type, scope);
        v.fields.push_back(std::move(d));
      }
    }
  }

  for (TraitDef* t : pending_traits_) {
    ModuleScope active(prog_, t->module);
    TypeScope scope{&t->generics, t->self_type, &root};
    for (size_t i = 0; i < t->ast->generics.size(); ++i) {
      for (auto& b : t->ast->generics[i].bounds) t->generics[i].bounds.push_back(resolve_bound(b, scope));
    }
  }

  for (ImplDef* i : pending_impls_) {
    ModuleScope active(prog_, i->module);
    TypeScope gscope{&i->generics, nullptr, &root};
    for (size_t k = 0; k < i->ast->generics.size(); ++k) {
      for (auto& b : i->ast->generics[k].bounds) i->generics[k].bounds.push_back(resolve_bound(b, gscope));
    }
    i->self_type = resolve_type(*i->ast->target, gscope);
    TypeScope scope{&i->generics, i->self_type, &root};
    i->trait = resolve_bound(i->ast->trait, scope);
    for (auto* m : i->methods) m->self_type = i->self_type;
  }

  for (FunctionDef* f : pending_functions_) {
    ModuleScope active(prog_, f->module);
    TypeScope owner_scope;
    owner_scope.parent = &root;
    switch (f->owner) {
      case FnOwner::Struct: owner_scope.generics = &f->owner_struct->generics; owner_scope.self_type = f->self_type; break;
      case FnOwner::Enum: owner_scope.generics = &f->owner_enum->generics; owner_scope.self_type = f->self_type; break;
      case FnOwner::Trait: owner_scope.generics = &f->owner_trait->generics; owner_scope.self_type = f->self_type; break;
      case FnOwner::Impl: owner_scope.generics = &f->owner_impl->generics; owner_scope.self_type = f->self_type; break;
      default: break;
    }
    resolve_function_signature(*f, owner_scope);
  }

  // Impl methods must match the trait's method set.
  for (ImplDef* i : pending_impls_) {
    ModuleScope active(prog_, i->module);
    if (!i->trait.trait) continue;
    for (auto* tm : i->trait.trait->methods) {
      if (!i->find_method(tm->name) && !tm->ast->body) {
        sink_.error(i->span, "impl is missing method '" + tm->name + "' required by trait '" + i->trait.trait->name + "'");
      }
    }
    for (auto* m : i->methods) {
      if (!i->trait.trait->find_method(m->name)) {
        sink_.error(m->span, "method '" + m->name + "' is not a member of trait '" + i->trait.trait->name + "'");
      }
    }
  }

  for (ConstDef* c : pending_consts_) {
    ModuleScope active(prog_, c->module);
    if (c->ast->type) c->type = resolve_type(*c->ast->type, root);
  }
}

}  // namespace synx::sema
