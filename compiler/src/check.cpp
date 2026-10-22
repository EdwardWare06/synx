#include "synx/check.hpp"

#include <functional>

#include <algorithm>
#include <cctype>
#include <set>
#include <string>

namespace synx::sema {

using ast::ExprKind;
using ast::PrimKind;

Checker::Checker(Program& program, TypeContext& ctx, Resolver& resolver, DiagnosticSink& sink)
    : prog_(program), ctx_(ctx), resolver_(resolver), sink_(sink) {}

// ---------- builtins ----------

void Checker::register_builtins() {
  auto make = [&](const std::string& name, std::vector<const Type*> params, const Type* ret,
                  const Type* self_type) {
    auto def = std::make_unique<FunctionDef>();
    def->name = name;
    def->is_builtin = true;
    def->checked = true;
    def->ret = ret;
    def->ret_declared = true;
    def->self_type = self_type;
    if (self_type) {
      ParamDef self;
      self.name = "self";
      self.self_kind = ast::SelfKind::ConstRef;
      self.type = ctx_.ref(self_type, true);
      def->params.push_back(self);
    }
    int i = 0;
    for (auto* p : params) {
      ParamDef d;
      d.name = "arg" + std::to_string(i++);
      d.type = p;
      def->params.push_back(d);
    }
    FunctionDef* out = def.get();
    prog_.functions.push_back(std::move(def));
    if (!self_type) {
      Symbol sym{SymbolKind::Function};
      sym.fn_def = out;
      prog_.globals[name] = sym;
    }
    return out;
  };
  const Type* cstr = ctx_.ref(ctx_.str(), true);
  print_fn_ = make("print", {cstr}, ctx_.void_type(), nullptr);
  panic_fn_ = make("panic", {cstr}, ctx_.never(), nullptr);
  // Intrinsics used by the prelude to build String and Vec.
  const Type* cbyte_ptr = ctx_.ptr(ctx_.prim(PrimKind::Byte), true);
  make("str_from_raw", {cbyte_ptr, ctx_.usize_type()}, cstr, nullptr);
  FunctionDef* size_of = make("size_of", {}, ctx_.usize_type(), nullptr);
  GenericParamDef tp;
  tp.name = "T";
  tp.type = ctx_.param(size_of, 0, "T");
  size_of->generics.push_back(tp);
  FunctionDef* drop_in_place = make("drop_in_place", {}, ctx_.void_type(), nullptr);
  GenericParamDef dp;
  dp.name = "T"; dp.type = ctx_.param(drop_in_place, 0, "T");
  drop_in_place->generics.push_back(dp);
  ParamDef pointer;
  pointer.name = "value"; pointer.type = ctx_.ptr(dp.type, false);
  drop_in_place->params.push_back(pointer);
  // Methods on str. Option<int> for parse_int is filled in after the prelude.
  builtin_str_methods_.push_back(make("len", {}, ctx_.usize_type(), ctx_.str()));
  builtin_str_methods_.push_back(make("as_ptr", {}, cbyte_ptr, ctx_.str()));
  builtin_str_methods_.push_back(make("parse_int", {}, nullptr, ctx_.str()));
}

// ---------- inference ----------

const Type* Checker::shallow(const Type* t) const {
  while (t && t->kind == TypeKind::Infer && t->index < bindings_.size() && bindings_[t->index]) {
    t = bindings_[t->index];
  }
  return t;
}

const Type* Checker::zonk(const Type* t) {
  if (!t) return t;
  t = shallow(t);
  switch (t->kind) {
    case TypeKind::Ref: return ctx_.ref(zonk(t->inner), t->is_const);
    case TypeKind::Ptr: return ctx_.ptr(zonk(t->inner), t->is_const);
    case TypeKind::Array: return ctx_.array(zonk(t->inner), t->length);
    case TypeKind::Slice: return ctx_.slice(zonk(t->inner));
    case TypeKind::Tuple: {
      std::vector<const Type*> e;
      for (auto* x : t->elems) e.push_back(zonk(x));
      return ctx_.tuple(std::move(e));
    }
    case TypeKind::Fn: {
      std::vector<const Type*> e;
      for (auto* x : t->elems) e.push_back(zonk(x));
      return ctx_.fn(std::move(e), zonk(t->ret), t->call_mode, t->is_send);
    }
    case TypeKind::Struct: {
      std::vector<const Type*> e;
      for (auto* x : t->elems) e.push_back(zonk(x));
      return ctx_.struct_type(t->struct_def, std::move(e));
    }
    case TypeKind::Dyn: {
      std::vector<const Type*> e;
      for (auto* x : t->elems) e.push_back(zonk(x));
      return ctx_.dyn_type(t->trait_def, std::move(e));
    }
    case TypeKind::Enum: {
      std::vector<const Type*> e;
      for (auto* x : t->elems) e.push_back(zonk(x));
      return ctx_.enum_type(t->enum_def, std::move(e));
    }
    default:
      return t;
  }
}

std::string Checker::type_name(const Type* t) { return zonk(t)->to_string(); }

void Checker::rollback(size_t mark) {
  while (trail_.size() > mark) {
    bindings_[trail_.back()] = nullptr;
    trail_.pop_back();
  }
}

bool Checker::try_unify(const Type* a, const Type* b) {
  size_t mark = snapshot();
  if (unify(a, b)) return true;
  rollback(mark);
  return false;
}

bool Checker::unify(const Type* a, const Type* b) {
  a = shallow(a);
  b = shallow(b);
  if (a == b) return true;
  if (a->is_error() || b->is_error()) return true;
  if (a->is_never() || b->is_never()) return true;

  auto bind = [&](const Type* var, const Type* to) {
    if (var->index >= bindings_.size()) bindings_.resize(var->index + 1, nullptr);
    bindings_[var->index] = to;
    trail_.push_back(var->index);
    return true;
  };

  if (a->is_infer() && b->is_infer()) {
    // Keep the more specific kind.
    if (a->infer_kind == InferKind::Any) return bind(a, b);
    if (b->infer_kind == InferKind::Any) return bind(b, a);
    if (a->infer_kind != b->infer_kind) return false;
    return bind(a, b);
  }
  if (a->is_infer()) std::swap(a, b);
  if (b->is_infer()) {
    switch (b->infer_kind) {
      case InferKind::Any: return bind(b, a);
      case InferKind::Int: return a->is_integer() ? bind(b, a) : false;
      case InferKind::Float: return a->is_float() ? bind(b, a) : false;
    }
  }

  if (a->kind != b->kind) return false;
  switch (a->kind) {
    case TypeKind::Ref:
    case TypeKind::Ptr:
      return a->is_const == b->is_const && unify(a->inner, b->inner);
    case TypeKind::Array:
      return a->length == b->length && unify(a->inner, b->inner);
    case TypeKind::Slice:
      return unify(a->inner, b->inner);
    case TypeKind::Tuple:
    case TypeKind::Fn:
      if (a->kind == TypeKind::Fn && (a->call_mode != b->call_mode || a->is_send != b->is_send)) return false;
      [[fallthrough]];
    case TypeKind::Struct:
    case TypeKind::Dyn:
    case TypeKind::Enum: {
      if (a->kind == TypeKind::Dyn && a->trait_def != b->trait_def) return false;
      if (a->kind == TypeKind::Struct && a->struct_def != b->struct_def) return false;
      if (a->kind == TypeKind::Enum && a->enum_def != b->enum_def) return false;
      if (a->elems.size() != b->elems.size()) return false;
      for (size_t i = 0; i < a->elems.size(); ++i) {
        if (!unify(a->elems[i], b->elems[i])) return false;
      }
      if (a->kind == TypeKind::Fn) return unify(a->ret, b->ret);
      return true;
    }
    default:
      return false;  // distinct prims, params, etc.
  }
}

bool Checker::unify_or_error(const Type* a, const Type* b, Span span, const char* context) {
  if (unify(a, b)) return true;
  error(span, std::string("type mismatch in ") + context + ": expected '" + type_name(b) +
                  "', found '" + type_name(a) + "'", "SNX0402");
  return false;
}

// Tries to make `actual` fit `expected`, recording a coercion when one applies.
bool Checker::coerce(const ast::Expr& e, const Type* actual, const Type* expected, const char* context) {
  if (!expected) return true;
  const Type* a = shallow(actual);
  const Type* x = shallow(expected);
  if (a->is_error() || x->is_error()) return true;

  if (a->kind == TypeKind::Fn && x->kind == TypeKind::Fn && (a->call_mode != x->call_mode || a->is_send != x->is_send)) {
    auto ref = result_.refs.find(&e);
    bool named = ref != result_.refs.end() && (ref->second.kind == RefKind::Function || ref->second.kind == RefKind::AssocFn);
    bool compatible = (named || a->call_mode == x->call_mode || (a->call_mode == ast::CallMode::Shared && x->call_mode == ast::CallMode::Writable)) && (!x->is_send || a->is_send || named);
    auto* same_mode = ctx_.fn(a->elems, a->ret, x->call_mode, x->is_send);
    if (compatible && try_unify(same_mode, x)) {
      result_.coercions[&e] = {CoercionKind::FnMode, false, x};
      return true;
    }
  }
  if (x->kind == TypeKind::Ref) {
    const Type* want = shallow(x->inner);
    if (a->kind == TypeKind::Ref) {
      const Type* have = shallow(a->inner);
      if (!x->is_const && a->is_const) {
        error(e.span, std::string("type mismatch in ") + context + ": expected a writable reference '" +
                          type_name(x) + "', found read only '" + type_name(a) + "'");
        return false;
      }
      Coercion c;
      if (x->is_const && !a->is_const) c.kind = CoercionKind::MutToConst;
      if (try_unify(have, want)) {
        if (c.kind != CoercionKind::None) result_.coercions[&e] = c;
        return true;
      }
      if (want->kind == TypeKind::Dyn && !have->is_unsized()) {
        return coerce_dyn(e, have, x, false);
      }
      if (x->is_const) {
        if (have->kind == TypeKind::Struct && have->struct_def == prog_.string_def && want->kind == TypeKind::Str) {
          c.kind = CoercionKind::StringToStr;
          result_.coercions[&e] = c;
          return true;
        }
        if (have->kind == TypeKind::Array && want->kind == TypeKind::Slice && try_unify(have->inner, want->inner)) {
          c.kind = CoercionKind::ArrayToSlice;
          result_.coercions[&e] = c;
          return true;
        }
        if (have->kind == TypeKind::Struct && have->struct_def == prog_.vec_def && want->kind == TypeKind::Slice &&
            !have->elems.empty() && try_unify(have->elems[0], want->inner)) {
          c.kind = CoercionKind::VecToSlice;
          result_.coercions[&e] = c;
          return true;
        }
      }
      return unify_or_error(actual, expected, e.span, context);
    }
    if (x->is_const && !a->is_never()) {
      // Implicit shared borrow of a value.
      Coercion c;
      c.kind = CoercionKind::AutoBorrowConst;
      if (try_unify(a, want)) {
        result_.coercions[&e] = c;
        return true;
      }
      if (want->kind == TypeKind::Dyn && !a->is_unsized()) {
        return coerce_dyn(e, a, x, true);
      }
      if (a->kind == TypeKind::Struct && a->struct_def == prog_.string_def && want->kind == TypeKind::Str) {
        c.kind = CoercionKind::StringToStr;
        c.borrow_first = true;
        result_.coercions[&e] = c;
        return true;
      }
      if (a->kind == TypeKind::Array && want->kind == TypeKind::Slice && try_unify(a->inner, want->inner)) {
        c.kind = CoercionKind::ArrayToSlice;
        c.borrow_first = true;
        result_.coercions[&e] = c;
        return true;
      }
      if (a->kind == TypeKind::Struct && a->struct_def == prog_.vec_def && want->kind == TypeKind::Slice &&
          !a->elems.empty() && try_unify(a->elems[0], want->inner)) {
        c.kind = CoercionKind::VecToSlice;
        c.borrow_first = true;
        result_.coercions[&e] = c;
        return true;
      }
      return unify_or_error(actual, expected, e.span, context);
    }
    if (!x->is_const && !a->is_never()) {
      error(e.span, std::string("expected a writable reference '") + type_name(x) + "', found '" +
                        type_name(a) + "'. Write &" + " in front of the value");
      return false;
    }
  }
  return unify_or_error(actual, expected, e.span, context);
}

bool Checker::coerce_dyn(const ast::Expr& e, const Type* concrete, const Type* target, bool borrow_first) {
  const Type* dyn = target->inner;
  TraitRef trait{dyn->trait_def, dyn->elems};
  implements(concrete, trait);
  add_obligation(concrete, trait, e.span, "trait object conversion");
  result_.coercions[&e] = {CoercionKind::ToDyn, borrow_first, target};
  return true;
}

// ---------- helpers ----------

const Type* Checker::record(const ast::Expr& e, const Type* t) {
  result_.expr_types[&e] = t;
  exprs_in_function_.push_back(&e);
  return t;
}

const Type* Checker::option_of(const Type* t) {
  if (!prog_.option_def) return ctx_.error();
  return ctx_.enum_type(prog_.option_def, {t});
}

const Type* Checker::string_type() {
  if (!prog_.string_def) return ctx_.error();
  return ctx_.struct_type(prog_.string_def, {});
}

const Type* Checker::strip_refs(const Type* t, int* count) {
  t = shallow(t);
  int n = 0;
  while (t->kind == TypeKind::Ref) {
    t = shallow(t->inner);
    ++n;
  }
  if (count) *count = n;
  return t;
}

bool Checker::is_copy(const Type* t) {
  t = zonk(t);
  if (t->contains_infer()) {
    const Type* s = shallow(t);
    return s->is_infer() && s->infer_kind != InferKind::Any;
  }
  if (t->kind == TypeKind::Param) {
    TraitDef* copy = prog_.core_trait("Copy");
    const std::vector<TraitRef>* bounds = bounds_of(t);
    if (bounds) for (auto& b : *bounds) if (b.trait == copy) return true;
    return false;
  }
  return is_copy_type(prog_, ctx_, t);
}

// ---------- program ----------

void Checker::check_program() {
  for (const auto& impl : prog_.impls) {
    if (!impl->ast) continue;
    auto* trait = impl->trait.trait;
    bool marker = trait && (trait == prog_.core_trait("Send") || trait == prog_.core_trait("Sync") || trait == prog_.core_trait("Owned"));
    if (marker && !impl->ast->is_unsafe) error(impl->span, "Send, Sync, and Owned implementations require unsafe impl", "SNX0418");
    if (!marker && impl->ast->is_unsafe) error(impl->span, "unsafe impl requires Send, Sync, or Owned", "SNX0418");
  }
  // An explicit Copy impl must satisfy the structural rule: no Drop and
  // every field Copy. A type parameter with a Copy bound counts as Copy.
  TraitDef* copy_trait = prog_.core_trait("Copy");
  for (const auto& impl : prog_.impls) {
    if (!impl->ast || !copy_trait || impl->trait.trait != copy_trait) continue;
    Subst subst;
    for (auto& g : impl->generics)
      for (auto& b : g.bounds) if (b.trait == copy_trait) subst.bind(g.type, ctx_.int_type());
    if (!is_copy_type(prog_, ctx_, subst.apply(ctx_, impl->self_type)))
      error(impl->span, "'" + type_name(impl->self_type) + "' cannot implement Copy: it implements Drop or has a field that is not Copy", "SNX0421");
  }
  for (const auto& fn : prog_.functions) {
    if (!fn->ast || !fn->ast->is_async) continue;
    if (fn->owner == FnOwner::Trait || fn->owner == FnOwner::Impl || fn->owner == FnOwner::Extern)
      error(fn->span, "async trait and extern methods are not supported; return a Task<T> explicitly", "SNX0417");
    if (fn->owner == FnOwner::Module && fn->module == prog_.root_module && fn->name == "main")
      error(fn->span, "main must be synchronous; use std.task.block_on", "SNX0417");
  }
  // Fill in builtin return types that need prelude types.
  for (auto* m : builtin_str_methods_) {
    if (m->name == "parse_int") m->ret = option_of(ctx_.int_type());
  }
  // Layout follows inline fields only. A pointer or reference ends a path;
  // its type arguments do not occupy inline storage. Generic substitution
  // catches indirect cycles such as A -> Wrapper<A> -> A as well.
  std::set<const Type*> layouts_checked, active_layouts;
  std::function<bool(const Type*, Span, unsigned)> finite_layout = [&](const Type* t, Span span, unsigned depth) {
    if (!t || layouts_checked.count(t)) return true;
    if (active_layouts.count(t) || depth == 128) {
      error(span, "type has an infinite or excessively nested inline layout; use Box<T>, Vec<T>, or a reference for recursion", "SNX0414");
      return false;
    }
    if (t->kind != TypeKind::Struct && t->kind != TypeKind::Enum && t->kind != TypeKind::Array && t->kind != TypeKind::Tuple) return true;
    active_layouts.insert(t);
    bool valid = true;
    auto visit = [&](const Type* field) { if (valid) valid = finite_layout(field, span, depth + 1); };
    if (t->kind == TypeKind::Array) visit(t->inner);
    else if (t->kind == TypeKind::Tuple) { for (auto* field : t->elems) visit(field); }
    else {
      Subst subst;
      const auto& generics = t->kind == TypeKind::Struct ? t->struct_def->generics : t->enum_def->generics;
      for (size_t i = 0; i < generics.size() && i < t->elems.size(); ++i) subst.bind(generics[i].type, t->elems[i]);
      if (t->kind == TypeKind::Struct) {
        for (auto& field : t->struct_def->fields) visit(subst.apply(ctx_, field.type));
      } else {
        for (auto& variant : t->enum_def->variants) {
          for (auto* field : variant.tuple_types) visit(subst.apply(ctx_, field));
          for (auto& field : variant.fields) visit(subst.apply(ctx_, field.type));
        }
      }
    }
    active_layouts.erase(t);
    if (valid) layouts_checked.insert(t);
    return valid;
  };
  for (auto& def : prog_.structs) if (!finite_layout(def->self_type, def->span, 0)) return;
  for (auto& def : prog_.enums) if (!finite_layout(def->self_type, def->span, 0)) return;
  for (auto& c : prog_.consts) check_const(*c);
  // check_function may be re-entered through ensure_signature, which never
  // adds functions, so indexing stays valid.
  for (size_t i = 0; i < prog_.functions.size(); ++i) {
    if (!prog_.functions[i]->is_builtin) check_function(*prog_.functions[i]);
  }
  // Erased shared wake callbacks and future dispatch rely on exact receivers.
  for (const auto& impl : prog_.impls) {
    auto* trait = impl->trait.trait;
    if (trait != prog_.core_trait("Wake") && trait != prog_.core_trait("Future")) continue;
    Subst subst; subst.bind(trait->self_type, impl->self_type);
    for (size_t i = 0; i < trait->generics.size() && i < impl->trait.args.size(); ++i) subst.bind(trait->generics[i].type, impl->trait.args[i]);
    for (auto* required : trait->methods) {
      auto* actual = impl->find_method(required->name);
      if (!actual) continue;
      bool valid = actual->generics.empty() && (!actual->ast || !actual->ast->is_async) && actual->params.size() == required->params.size();
      if (actual->ret != subst.apply(ctx_, required->ret)) valid = false;
      for (size_t p = 0; p < actual->params.size() && p < required->params.size(); ++p)
        if (actual->params[p].type != subst.apply(ctx_, required->params[p].type) || actual->params[p].self_kind != required->params[p].self_kind) valid = false;
      if (!valid) error(actual->span, "implementation of " + trait->name + "." + required->name + " does not match its trait signature", "SNX0419");
    }
  }
  // Inference has fixed literal types. A negative literal may use the
  // extra magnitude of the signed minimum. Explicit literal casts retain
  // their truncation behavior instead of first restricting the source.
  std::set<const ast::Expr*> negative_literals, cast_literals;
  for (auto& [expr, type] : result_.expr_types) {
    (void)type;
    if (expr->kind == ExprKind::Unary) {
      auto& unary = static_cast<const ast::UnaryExpr&>(*expr);
      if (unary.op == ast::UnaryOp::Neg && unary.operand->kind == ExprKind::IntLit) negative_literals.insert(unary.operand.get());
    } else if (expr->kind == ExprKind::Cast) {
      auto& cast = static_cast<const ast::CastExpr&>(*expr);
      if (cast.operand->kind == ExprKind::IntLit) cast_literals.insert(cast.operand.get());
    }
  }
  for (auto& [expr, raw_type] : result_.expr_types) {
    if (expr->kind != ExprKind::IntLit || cast_literals.count(expr)) continue;
    const Type* type = zonk(raw_type);
    if (type->kind != TypeKind::Prim || !type->is_integer()) continue;
    unsigned bits = type->is_prim(PrimKind::Byte) ? 8 : type->is_prim(PrimKind::Short) ? 16 :
                    (type->is_prim(PrimKind::Int) || type->is_prim(PrimKind::Uint)) ? 32 : 64;
    uint64_t limit = bits == 64 ? UINT64_MAX : (uint64_t(1) << bits) - 1;
    if (type->is_signed_integer()) limit = (uint64_t(1) << (bits - 1)) - (negative_literals.count(expr) ? 0 : 1);
    if (static_cast<const ast::IntLitExpr&>(*expr).value > limit)
      error(expr->span, "integer literal is out of range for '" + type->to_string() + "'", "SNX0413");
  }
  // Validate resolved nominal type arguments even when no method uses the
  // parameter (for example HashMap<double, int>.new()). Keep every owner's
  // declared bounds available while checking generic signatures and fields.
  auto register_bounds = [&](const std::vector<GenericParamDef>& generics) {
    for (auto& g : generics) param_bounds_[g.type] = &g.bounds;
  };
  for (auto& s : prog_.structs) register_bounds(s->generics);
  for (auto& e : prog_.enums) register_bounds(e->generics);
  for (auto& i : prog_.impls) register_bounds(i->generics);
  for (auto& f : prog_.functions) register_bounds(f->generics);
  for (auto& t : prog_.traits) {
    register_bounds(t->generics);
    auto& bounds = self_bounds_[t.get()];
    if (bounds.empty()) {
      TraitRef self{t.get(), {}};
      for (auto& g : t->generics) self.args.push_back(g.type);
      bounds.push_back(std::move(self));
    }
    param_bounds_[t->self_type] = &bounds;
  }
  // Check type bounds, sized arguments, and object safety after every
  // signature and body has been resolved.
  std::set<const Type*> seen;
  std::function<void(const Type*, Span)> validate = [&](const Type* t, Span span) {
    if (!t || !seen.insert(t).second) return;
    auto require_sized = [&](const Type* x) {
      if (x && x->is_unsized()) error(span, "type '" + x->to_string() + "' is unsized here; use a reference", "SNX0411");
    };
    if ((t->kind == TypeKind::Struct || t->kind == TypeKind::Enum) && !t->contains_infer()) {
      const auto& generics = t->kind == TypeKind::Struct ? t->struct_def->generics : t->enum_def->generics;
      Subst subst;
      for (size_t i = 0; i < generics.size() && i < t->elems.size(); ++i) subst.bind(generics[i].type, t->elems[i]);
      for (size_t i = 0; i < generics.size() && i < t->elems.size(); ++i) {
        for (const auto& bound : generics[i].bounds) {
          TraitRef concrete = bound;
          for (auto*& arg : concrete.args) arg = subst.apply(ctx_, arg);
          if (concrete.trait && !implements(t->elems[i], concrete)) {
            error(span, "the type '" + t->elems[i]->to_string() + "' does not implement '" + concrete.trait->name +
                        "', required for type argument '" + generics[i].name + "' of '" + t->to_string() + "'");
          }
        }
      }
    }
    if (t->kind == TypeKind::Struct || t->kind == TypeKind::Enum || t->kind == TypeKind::Dyn ||
        t->kind == TypeKind::Tuple || t->kind == TypeKind::Fn) {
      for (auto* el : t->elems) require_sized(el);
    }
    if (t->kind == TypeKind::Array || t->kind == TypeKind::Slice) require_sized(t->inner);
    if (t->kind == TypeKind::Fn) require_sized(t->ret);
    if (t->kind == TypeKind::Dyn) {
      auto* trait = t->trait_def;
      std::function<bool(const Type*)> has_self = [&](const Type* x) {
        if (!x) return false;
        if (x == trait->self_type) return true;
        if (has_self(x->inner) || has_self(x->ret)) return true;
        for (auto* el : x->elems) if (has_self(el)) return true;
        return false;
      };
      for (auto* m : trait->methods) {
        bool safe = m->has_self() && m->generics.empty() &&
                    m->params[0].self_kind != ast::SelfKind::Value && !has_self(m->ret);
        for (size_t i = 1; i < m->params.size(); ++i) safe = safe && !has_self(m->params[i].type);
        if (!safe) error(span, "trait '" + trait->name + "' cannot be used as dyn: method '" + m->name +
                        "' must have a reference receiver, no generics, and no Self outside the receiver", "SNX0410");
      }
    }
    validate(t->inner, span);
    validate(t->ret, span);
    for (auto* el : t->elems) validate(el, span);
  };
  for (auto& impl : prog_.impls) {
    validate(impl->self_type, impl->span);
    for (auto* arg : impl->trait.args) validate(arg, impl->span);
  }
  for (auto& s : prog_.structs) for (auto& field : s->fields) validate(field.type, field.span);
  for (auto& e : prog_.enums) for (auto& variant : e->variants) {
    for (auto* type : variant.tuple_types) {
      if (type->is_unsized()) error(variant.span, "enum payload is unsized; use a reference", "SNX0411");
      validate(type, variant.span);
    }
    for (auto& field : variant.fields) validate(field.type, field.span);
  }
  for (auto& c : prog_.consts) validate(c->type, c->span);
  for (auto& [expr, ref] : result_.refs) {
    if (ref.struct_def && ref.owner_args.size() == ref.struct_def->generics.size())
      validate(ctx_.struct_type(ref.struct_def, ref.owner_args), expr->span);
    if (ref.enum_def && ref.owner_args.size() == ref.enum_def->generics.size())
      validate(ctx_.enum_type(ref.enum_def, ref.owner_args), expr->span);
    for (auto* type : ref.type_args) {
      if (type->is_unsized()) error(expr->span, "type argument '" + type->to_string() + "' is unsized; use a reference", "SNX0411");
      validate(type, expr->span);
    }
    for (auto* type : ref.owner_args) {
      if (type->is_unsized()) error(expr->span, "type argument '" + type->to_string() + "' is unsized; use a reference", "SNX0411");
      validate(type, expr->span);
    }
  }
  for (auto& f : prog_.functions) {
    if (f->ret && f->ret->is_unsized()) error(f->span, "return type '" + f->ret->to_string() + "' is unsized; use a reference", "SNX0411");
    validate(f->ret, f->span);
    for (auto& p : f->params) validate(p.type, p.span);
  }
  for (auto& [expr, type] : result_.expr_types) validate(type, expr->span);
  for (auto& [fn, locals] : result_.fn_locals) {
    (void)fn;
    for (auto& l : locals) validate(l.type, l.span);
  }
  for (auto& [constant, locals] : result_.const_locals) {
    (void)constant;
    for (auto& l : locals) validate(l.type, l.span);
  }
  for (auto& [closure, locals] : result_.closure_locals) {
    (void)closure;
    for (auto& l : locals) validate(l.type, l.span);
  }
  for (auto& [expr, co] : result_.coercions) validate(co.target, expr->span);
}

void Checker::check_const(ConstDef& c) {
  if (c.checked) return;
  if (c.checking) { error(c.span, "cyclic constant '" + c.name + "'", "SNX0405"); c.type = ctx_.error(); return; }
  c.checking = true;
  auto saved_exprs = std::move(exprs_in_function_);
  auto saved_sites = std::move(infer_sites_);
  auto saved_obligations = std::move(obligations_);
  auto saved_bounds = param_bounds_;
  exprs_in_function_.clear(); infer_sites_.clear(); obligations_.clear(); param_bounds_.clear();
  ModuleScope active(prog_, c.module);
  FnCtx fc;
  fc.in_const_initializer = true;
  std::vector<LocalVar> locals;
  fc.locals = &locals;
  fc.ret = ctx_.void_type();
  push_scope(fc);
  const Type* t = check_expr(fc, *c.ast->value, c.type);
  if (c.type) {
    coerce(*c.ast->value, t, c.type, "constant");
  } else {
    c.type = t;
  }
  pop_scope(fc);
  finish_function_types();
  c.type = zonk(c.type);
  for (auto& local : locals) local.type = zonk(local.type);
  result_.const_locals[&c] = std::move(locals);
  exprs_in_function_ = std::move(saved_exprs);
  infer_sites_ = std::move(saved_sites);
  obligations_ = std::move(saved_obligations);
  param_bounds_ = std::move(saved_bounds);
  c.checking = false;
  c.checked = true;
}

TypeScope Checker::owner_scope(const FunctionDef& fn, TypeScope& storage) {
  storage = TypeScope{};
  switch (fn.owner) {
    case FnOwner::Struct: storage.generics = &fn.owner_struct->generics; break;
    case FnOwner::Enum: storage.generics = &fn.owner_enum->generics; break;
    case FnOwner::Trait: storage.generics = &fn.owner_trait->generics; break;
    case FnOwner::Impl: storage.generics = &fn.owner_impl->generics; break;
    default: break;
  }
  storage.self_type = fn.self_type;
  TypeScope scope{&fn.generics, fn.self_type, &storage};
  return scope;
}

void Checker::ensure_signature(FunctionDef& fn, Span use_span) {
  if (fn.ret) return;
  if (fn.checking) {
    error(use_span, "function '" + fn.name + "' calls itself, so it must write its return type", "SNX0405");
    sink_.note(fn.span, "add the return type here");
    fn.ret = ctx_.error();
    return;
  }
  check_function(fn);
}

void Checker::check_function(FunctionDef& fn) {
  if (fn.checked || fn.checking) return;
  if (!fn.ast || !fn.ast->body) {
    fn.checked = true;
    if (!fn.ret) fn.ret = ctx_.error();
    return;
  }
  ModuleScope active(prog_, fn.module);
  fn.checking = true;

  // Save the state of any outer function being checked (nested through
  // ensure_signature) so the two do not mix.
  auto saved_exprs = std::move(exprs_in_function_);
  auto saved_sites = std::move(infer_sites_);
  auto saved_obligations = std::move(obligations_);
  auto saved_bounds = param_bounds_;
  exprs_in_function_.clear();
  infer_sites_.clear();
  obligations_.clear();

  FnCtx fc;
  fc.fn = &fn;
  TypeScope storage;
  fc.type_scope = owner_scope(fn, storage);
  std::vector<LocalVar> locals;
  fc.locals = &locals;
  fc.ret_inferred = !fn.ret;
  fc.ret = fn.ret ? fn.ret : ctx_.fresh_infer(InferKind::Any);

  for (const TypeScope* s = &fc.type_scope; s; s = s->parent) {
    if (s->generics) {
      for (auto& g : *s->generics) param_bounds_[g.type] = &g.bounds;
    }
  }
  if (fn.owner == FnOwner::Trait && fn.owner_trait) {
    // Inside a trait default body, Self implements the trait.
    auto& v = self_bounds_[fn.owner_trait];
    if (v.empty()) {
      TraitRef r;
      r.trait = fn.owner_trait;
      for (auto& g : fn.owner_trait->generics) r.args.push_back(g.type);
      v.push_back(r);
    }
    param_bounds_[fn.owner_trait->self_type] = &v;
  }

  push_scope(fc);
  for (auto& p : fn.params) declare_local(fc, p.name, p.type, false, p.span);

  const Type* body_type = check_block(fc, *fn.ast->body, fc.ret);
  const ast::Block& body = *fn.ast->body;
  // A void tail such as an if without else is not a return value; the
  // function falls off the end after it.
  bool tail_has_value = body.tail && !shallow(body_type)->is_void() && !shallow(body_type)->is_never();
  if (tail_has_value) {
    coerce(*body.tail, body_type, fc.ret, "return value");
  } else if (!shallow(body_type)->is_never()) {
    // Falls off the end: the function returns void.
    if (fc.ret_inferred) {
      if (!fc.saw_return) unify(fc.ret, ctx_.void_type());
      else if (!try_unify(fc.ret, ctx_.void_type())) {
        error(Span{body.span.end - 1, body.span.end, body.span.file},
              "function returns '" + type_name(fc.ret) + "' on some paths but falls off the end without a value");
      }
    } else if (!shallow(fc.ret)->is_void() && !shallow(fc.ret)->is_error()) {
      error(Span{body.span.end - 1, body.span.end, body.span.file},
            "missing return value: function returns '" + type_name(fc.ret) + "'");
    }
  } else if (fc.ret_inferred && !fc.saw_return) {
    // The body always diverges and never returns a value: infer void.
    unify(fc.ret, ctx_.void_type());
  }
  pop_scope(fc);

  if (fn.ast->async_send) {
    for (const char* marker : {"Send", "Owned"}) {
      for (const auto& p : fn.params) add_obligation(p.type, TraitRef{prog_.core_trait(marker), {}}, p.span, "send async input " + p.name);
      add_obligation(fc.ret, TraitRef{prog_.core_trait(marker), {}}, fn.span, "send async output");
    }
  }
  finish_function_types();
  if (fc.ret_inferred) {
    fn.ret = zonk(fc.ret);
    if (fn.ret->contains_infer()) {
      error(fn.span, "cannot infer the return type of '" + fn.name + "'; write it");
      fn.ret = ctx_.error();
    }
  }
  for (auto& l : locals) l.type = zonk(l.type);
  result_.fn_locals[&fn] = std::move(locals);

  exprs_in_function_ = std::move(saved_exprs);
  infer_sites_ = std::move(saved_sites);
  obligations_ = std::move(saved_obligations);
  param_bounds_ = std::move(saved_bounds);
  fn.checking = false;
  fn.checked = true;
}

// Resolves every recorded type of the current function, applies literal
// defaults, reports types that are still unknown, and checks trait bounds.
void Checker::finish_function_types() {
  // Default integer and float literals. Two passes so a variable bound in
  // the first pass can settle types that contain it.
  for (size_t pass = 0; pass < 2; ++pass) {
    for (auto* e : exprs_in_function_) {
      const Type* t = zonk(result_.expr_types[e]);
      // Walk the type and bind literal variables.
      std::vector<const Type*> stack{t};
      while (!stack.empty()) {
        const Type* x = shallow(stack.back());
        stack.pop_back();
        if (x->is_infer()) {
          if (x->infer_kind == InferKind::Int) unify(x, ctx_.int_type());
          else if (x->infer_kind == InferKind::Float) unify(x, ctx_.double_type());
          continue;
        }
        if (x->inner) stack.push_back(x->inner);
        if (x->ret) stack.push_back(x->ret);
        for (auto* el : x->elems) stack.push_back(el);
      }
    }
  }
  std::set<const Type*> reported;
  for (auto* e : exprs_in_function_) {
    const Type* t = zonk(result_.expr_types[e]);
    result_.expr_types[e] = t;
    if (t->contains_infer()) {
      // Report the outermost expression once per variable.
      std::vector<const Type*> stack{t};
      while (!stack.empty()) {
        const Type* x = shallow(stack.back());
        stack.pop_back();
        if (x->is_infer()) {
          if (!reported.count(x)) {
            reported.insert(x);
            error(e->span, "cannot infer the type of this expression; add a type annotation");
          }
          continue;
        }
        if (x->inner) stack.push_back(x->inner);
        if (x->ret) stack.push_back(x->ret);
        for (auto* el : x->elems) stack.push_back(el);
      }
    }
  }
  for (auto& [e, ref] : result_.refs) {
    for (auto*& t : ref.type_args) t = zonk(t);
    for (auto*& t : ref.owner_args) t = zonk(t);
  }
  for (auto& [expr, co] : result_.coercions) {
    (void)expr;
    if (co.target) co.target = zonk(co.target);
  }
  for (auto& [closure, locals] : result_.closure_locals) {
    (void)closure;
    for (auto& local : locals) local.type = zonk(local.type);
  }
  check_obligations();
  exprs_in_function_.clear();
}

// ---------- scopes ----------

void Checker::push_scope(FnCtx& fc) { fc.scopes.emplace_back(); }
void Checker::pop_scope(FnCtx& fc) { fc.scopes.pop_back(); }

uint32_t Checker::declare_local(FnCtx& fc, const std::string& name, const Type* type, bool is_const, Span span) {
  LocalVar v;
  v.name = name;
  v.type = type;
  v.is_const = is_const;
  v.span = span;
  uint32_t id = static_cast<uint32_t>(fc.locals->size());
  fc.locals->push_back(v);
  if (!fc.scopes.empty()) fc.scopes.back().names.emplace_back(name, id);
  return id;
}

bool Checker::lookup_local(FnCtx& fc, const std::string& name, uint32_t& id_out, const LocalVar** var_out,
                           bool record_capture) {
  for (FnCtx* c = &fc; c; c = c->parent) {
    for (auto it = c->scopes.rbegin(); it != c->scopes.rend(); ++it) {
      for (auto n = it->names.rbegin(); n != it->names.rend(); ++n) {
        if (n->first == name) {
          id_out = n->second;
          *var_out = &(*c->locals)[n->second];
          for (FnCtx* capture = &fc; capture; capture = capture->parent) {
            if (!record_capture || !capture->closure || id_out >= capture->local_start) continue;
            auto& ids = result_.closure_captures[capture->closure];
            if (std::find(ids.begin(), ids.end(), id_out) == ids.end()) {
              if (capture->closure->explicit_captures && !result_.closure_capture_modes[capture->closure].count(id_out))
                error(capture->closure->span, "captured variable '" + name + "' is not in the capture list", "SNX0408");
              ids.push_back(id_out);
            }
          }
          return true;
        }
      }
    }
  }
  return false;
}

// ---------- statements ----------

const Type* Checker::check_block(FnCtx& fc, const ast::Block& block, const Type* expected) {
  push_scope(fc);
  bool diverged = false;
  for (auto& s : block.stmts) check_stmt(fc, *s, diverged);
  const Type* t;
  if (block.tail) {
    t = check_expr(fc, *block.tail, expected);
  } else {
    t = diverged ? ctx_.never() : ctx_.void_type();
  }
  pop_scope(fc);
  return t;
}

void Checker::check_stmt(FnCtx& fc, const ast::Stmt& stmt, bool& diverged) {
  switch (stmt.kind) {
    case ast::StmtKind::VarDecl:
      check_var_decl(fc, static_cast<const ast::VarDeclStmt&>(stmt));
      break;
    case ast::StmtKind::Expr: {
      auto& s = static_cast<const ast::ExprStmt&>(stmt);
      const Type* t = check_expr(fc, *s.expr, s.has_semi ? nullptr : ctx_.void_type());
      if (shallow(t)->is_never()) diverged = true;
      if (!s.has_semi && !shallow(t)->is_void() && !shallow(t)->is_never() && !shallow(t)->is_error()) {
        if (!try_unify(t, ctx_.void_type())) {
          error(s.expr->span, "this value of type '" + type_name(t) + "' is not used; add ';' or use it");
        }
      }
      break;
    }
    case ast::StmtKind::Assign:
      check_assign(fc, static_cast<const ast::AssignStmt&>(stmt));
      break;
    case ast::StmtKind::While: {
      auto& s = static_cast<const ast::WhileStmt&>(stmt);
      const Type* c = check_expr(fc, *s.cond, ctx_.bool_type());
      unify_or_error(c, ctx_.bool_type(), s.cond->span, "while condition");
      ++fc.loop_depth;
      fc.loop_breaks.push_back(false);
      const Type* b = check_block(fc, *s.body, ctx_.void_type());
      fc.loop_breaks.pop_back();
      --fc.loop_depth;
      if (s.body->tail) coerce(*s.body->tail, b, ctx_.void_type(), "loop body");
      break;
    }
    case ast::StmtKind::Loop: {
      auto& s = static_cast<const ast::LoopStmt&>(stmt);
      ++fc.loop_depth;
      fc.loop_breaks.push_back(false);
      const Type* b = check_block(fc, *s.body, ctx_.void_type());
      bool broke = fc.loop_breaks.back();
      fc.loop_breaks.pop_back();
      --fc.loop_depth;
      if (s.body->tail) coerce(*s.body->tail, b, ctx_.void_type(), "loop body");
      // A loop that no break targets never finishes.
      if (!broke) diverged = true;
      break;
    }
    case ast::StmtKind::For: {
      auto& s = static_cast<const ast::ForStmt&>(stmt);
      const Type* it = check_expr(fc, *s.iter, nullptr);
      const Type* t = shallow(it);
      const Type* elem = nullptr;
      int refs = 0;
      const Type* base = strip_refs(t, &refs);
      bool is_const_ref = refs > 0 && t->is_const;
      if (base->kind == TypeKind::Struct && (base->struct_def == prog_.range_def ||
                                             base->struct_def == prog_.range_inclusive_def)) {
        elem = base->elems[0];
        if (!shallow(elem)->is_integer()) {
          error(s.iter->span, "for over a range needs integer bounds, found '" + type_name(elem) + "'");
        }
      } else if (base->kind == TypeKind::Array || base->kind == TypeKind::Slice) {
        elem = refs > 0 ? ctx_.ref(base->inner, is_const_ref) : base->inner;
      } else if (base->kind == TypeKind::Struct && base->struct_def == prog_.vec_def && !base->elems.empty()) {
        elem = refs > 0 ? ctx_.ref(base->elems[0], is_const_ref) : base->elems[0];
      } else if (!base->is_error()) {
        error(s.iter->span, "cannot iterate over a value of type '" + type_name(it) + "'");
        elem = ctx_.error();
      } else {
        elem = ctx_.error();
      }
      push_scope(fc);
      check_pattern(fc, *s.pattern, elem, 0);
      ++fc.loop_depth;
      fc.loop_breaks.push_back(false);
      const Type* b = check_block(fc, *s.body, ctx_.void_type());
      fc.loop_breaks.pop_back();
      --fc.loop_depth;
      if (s.body->tail) coerce(*s.body->tail, b, ctx_.void_type(), "loop body");
      pop_scope(fc);
      break;
    }
  }
}

void Checker::check_var_decl(FnCtx& fc, const ast::VarDeclStmt& s) {
  const Type* declared = s.type ? resolver_.resolve_type(*s.type, fc.type_scope) : nullptr;
  const Type* type = declared;
  if (s.init) {
    const Type* init = check_expr(fc, *s.init, declared);
    if (declared) {
      coerce(*s.init, init, declared, "variable initializer");
    } else {
      type = init;
      if (shallow(init)->is_never()) type = ctx_.fresh_infer(InferKind::Any);
      if (shallow(init)->is_void()) {
        error(s.init->span, "cannot store a void value in a variable");
        type = ctx_.error();
      }
    }
  }
  if (!type) type = ctx_.error();
  if (shallow(type)->is_unsized()) {
    error(s.span, "a variable cannot have the unsized type '" + type_name(type) + "'; use a reference");
    type = ctx_.error();
  }
  uint32_t id = declare_local(fc, s.name, type, s.is_const, s.name_span);
  result_.decl_locals[&s] = id;
}

void Checker::check_assign(FnCtx& fc, const ast::AssignStmt& s) {
  const Type* target = check_expr(fc, *s.target, nullptr);
  if (shallow(target)->is_error()) {
    check_expr(fc, *s.value, nullptr);  // already reported; avoid follow-on errors
    return;
  }
  if (!is_place(fc, *s.target)) {
    error(s.target->span, "cannot assign to this expression");
  } else if (!place_is_mutable(fc, *s.target)) {
    error(s.target->span, "cannot assign: this value is read only", "SNX0403");
  }
  const Type* value = check_expr(fc, *s.value, target);
  if (s.op == ast::AssignOp::Assign) {
    coerce(*s.value, value, target, "assignment");
    return;
  }
  // Compound assignment: both sides must be the same numeric type.
  unify_or_error(value, target, s.value->span, "assignment");
  const Type* t = shallow(zonk(target));
  bool ok = t->is_numeric() || t->is_error();
  if (s.op == ast::AssignOp::BitAnd || s.op == ast::AssignOp::BitOr || s.op == ast::AssignOp::BitXor ||
      s.op == ast::AssignOp::Shl || s.op == ast::AssignOp::Shr) {
    ok = t->is_integer() || t->is_error();
  }
  if (!ok) {
    error(s.span, std::string("operator '") + ast::assign_op_name(s.op) + "' is not defined for '" + type_name(target) + "'");
  }
}

// ---------- places ----------

bool Checker::is_place(FnCtx& fc, const ast::Expr& e) {
  switch (e.kind) {
    case ExprKind::Name: {
      auto it = result_.refs.find(&e);
      return it != result_.refs.end() && it->second.kind == RefKind::Local;
    }
    case ExprKind::Field: {
      auto it = result_.refs.find(&e);
      if (it == result_.refs.end()) return false;
      if (it->second.kind != RefKind::Field && it->second.kind != RefKind::TupleField) return false;
      auto& f = static_cast<const ast::FieldExpr&>(e);
      if (it->second.auto_derefs > 0) return true;  // through a reference
      return is_place(fc, *f.base);
    }
    case ExprKind::Index: return true;
    case ExprKind::Unary: return static_cast<const ast::UnaryExpr&>(e).op == ast::UnaryOp::Deref;
    default: return false;
  }
}

bool Checker::place_is_mutable(FnCtx& fc, const ast::Expr& e) {
  switch (e.kind) {
    case ExprKind::Name: {
      auto it = result_.refs.find(&e);
      if (it == result_.refs.end() || it->second.kind != RefKind::Local) return false;
      // Closures share the locals vector of the function they are in.
      if (it->second.local_id >= fc.locals->size()) return true;
      uint32_t id = it->second.local_id;
      if (fc.closure && id < fc.local_start) {
        auto& modes = result_.closure_capture_modes[fc.closure];
        auto mode = modes.find(id);
        if (fc.closure->call_mode == ast::CallMode::Shared || (mode != modes.end() && mode->second == ast::CaptureMode::Shared)) return false;
      }
      return !(*fc.locals)[id].is_const;
    }
    case ExprKind::Field: {
      auto& f = static_cast<const ast::FieldExpr&>(e);
      auto it = result_.refs.find(&e);
      if (it != result_.refs.end() && it->second.auto_derefs > 0) {
        // Mutability comes from the outermost reference.
        const Type* bt = shallow(result_.type_of(*f.base));
        return bt->kind == TypeKind::Ref ? !bt->is_const : true;
      }
      return place_is_mutable(fc, *f.base);
    }
    case ExprKind::Index: {
      auto& i = static_cast<const ast::IndexExpr&>(e);
      const Type* bt = shallow(result_.type_of(*i.base));
      if (bt->kind == TypeKind::Ref) return !bt->is_const;
      return place_is_mutable(fc, *i.base);
    }
    case ExprKind::Unary: {
      auto& u = static_cast<const ast::UnaryExpr&>(e);
      const Type* ot = shallow(result_.type_of(*u.operand));
      return (ot->kind == TypeKind::Ref || ot->kind == TypeKind::Ptr) ? !ot->is_const : false;
    }
    default: return false;
  }
}

// ---------- expressions ----------

const Type* Checker::check_expr(FnCtx& fc, const ast::Expr& e, const Type* expected) {
  const Type* t = check_expr_inner(fc, e, expected);
  if (!t) t = ctx_.error();
  return record(e, t);
}

const Type* Checker::check_expr_inner(FnCtx& fc, const ast::Expr& e, const Type* expected) {
  if (fc.in_const_initializer && (e.kind == ExprKind::Return || e.kind == ExprKind::Try || e.kind == ExprKind::Await)) {
    error(e.span, "constant initializers cannot use return, ?, or await", "SNX0412");
    return ctx_.error();
  }

  switch (e.kind) {
    case ExprKind::IntLit: {
      auto& l = static_cast<const ast::IntLitExpr&>(e);
      if (l.is_long) return ctx_.prim(PrimKind::Long);
      const Type* x = expected ? shallow(expected) : nullptr;
      if (x && x->is_integer() && x->kind == TypeKind::Prim) return x;
      return ctx_.fresh_infer(InferKind::Int);
    }
    case ExprKind::FloatLit: {
      auto& l = static_cast<const ast::FloatLitExpr&>(e);
      if (l.is_float) return ctx_.prim(PrimKind::Float);
      const Type* x = expected ? shallow(expected) : nullptr;
      if (x && x->is_float() && x->kind == TypeKind::Prim) return x;
      return ctx_.fresh_infer(InferKind::Float);
    }
    case ExprKind::CharLit: return ctx_.prim(PrimKind::Char);
    case ExprKind::StringLit: return ctx_.ref(ctx_.str(), true);
    case ExprKind::BoolLit: return ctx_.bool_type();
    case ExprKind::Interp: return check_interp(fc, static_cast<const ast::InterpExpr&>(e));
    case ExprKind::Name: return check_name(fc, static_cast<const ast::NameExpr&>(e));
    case ExprKind::Field: return check_field(fc, static_cast<const ast::FieldExpr&>(e));
    case ExprKind::Call: return check_call(fc, static_cast<const ast::CallExpr&>(e), expected);
    case ExprKind::Index: return check_index(fc, static_cast<const ast::IndexExpr&>(e));
    case ExprKind::Unary: return check_unary(fc, static_cast<const ast::UnaryExpr&>(e), expected);
    case ExprKind::Binary: return check_binary(fc, static_cast<const ast::BinaryExpr&>(e), expected);
    case ExprKind::Cast: return check_cast(fc, static_cast<const ast::CastExpr&>(e));
    case ExprKind::Try: return check_try(fc, static_cast<const ast::TryExpr&>(e));
    case ExprKind::OptChain: {
      auto& o = static_cast<const ast::OptChainExpr&>(e);
      const Type* base = shallow(check_expr(fc, *o.base, nullptr));
      int refs = 0;
      const Type* b = strip_refs(base, &refs);
      if (b->kind != TypeKind::Enum || b->enum_def != prog_.option_def) {
        if (!b->is_error()) error(o.base->span, "'?.' needs an Option, found '" + type_name(base) + "'");
        return ctx_.error();
      }
      const Type* inner = strip_refs(b->elems[0], nullptr);
      if (inner->kind == TypeKind::Struct) {
        const FieldDef* f = inner->struct_def->find_field(o.name);
        if (!f) {
          error(e.span, "no field '" + o.name + "' on '" + type_name(inner) + "'");
          return ctx_.error();
        }
        Subst s;
        for (size_t i = 0; i < inner->struct_def->generics.size(); ++i) s.bind(inner->struct_def->generics[i].type, inner->elems[i]);
        NameRef ref;
        ref.kind = RefKind::Field;
        ref.struct_def = inner->struct_def;
        ref.field_index = static_cast<uint32_t>(f - inner->struct_def->fields.data());
        result_.refs[&e] = ref;
        return option_of(s.apply(ctx_, f->type));
      }
      if (!inner->is_error()) error(e.span, "'?.' field access needs a struct inside the Option, found '" + type_name(inner) + "'");
      return ctx_.error();
    }
    case ExprKind::Coalesce: {
      auto& c = static_cast<const ast::CoalesceExpr&>(e);
      const Type* v = shallow(check_expr(fc, *c.value, nullptr));
      if (v->kind != TypeKind::Enum || v->enum_def != prog_.option_def) {
        if (!v->is_error()) error(c.value->span, "'?" "?' needs an Option on the left, found '" + type_name(v) + "'");
        check_expr(fc, *c.fallback, nullptr);
        return ctx_.error();
      }
      const Type* inner = v->elems[0];
      const Type* f = check_expr(fc, *c.fallback, inner);
      coerce(*c.fallback, f, inner, "'?" "?' fallback");
      return inner;
    }
    case ExprKind::Range: return check_range(fc, static_cast<const ast::RangeExpr&>(e), expected);
    case ExprKind::Ref: {
      auto& r = static_cast<const ast::RefExpr&>(e);
      const Type* want_inner = nullptr;
      if (expected && shallow(expected)->kind == TypeKind::Ref) want_inner = shallow(expected)->inner;
      const Type* inner = check_expr(fc, *r.operand, want_inner);
      if (!r.is_const) {
        if (!is_place(fc, *r.operand)) {
          error(e.span, "cannot take a writable reference to a temporary value");
        } else if (!place_is_mutable(fc, *r.operand)) {
          error(e.span, "cannot take a writable reference to a read only value");
        }
      }
      return ctx_.ref(inner, r.is_const);
    }
    case ExprKind::StructLit: return check_struct_lit(fc, static_cast<const ast::StructLitExpr&>(e));
    case ExprKind::Tuple: {
      auto& t = static_cast<const ast::TupleExpr&>(e);
      const Type* x = expected ? shallow(expected) : nullptr;
      std::vector<const Type*> elems;
      for (size_t i = 0; i < t.elems.size(); ++i) {
        const Type* want = (x && x->kind == TypeKind::Tuple && x->elems.size() == t.elems.size()) ? x->elems[i] : nullptr;
        elems.push_back(check_expr(fc, *t.elems[i], want));
      }
      return ctx_.tuple(std::move(elems));
    }
    case ExprKind::Array: {
      auto& a = static_cast<const ast::ArrayExpr&>(e);
      const Type* x = expected ? shallow(expected) : nullptr;
      const Type* elem = (x && x->kind == TypeKind::Array) ? x->inner : nullptr;
      if (!elem) elem = ctx_.fresh_infer(InferKind::Any);
      for (auto& el : a.elems) {
        const Type* t = check_expr(fc, *el, elem);
        coerce(*el, t, elem, "array element");
      }
      return ctx_.array(elem, a.elems.size());
    }
    case ExprKind::ArrayRepeat: {
      auto& a = static_cast<const ast::ArrayRepeatExpr&>(e);
      const Type* x = expected ? shallow(expected) : nullptr;
      const Type* elem = (x && x->kind == TypeKind::Array) ? x->inner : nullptr;
      const Type* t = check_expr(fc, *a.value, elem);
      if (elem) coerce(*a.value, t, elem, "array element"); else elem = t;
      check_expr(fc, *a.count, ctx_.usize_type());
      if (a.count->kind != ExprKind::IntLit) {
        error(a.count->span, "array repeat count must be an integer literal");
        return ctx_.error();
      }
      return ctx_.array(elem, static_cast<const ast::IntLitExpr&>(*a.count).value);
    }
    case ExprKind::Block: {
      auto& b = static_cast<const ast::BlockExpr&>(e);
      return check_block(fc, *b.block, expected);
    }
    case ExprKind::If: return check_if(fc, static_cast<const ast::IfExpr&>(e), expected);
    case ExprKind::Match: return check_match(fc, static_cast<const ast::MatchExpr&>(e), expected);
    case ExprKind::Closure: return check_closure(fc, static_cast<const ast::ClosureExpr&>(e), expected);
    case ExprKind::Unsafe: {
      auto& u = static_cast<const ast::UnsafeExpr&>(e);
      ++fc.unsafe_depth;
      const Type* t = check_block(fc, *u.block, expected);
      --fc.unsafe_depth;
      return t;
    }
    case ExprKind::Await: {
      const Type* future = shallow(check_expr(fc, *static_cast<const ast::AwaitExpr&>(e).operand, nullptr));
      if (!fc.fn || !fc.fn->ast || !fc.fn->ast->is_async) {
        error(e.span, "await requires an async function body", "SNX0417");
        return ctx_.error();
      }
      while (future->kind == TypeKind::Ref) {
        if (future->is_const) { error(e.span, "await needs writable access to the future", "SNX0417"); return ctx_.error(); }
        future = shallow(future->inner);
      }
      const Type* output = ctx_.fresh_infer(InferKind::Any);
      TraitRef trait{prog_.core_trait("Future"), {output}};
      if (!implements(future, trait)) {
        if (!future->is_error()) error(e.span, "await requires a type that implements Future<T>", "SNX0417");
        return ctx_.error();
      }
      return output;
    }
    case ExprKind::Return: {
      auto& r = static_cast<const ast::ReturnExpr&>(e);
      fc.saw_return = true;
      if (r.value) {
        const Type* t = check_expr(fc, *r.value, fc.ret);
        coerce(*r.value, t, fc.ret, "return value");
      } else {
        if (!try_unify(fc.ret, ctx_.void_type())) {
          error(e.span, "this function returns '" + type_name(fc.ret) + "', so return needs a value");
        }
      }
      return ctx_.never();
    }
    case ExprKind::Break:
      if (fc.loop_depth == 0) error(e.span, "break outside a loop");
      if (!fc.loop_breaks.empty()) fc.loop_breaks.back() = true;
      return ctx_.never();
    case ExprKind::Continue:
      if (fc.loop_depth == 0) error(e.span, "continue outside a loop");
      return ctx_.never();
  }
  return ctx_.error();
}

const Type* Checker::check_name(FnCtx& fc, const ast::NameExpr& e) {
  uint32_t id;
  const LocalVar* var;
  if (lookup_local(fc, e.name, id, &var)) {
    NameRef ref;
    ref.kind = RefKind::Local;
    ref.local_id = id;
    result_.refs[&e] = ref;
    return var->type;
  }
  if (e.name == "Self") {
    NameRef ref;
    ref.kind = RefKind::TypeName;
    const Type* self = fc.type_scope.find_self();
    if (!self) {
      error(e.span, "Self is only valid inside a type");
      return ctx_.error();
    }
    const Type* s = shallow(self);
    if (s->kind == TypeKind::Struct) ref.struct_def = s->struct_def;
    if (s->kind == TypeKind::Enum) ref.enum_def = s->enum_def;
    result_.refs[&e] = ref;
    error(e.span, "'Self' names a type; use Self.item or Self { ... }");
    return ctx_.error();
  }
  const Symbol* sym = prog_.find(e.name);
  if (!sym) {
    error(e.span, "unknown name '" + e.name + "'", "SNX0401");
    return ctx_.error();
  }
  return check_symbol(fc, e, *sym, e.type_args, e.name);
}

bool Checker::accessible(ModuleDef* module, bool is_pub, Span span, const std::string& name) {
  if (!module || module == prog_.active_module || is_pub) return true;
  error(span, "'" + name + "' is private to module '" + module->name + "'", "SNX0304");
  return false;
}

bool Checker::method_accessible(FunctionDef& fn, Span span) {
  auto* drop = prog_.core_trait("Drop");
  if ((fn.owner == FnOwner::Trait && fn.owner_trait == drop) ||
      (fn.owner == FnOwner::Impl && fn.owner_impl->trait.trait == drop)) {
    error(span, "Drop.drop cannot be called explicitly; use std.mem.drop(value) to consume a value", "SNX0416");
    return false;
  }
  if (fn.is_builtin) return true;
  TraitDef* trait = fn.owner_trait ? fn.owner_trait : (fn.owner_impl ? fn.owner_impl->trait.trait : nullptr);
  if (trait) return accessible(trait->module, trait->is_pub, span, fn.name);
  return accessible(fn.module, fn.is_pub, span, fn.name);
}

const Symbol* Checker::module_member(ModuleDef& module, const std::string& name, Span span) {
  const ModuleDef* declared = nullptr;
  auto* found = module.find(name, &declared);
  if (!found) return nullptr;
  if (declared->namespace_owner == prog_.active_module) return found;
  return accessible(const_cast<ModuleDef*>(declared), found->is_pub, span, name) ? found : nullptr;
}

ModuleDef* Checker::module_expr(FnCtx& fc, const ast::Expr& e) {
  const Symbol* symbol = nullptr;
  if (e.kind == ExprKind::Name) {
    auto& name = static_cast<const ast::NameExpr&>(e);
    uint32_t id; const LocalVar* var;
    if (lookup_local(fc, name.name, id, &var, false)) return nullptr;
    symbol = prog_.find(name.name);
  } else if (e.kind == ExprKind::Field) {
    auto& field = static_cast<const ast::FieldExpr&>(e);
    auto* module = module_expr(fc, *field.base);
    if (!module) return nullptr;
    auto* member = module->find(field.name);
    if (member && member->kind == SymbolKind::Module)
      symbol = module_member(*module, field.name, field.name_span);
  }
  return symbol && symbol->kind == SymbolKind::Module ? symbol->module_def : nullptr;
}

const Type* Checker::check_symbol(FnCtx& fc, const ast::Expr& e, const Symbol& symbol,
                                  const std::vector<ast::TypePtr>& type_args, const std::string& name) {
  NameRef ref;
  switch (symbol.kind) {
    case SymbolKind::Function: {
      std::vector<const Type*> explicit_args = resolve_explicit_args(fc, type_args);
      ensure_signature(*symbol.fn_def, e.span);
      const Type* t = instantiate_function(*symbol.fn_def, explicit_args, Subst(), e.span, ref);
      ref.kind = RefKind::Function;
      result_.refs[&e] = ref;
      return t;
    }
    case SymbolKind::Variant: {
      std::vector<const Type*> args;
      for (auto& g : symbol.enum_def->generics) { (void)g; args.push_back(ctx_.fresh_infer(InferKind::Any)); }
      const Type* t = variant_value_type(*symbol.variant, args, ref);
      result_.refs[&e] = ref;
      return t;
    }
    case SymbolKind::Const:
      check_const(*symbol.const_def);
      ref.kind = RefKind::Const;
      ref.const_def = symbol.const_def;
      result_.refs[&e] = ref;
      return symbol.const_def->type;
    case SymbolKind::Struct:
    case SymbolKind::Enum:
      ref.kind = RefKind::TypeName;
      ref.struct_def = symbol.struct_def;
      ref.enum_def = symbol.enum_def;
      result_.refs[&e] = ref;
      error(e.span, "'" + name + "' is a type, not a value");
      return ctx_.error();
    case SymbolKind::Module:
      error(e.span, "'" + name + "' is a module, not a value");
      return ctx_.error();
    case SymbolKind::Trait:
      error(e.span, "'" + name + "' is a trait, not a value");
      return ctx_.error();
  }
  return ctx_.error();
}

std::vector<const Type*> Checker::resolve_explicit_args(FnCtx& fc, const std::vector<ast::TypePtr>& args) {
  std::vector<const Type*> out;
  for (auto& a : args) out.push_back(resolver_.resolve_type(*a, fc.type_scope));
  return out;
}

// Unit variant: value of the enum type. Tuple variant: constructor function.
const Type* Checker::variant_value_type(const VariantDef& v, std::vector<const Type*> enum_args, NameRef& ref) {
  ref.kind = RefKind::Variant;
  ref.variant = &v;
  ref.enum_def = v.owner;
  ref.owner_args = enum_args;
  const Type* enum_t = ctx_.enum_type(v.owner, enum_args);
  Subst s;
  for (size_t i = 0; i < v.owner->generics.size(); ++i) s.bind(v.owner->generics[i].type, enum_args[i]);
  switch (v.kind) {
    case ast::VariantKind::Unit: return enum_t;
    case ast::VariantKind::Tuple: {
      std::vector<const Type*> params;
      for (auto* t : v.tuple_types) params.push_back(s.apply(ctx_, t));
      return ctx_.fn(std::move(params), enum_t);
    }
    case ast::VariantKind::Struct:
      return enum_t;  // used only through a struct literal; checked there
  }
  return ctx_.error();
}

// True if e names a type (a bare struct or enum name, or Self). Fills the
// def and the type arguments (explicit or fresh).
bool Checker::resolve_type_name(FnCtx& fc, const ast::Expr& e, StructDef*& s, EnumDef*& en,
                                std::vector<const Type*>& args, Span span) {
  s = nullptr;
  en = nullptr;
  const Symbol* sym = nullptr;
  const std::vector<ast::TypePtr>* explicit_args = nullptr;
  std::string name;
  if (e.kind == ExprKind::Name) {
    auto& n = static_cast<const ast::NameExpr&>(e);
    uint32_t id; const LocalVar* var;
    if (lookup_local(fc, n.name, id, &var, false)) return false;
  if (n.name == "Self") {
    const Type* self = fc.type_scope.find_self();
    if (!self) return false;
    const Type* t = shallow(self);
    if (t->kind == TypeKind::Struct) s = t->struct_def;
    else if (t->kind == TypeKind::Enum) en = t->enum_def;
    else return false;
    args = t->elems;
    NameRef ref;
    ref.kind = RefKind::TypeName;
    ref.struct_def = s;
    ref.enum_def = en;
    result_.refs[&e] = ref;
    result_.expr_types[&e] = ctx_.void_type();
    return true;
  }
    sym = prog_.find(n.name);
    explicit_args = &n.type_args;
    name = n.name;
  } else if (e.kind == ExprKind::Field) {
    auto& field = static_cast<const ast::FieldExpr&>(e);
    auto* module = module_expr(fc, *field.base);
    if (!module) return false;
    sym = module_member(*module, field.name, field.name_span);
    explicit_args = &field.type_args;
    name = field.name;
  } else return false;
  if (!sym || (sym->kind != SymbolKind::Struct && sym->kind != SymbolKind::Enum)) return false;
  s = sym->struct_def;
  en = sym->enum_def;
  size_t want = s ? s->generics.size() : en->generics.size();
  args = resolve_explicit_args(fc, *explicit_args);
  if (!args.empty() && args.size() != want) {
    error(span, "'" + name + "' expects " + std::to_string(want) + " type arguments, got " + std::to_string(args.size()));
    args.clear();
  }
  if (args.empty()) {
    for (size_t i = 0; i < want; ++i) args.push_back(ctx_.fresh_infer(InferKind::Any));
  }
  NameRef ref;
  ref.kind = RefKind::TypeName;
  ref.struct_def = s;
  ref.enum_def = en;
  result_.refs[&e] = ref;
  result_.expr_types[&e] = ctx_.void_type();
  return true;
}

const Type* Checker::instantiate_function(FunctionDef& fn, const std::vector<const Type*>& explicit_args,
                                          const Subst& owner_subst, Span span, NameRef& ref) {
  ref.fn = &fn;
  Subst s = owner_subst;
  if (!explicit_args.empty() && explicit_args.size() != fn.generics.size()) {
    error(span, "'" + fn.name + "' expects " + std::to_string(fn.generics.size()) + " type arguments, got " +
                    std::to_string(explicit_args.size()));
  }
  for (size_t i = 0; i < fn.generics.size(); ++i) {
    const Type* arg = (explicit_args.size() == fn.generics.size()) ? explicit_args[i] : ctx_.fresh_infer(InferKind::Any);
    s.bind(fn.generics[i].type, arg);
    ref.type_args.push_back(arg);
    for (auto& b : fn.generics[i].bounds) {
      TraitRef br = b;
      for (auto*& a : br.args) a = s.apply(ctx_, a);
      add_obligation(arg, br, span, "type argument '" + fn.generics[i].name + "' of '" + fn.name + "'");
    }
  }
  std::vector<const Type*> params;
  for (auto& p : fn.params) params.push_back(s.apply(ctx_, p.type));
  const Type* ret = fn.ret ? s.apply(ctx_, fn.ret) : ctx_.error();
  if (fn.ast && fn.ast->is_async && prog_.task_def) ret = ctx_.struct_type(fn.ast->async_send ? prog_.send_task_def : prog_.task_def, {ret});
  return ctx_.fn(std::move(params), ret);
}

const Type* Checker::check_field(FnCtx& fc, const ast::FieldExpr& e) {
  if (auto* module = module_expr(fc, *e.base)) {
    auto* symbol = module_member(*module, e.name, e.name_span);
    if (!symbol) {
      // A private item already reported SNX0304 in module_member.
      if (!module->find(e.name)) error(e.name_span, "no public item '" + e.name + "' in module '" + module->name + "'");
      return ctx_.error();
    }
    return check_symbol(fc, e, *symbol, e.type_args, e.name);
  }
  StructDef* s;
  EnumDef* en;
  std::vector<const Type*> args;
  if (resolve_type_name(fc, *e.base, s, en, args, e.base->span)) {
    // Type.item: a variant, or an associated function.
    NameRef ref;
    if (en) {
      if (const VariantDef* v = en->find_variant(e.name)) {
        const Type* t = variant_value_type(*v, args, ref);
        result_.refs[&e] = ref;
        return t;
      }
    }
    FunctionDef* fn = s ? s->find_method(e.name) : en->find_method(e.name);
    if (!fn) {
      error(e.name_span, "no item '" + e.name + "' on type '" + (s ? s->name : en->name) + "'");
      return ctx_.error();
    }
    if (!method_accessible(*fn, e.name_span)) return ctx_.error();
    if (fn->has_self()) {
      error(e.name_span, "'" + e.name + "' is a method; call it on a value");
      return ctx_.error();
    }
    Subst owner;
    auto& generics = s ? s->generics : en->generics;
    for (size_t i = 0; i < generics.size(); ++i) owner.bind(generics[i].type, args[i]);
    ensure_signature(*fn, e.span);
    const Type* t = instantiate_function(*fn, resolve_explicit_args(fc, e.type_args), owner, e.span, ref);
    ref.kind = RefKind::AssocFn;
    ref.struct_def = s;
    ref.enum_def = en;
    ref.owner_args = args;
    result_.refs[&e] = ref;
    return t;
  }

  const Type* base = check_expr(fc, *e.base, nullptr);
  int derefs = 0;
  const Type* b = strip_refs(base, &derefs);
  if (b->is_error()) return ctx_.error();

  if (b->kind == TypeKind::Tuple && !e.name.empty() && std::isdigit(static_cast<unsigned char>(e.name[0]))) {
    size_t idx = std::stoul(e.name);
    if (idx >= b->elems.size()) {
      error(e.name_span, "tuple of type '" + type_name(b) + "' has no element " + e.name);
      return ctx_.error();
    }
    NameRef ref;
    ref.kind = RefKind::TupleField;
    ref.field_index = static_cast<uint32_t>(idx);
    ref.auto_derefs = derefs;
    result_.refs[&e] = ref;
    return b->elems[idx];
  }
  if (b->kind == TypeKind::Struct) {
    const FieldDef* f = b->struct_def->find_field(e.name);
    if (f) {
      if (!accessible(b->struct_def->module, f->is_pub, e.name_span, f->name)) return ctx_.error();
      Subst sub;
      for (size_t i = 0; i < b->struct_def->generics.size(); ++i) sub.bind(b->struct_def->generics[i].type, b->elems[i]);
      NameRef ref;
      ref.kind = RefKind::Field;
      ref.struct_def = b->struct_def;
      ref.field_index = static_cast<uint32_t>(f - b->struct_def->fields.data());
      ref.auto_derefs = derefs;
      result_.refs[&e] = ref;
      return sub.apply(ctx_, f->type);
    }
    if (b->struct_def->find_method(e.name)) {
      error(e.name_span, "'" + e.name + "' is a method; add parentheses to call it");
      return ctx_.error();
    }
    error(e.name_span, "no field '" + e.name + "' on type '" + type_name(b) + "'");
    return ctx_.error();
  }
  if (b->kind == TypeKind::Enum) {
    error(e.name_span, "cannot access '" + e.name + "' on enum '" + type_name(b) + "'; use match");
    return ctx_.error();
  }
  MethodHit hit;
  if (lookup_method(base, e.name, hit)) {
    error(e.name_span, "'" + e.name + "' is a method; add parentheses to call it");
    return ctx_.error();
  }
  error(e.name_span, "no field '" + e.name + "' on type '" + type_name(base) + "'");
  return ctx_.error();
}

// ---------- calls ----------

const Type* Checker::check_args(FnCtx& fc, const std::vector<ast::ExprPtr>& args, const Type* fn_type,
                                size_t skip_params, Span span) {
  const Type* ft = shallow(fn_type);
  if (ft->is_error()) {
    for (auto& a : args) check_expr(fc, *a, nullptr);
    return ctx_.error();
  }
  size_t want = ft->elems.size() - skip_params;
  if (args.size() != want) {
    error(span, "expected " + std::to_string(want) + " argument" + (want == 1 ? "" : "s") + ", got " +
                    std::to_string(args.size()));
    for (auto& a : args) check_expr(fc, *a, nullptr);
    return ft->ret;
  }
  for (size_t i = 0; i < args.size(); ++i) {
    const Type* want_t = ft->elems[i + skip_params];
    const Type* got = check_expr(fc, *args[i], want_t);
    coerce(*args[i], got, want_t, "argument");
  }
  return ft->ret;
}

const Type* Checker::check_call(FnCtx& fc, const ast::CallExpr& e, const Type* expected) {
  const ast::Expr& callee = *e.callee;

  if (callee.kind == ExprKind::OptChain) {
    // x?.method(args): call on the value inside an Option, result Option<R>.
    auto& o = static_cast<const ast::OptChainExpr&>(callee);
    const Type* base = check_expr(fc, *o.base, nullptr);
    int refs = 0;
    const Type* b = strip_refs(base, &refs);
    if (b->kind != TypeKind::Enum || b->enum_def != prog_.option_def) {
      if (!b->is_error()) error(o.base->span, "'?.' needs an Option, found '" + type_name(base) + "'");
      for (auto& a : e.args) check_expr(fc, *a, nullptr);
      return ctx_.error();
    }
    const Type* inner = b->elems[0];
    MethodHit hit;
    if (!lookup_method(inner, o.name, hit)) {
      error(callee.span, "no method '" + o.name + "' on type '" + type_name(inner) + "'", "SNX0407");
      for (auto& a : e.args) check_expr(fc, *a, nullptr);
      return ctx_.error();
    }
    if (!method_accessible(*hit.fn, callee.span)) return ctx_.error();
    NameRef ref;
    ensure_signature(*hit.fn, e.span);
    const Type* ft = instantiate_function(*hit.fn, {}, hit.owner_subst, e.span, ref);
    ref.kind = RefKind::Method;
    ref.impl = hit.impl;
    ref.trait = hit.trait;
    ref.auto_derefs = hit.derefs;
    ref.owner_args = hit.owner_args;
    result_.refs[&e] = ref;
    result_.expr_types[&callee] = ft;
    const Type* ret = check_args(fc, e.args, ft, hit.fn->has_self() ? 1 : 0, e.span);
    return option_of(ret);
  }

  if (callee.kind == ExprKind::Field) {
    auto& f = static_cast<const ast::FieldExpr&>(callee);
    StructDef* s;
    EnumDef* en;
    std::vector<const Type*> args;
    size_t mark = snapshot();
    (void)mark;
    if (!module_expr(fc, *f.base) && !resolve_type_name(fc, *f.base, s, en, args, f.base->span)) {
      return check_method_call(fc, e, f, expected);
    }
    // Type.item(...): fall through to the generic path below by checking
    // the callee as an expression; check_field handles Type.item.
  }

  NameRef ref;
  const Type* ft = check_expr(fc, callee, nullptr);
  auto it = result_.refs.find(&callee);
  if (it != result_.refs.end()) ref = it->second;
  const Type* t = strip_refs(ft, nullptr);
  if (t->kind != TypeKind::Fn) {
    if (!t->is_error()) error(callee.span, "cannot call a value of type '" + type_name(ft) + "'");
    for (auto& a : e.args) check_expr(fc, *a, nullptr);
    return ctx_.error();
  }
  if (ref.kind == RefKind::Function && ref.fn && ref.fn->is_builtin && (ref.fn->name == "drop_in_place" || ref.fn->name == "str_from_raw") && fc.unsafe_depth == 0) {
    error(e.span, ref.fn->name + " requires an unsafe block", "SNX0415");
  }
  if (ref.kind == RefKind::Function && ref.fn && ref.fn->owner == FnOwner::Extern && fc.unsafe_depth == 0) {
    error(e.span, "calling an extern function requires an unsafe block");
  }
  // Propagate the expected result into the return type for inference.
  bool readonly = is_place(fc, callee) && !place_is_mutable(fc, callee);
  for (auto* r = shallow(ft); r->kind == TypeKind::Ref; r = shallow(r->inner)) readonly = readonly || r->is_const;
  if (t->call_mode == ast::CallMode::Once && shallow(ft)->kind == TypeKind::Ref)
    error(callee.span, "a consuming function must be owned to call it", "SNX0408");
  if (t->call_mode == ast::CallMode::Writable && ref.kind != RefKind::Function && ref.kind != RefKind::AssocFn && readonly) {
    error(callee.span, "calling a function value requires writable access", "SNX0408");
  }
  if (expected) try_unify(t->ret, expected);
  return check_args(fc, e.args, t, 0, e.span);
}

bool Checker::method_in_impls(const Type* base, const std::string& name, MethodHit& hit) {
  for (auto& impl : prog_.impls) {
    FunctionDef* m = impl->find_method(name);
    if (!m) continue;
    // Instantiate the impl generics with fresh variables and try to match.
    Subst s;
    std::vector<const Type*> fresh;
    for (auto& g : impl->generics) {
      const Type* v = ctx_.fresh_infer(InferKind::Any);
      s.bind(g.type, v);
      fresh.push_back(v);
    }
    const Type* self = s.apply(ctx_, impl->self_type);
    size_t mark = snapshot();
    if (unify(self, base)) {
      hit.fn = m;
      hit.impl = impl.get();
      hit.owner_subst = s;
      hit.owner_args = fresh;
      for (size_t i = 0; i < impl->generics.size(); ++i) {
        for (auto& b : impl->generics[i].bounds) {
          TraitRef br = b;
          for (auto*& a : br.args) a = s.apply(ctx_, a);
          add_obligation(fresh[i], br, impl->span, "impl type parameter '" + impl->generics[i].name + "'");
        }
      }
      return true;
    }
    rollback(mark);
  }
  // Methods declared on a trait with a default body, for a type that has
  // an impl without that method.
  return false;
}

bool Checker::lookup_method(const Type* recv, const std::string& name, MethodHit& hit) {
  int derefs = 0;
  const Type* base = strip_refs(recv, &derefs);
  hit.derefs = derefs;
  hit.base = base;
  if (base->is_error()) return false;

  if (base->kind == TypeKind::Struct || base->kind == TypeKind::Enum) {
    FunctionDef* m = base->kind == TypeKind::Struct ? base->struct_def->find_method(name) : base->enum_def->find_method(name);
    if (m) {
      auto& generics = base->kind == TypeKind::Struct ? base->struct_def->generics : base->enum_def->generics;
      for (size_t i = 0; i < generics.size(); ++i) hit.owner_subst.bind(generics[i].type, base->elems[i]);
      hit.owner_args = base->elems;
      hit.fn = m;
      return true;
    }
  }
  if (base->kind == TypeKind::Str) {
    for (auto* m : builtin_str_methods_) {
      if (m->name == name) {
        hit.fn = m;
        return true;
      }
    }
  }
  if (base->kind == TypeKind::Dyn) {
    if (auto* m = base->trait_def->find_method(name)) {
      hit.fn = m;
      hit.trait = base->trait_def;
      hit.owner_subst.bind(base->trait_def->self_type, base);
      for (size_t i = 0; i < base->elems.size(); ++i)
        hit.owner_subst.bind(base->trait_def->generics[i].type, base->elems[i]);
      hit.owner_args = base->elems;
      return true;
    }
    return false;
  }
  if (base->kind == TypeKind::Param) {
    const std::vector<TraitRef>* bounds = bounds_of(base);
    if (bounds) {
      for (auto& b : *bounds) {
        if (!b.trait) continue;
        FunctionDef* m = b.trait->find_method(name);
        if (m) {
          hit.fn = m;
          hit.trait = b.trait;
          hit.owner_subst.bind(b.trait->self_type, base);
          for (size_t i = 0; i < b.trait->generics.size() && i < b.args.size(); ++i) {
            hit.owner_subst.bind(b.trait->generics[i].type, b.args[i]);
          }
          hit.owner_args = b.args;
          return true;
        }
      }
    }
  }
  if (method_in_impls(base, name, hit)) {
    // Bind Self for trait default bodies and the trait's own params.
    if (hit.impl && hit.impl->trait.trait) {
      hit.owner_subst.bind(hit.impl->trait.trait->self_type, base);
      for (size_t i = 0; i < hit.impl->trait.trait->generics.size() && i < hit.impl->trait.args.size(); ++i) {
        hit.owner_subst.bind(hit.impl->trait.trait->generics[i].type, hit.owner_subst.apply(ctx_, hit.impl->trait.args[i]));
      }
    }
    return true;
  }
  // Trait default methods: any impl of a trait that declares the method
  // with a body, for this type.
  for (auto& impl : prog_.impls) {
    if (!impl->trait.trait) continue;
    FunctionDef* m = impl->trait.trait->find_method(name);
    if (!m || !m->ast || !m->ast->body) continue;
    Subst s;
    std::vector<const Type*> fresh;
    for (auto& g : impl->generics) {
      const Type* v = ctx_.fresh_infer(InferKind::Any);
      s.bind(g.type, v);
      fresh.push_back(v);
    }
    size_t mark = snapshot();
    if (unify(s.apply(ctx_, impl->self_type), base)) {
      hit.fn = m;
      hit.impl = impl.get();
      hit.trait = impl->trait.trait;
      hit.owner_subst = s;
      hit.owner_subst.bind(impl->trait.trait->self_type, base);
      hit.owner_args = fresh;
      return true;
    }
    rollback(mark);
  }
  return false;
}

void Checker::check_receiver(FnCtx& fc, const ast::Expr& recv_expr, const Type* recv_type,
                             const MethodHit& hit, Span span) {
  if (!hit.fn->has_self()) return;
  ast::SelfKind kind = hit.fn->params[0].self_kind;
  const Type* rt = shallow(recv_type);
  Coercion c;
  switch (kind) {
    case ast::SelfKind::Value:
      if (hit.derefs > 0 && !is_copy(hit.base)) {
        error(span, "method '" + hit.fn->name + "' takes self by value, but the receiver is a reference to a non Copy value");
      }
      break;
    case ast::SelfKind::Ref:
      if (rt->kind == TypeKind::Ref) {
        if (rt->is_const) {
          error(span, "method '" + hit.fn->name + "' needs a writable reference, but the receiver is read only");
        }
      } else {
        if (!is_place(fc, recv_expr)) {
          error(span, "method '" + hit.fn->name + "' needs a writable reference to a variable, not a temporary");
        } else if (!place_is_mutable(fc, recv_expr)) {
          error(span, "method '" + hit.fn->name + "' changes the value, but the receiver is read only");
        }
        c.kind = CoercionKind::AutoBorrowMut;
        result_.coercions[&recv_expr] = c;
      }
      break;
    case ast::SelfKind::ConstRef:
      if (rt->kind != TypeKind::Ref) {
        c.kind = CoercionKind::AutoBorrowConst;
        result_.coercions[&recv_expr] = c;
      }
      break;
    default:
      break;
  }
}

const Type* Checker::check_method_call(FnCtx& fc, const ast::CallExpr& call, const ast::FieldExpr& callee,
                                       const Type* expected) {
  const Type* recv = check_expr(fc, *callee.base, nullptr);
  int field_derefs = 0;
  const Type* base = strip_refs(recv, &field_derefs);
  const Type* field_type = nullptr;
  NameRef field_ref;
  if (base->kind == TypeKind::Struct) {
    if (auto* field = base->struct_def->find_field(callee.name)) {
      Subst subst;
      for (size_t i = 0; i < base->elems.size(); ++i) subst.bind(base->struct_def->generics[i].type, base->elems[i]);
      field_type = subst.apply(ctx_, field->type);
      field_ref.kind = RefKind::Field;
      field_ref.struct_def = base->struct_def;
      field_ref.field_index = static_cast<uint32_t>(field - base->struct_def->fields.data());
    }
  } else if (base->kind == TypeKind::Tuple && !callee.name.empty() &&
             std::isdigit(static_cast<unsigned char>(callee.name[0]))) {
    size_t index = std::stoul(callee.name);
    if (index < base->elems.size()) {
      field_type = base->elems[index];
      field_ref.kind = RefKind::TupleField;
      field_ref.field_index = static_cast<uint32_t>(index);
    }
  }
  const Type* callable = field_type ? strip_refs(field_type, nullptr) : nullptr;
  if (callable && callable->kind == TypeKind::Fn) {
    if (base->kind == TypeKind::Struct) {
      auto* field = base->struct_def->find_field(callee.name);
      if (field && !accessible(base->struct_def->module, field->is_pub, callee.name_span, field->name)) return ctx_.error();
    }
    field_ref.auto_derefs = field_derefs;
    result_.refs[&callee] = field_ref;
    result_.expr_types[&callee] = field_type;
    bool readonly = is_place(fc, callee) && !place_is_mutable(fc, callee);
    for (auto* r = shallow(field_type); r->kind == TypeKind::Ref; r = shallow(r->inner)) readonly = readonly || r->is_const;
    if (callable->call_mode == ast::CallMode::Once && shallow(field_type)->kind == TypeKind::Ref)
      error(callee.span, "a consuming function must be owned to call it", "SNX0408");
    if (callable->call_mode == ast::CallMode::Writable && readonly) {
      error(callee.span, "calling a function value requires writable access", "SNX0408");
    }
    if (expected) try_unify(callable->ret, expected);
    return check_args(fc, call.args, callable, 0, call.span);
  }
  MethodHit hit;
  if (!lookup_method(recv, callee.name, hit)) {
    const Type* b = strip_refs(recv, nullptr);
    if (b->kind == TypeKind::Struct && b->struct_def->find_field(callee.name)) {
      error(callee.name_span, "'" + callee.name + "' is a field, not a method");
    } else if (!b->is_error()) {
      error(callee.name_span, "no method '" + callee.name + "' on type '" + type_name(recv) + "'", "SNX0407");
    }
    for (auto& a : call.args) check_expr(fc, *a, nullptr);
    return ctx_.error();
  }
  if (!method_accessible(*hit.fn, callee.name_span)) return ctx_.error();
  if (!hit.fn->has_self()) {
    error(callee.name_span, "'" + callee.name + "' is an associated function; call it as " +
                                type_name(hit.base) + "." + callee.name + "(...)");
    for (auto& a : call.args) check_expr(fc, *a, nullptr);
    return ctx_.error();
  }
  ensure_signature(*hit.fn, call.span);
  NameRef ref;
  const Type* ft = instantiate_function(*hit.fn, resolve_explicit_args(fc, callee.type_args), hit.owner_subst, call.span, ref);
  ref.kind = RefKind::Method;
  ref.impl = hit.impl;
  ref.trait = hit.trait;
  ref.auto_derefs = hit.derefs;
  ref.owner_args = hit.owner_args;
  result_.refs[&call] = ref;
  result_.expr_types[&callee] = ft;
  check_receiver(fc, *callee.base, recv, hit, callee.name_span);
  const Type* t = shallow(ft);
  if (expected) try_unify(t->ret, expected);
  return check_args(fc, call.args, ft, 1, call.span);
}

// ---------- other expressions ----------

const Type* Checker::check_index(FnCtx& fc, const ast::IndexExpr& e) {
  const Type* base = check_expr(fc, *e.base, nullptr);
  int derefs = 0;
  const Type* b = strip_refs(base, &derefs);
  const Type* elem = nullptr;
  if (b->kind == TypeKind::Array || b->kind == TypeKind::Slice) {
    elem = b->inner;
  } else if (b->kind == TypeKind::Struct && b->struct_def == prog_.vec_def && !b->elems.empty()) {
    elem = b->elems[0];
  } else if (b->kind == TypeKind::Struct && b->struct_def == prog_.string_def) {
    error(e.span, "strings cannot be indexed by position; use as_str() and byte or char methods");
    check_expr(fc, *e.index, nullptr);
    return ctx_.error();
  } else {
    if (!b->is_error()) error(e.base->span, "cannot index a value of type '" + type_name(base) + "'");
    check_expr(fc, *e.index, nullptr);
    return ctx_.error();
  }
  NameRef ref;
  ref.kind = RefKind::Field;
  ref.auto_derefs = derefs;
  result_.refs[&e] = ref;
  const Type* idx = check_expr(fc, *e.index, ctx_.usize_type());
  const Type* it = shallow(idx);
  if (it->kind == TypeKind::Struct && (it->struct_def == prog_.range_def || it->struct_def == prog_.range_inclusive_def)) {
    unify_or_error(it->elems[0], ctx_.usize_type(), e.index->span, "slice range");
    return ctx_.slice(elem);
  }
  unify_or_error(idx, ctx_.usize_type(), e.index->span, "index");
  return elem;
}

const Type* Checker::check_unary(FnCtx& fc, const ast::UnaryExpr& e, const Type* expected) {
  switch (e.op) {
    case ast::UnaryOp::Deref: {
      const Type* t = shallow(check_expr(fc, *e.operand, nullptr));
      if (t->kind == TypeKind::Ref) return t->inner;
      if (t->kind == TypeKind::Ptr) {
        if (fc.unsafe_depth == 0) error(e.span, "dereferencing a raw pointer requires an unsafe block");
        return t->inner;
      }
      if (!t->is_error()) error(e.span, "cannot dereference a value of type '" + type_name(t) + "'");
      return ctx_.error();
    }
    case ast::UnaryOp::Not: {
      const Type* t = check_expr(fc, *e.operand, ctx_.bool_type());
      const Type* s = shallow(t);
      if (s->is_bool() || s->is_error()) return ctx_.bool_type();
      if (s->is_integer()) return t;  // bitwise not on integers
      TraitDef* not_trait = prog_.core_trait("Not");
      if (not_trait) {
        TraitRef r;
        r.trait = not_trait;
        if (implements(s, r)) return t;
      }
      error(e.span, "operator '!' needs a bool, found '" + type_name(t) + "'");
      return ctx_.error();
    }
    case ast::UnaryOp::BitNot: {
      const Type* t = check_expr(fc, *e.operand, expected);
      if (shallow(t)->is_integer() || shallow(t)->is_error()) return t;
      error(e.span, "operator '~' needs an integer, found '" + type_name(t) + "'");
      return ctx_.error();
    }
    case ast::UnaryOp::Neg: {
      const Type* t = check_expr(fc, *e.operand, expected);
      const Type* s = shallow(t);
      if (s->is_numeric() || s->is_error()) {
        if (s->kind == TypeKind::Prim && !s->is_signed_integer() && !s->is_float()) {
          error(e.span, "cannot negate an unsigned value of type '" + type_name(t) + "'");
        }
        return t;
      }
      TraitDef* neg = prog_.core_trait("Neg");
      if (neg) {
        TraitRef r;
        r.trait = neg;
        if (implements(s, r)) return t;
      }
      error(e.span, "operator '-' is not defined for '" + type_name(t) + "'");
      return ctx_.error();
    }
  }
  return ctx_.error();
}

const Type* Checker::operator_result(FnCtx& fc, const ast::BinaryExpr& e, const Type* lhs, const Type* rhs) {
  (void)fc;
  const Type* l = shallow(lhs);
  const Type* r = shallow(rhs);
  if (l->is_error() || r->is_error()) return ctx_.error();
  const char* trait_name = nullptr;
  bool is_cmp = false;
  bool is_bits = false;
  switch (e.op) {
    case ast::BinaryOp::Add: trait_name = "Add"; break;
    case ast::BinaryOp::Sub: trait_name = "Sub"; break;
    case ast::BinaryOp::Mul: trait_name = "Mul"; break;
    case ast::BinaryOp::Div: trait_name = "Div"; break;
    case ast::BinaryOp::Rem: trait_name = "Rem"; break;
    case ast::BinaryOp::Eq: case ast::BinaryOp::Ne: trait_name = "Eq"; is_cmp = true; break;
    case ast::BinaryOp::Lt: case ast::BinaryOp::Gt: case ast::BinaryOp::Le: case ast::BinaryOp::Ge:
      trait_name = "Ord"; is_cmp = true; break;
    case ast::BinaryOp::BitAnd: case ast::BinaryOp::BitOr: case ast::BinaryOp::BitXor:
    case ast::BinaryOp::Shl: case ast::BinaryOp::Shr: is_bits = true; break;
    default: break;
  }
  if (is_bits) {
    if (!l->is_integer() || !r->is_integer()) {
      error(e.span, std::string("operator '") + ast::binary_op_name(e.op) + "' needs integers, found '" +
                        type_name(l) + "' and '" + type_name(r) + "'");
      return ctx_.error();
    }
    if (e.op == ast::BinaryOp::Shl || e.op == ast::BinaryOp::Shr) return lhs;
    unify_or_error(rhs, lhs, e.rhs->span, "operator operands");
    return lhs;
  }
  // Comparisons look through references on both sides.
  const Type* lb = is_cmp ? strip_refs(l, nullptr) : l;
  const Type* rb = is_cmp ? strip_refs(r, nullptr) : r;
  if (!unify(lb, rb)) {
    error(e.span, std::string("operator '") + ast::binary_op_name(e.op) + "' needs both sides to have the same type, found '" +
                      type_name(lhs) + "' and '" + type_name(rhs) + "'");
    return ctx_.error();
  }
  const Type* t = shallow(lb);
  bool builtin_ok = t->is_numeric() || (is_cmp && (t->is_bool() || t->is_prim(PrimKind::Char) || t->kind == TypeKind::Str ||
                                                  (t->kind == TypeKind::Struct && t->struct_def == prog_.string_def)));
  if (is_cmp && e.op != ast::BinaryOp::Eq && e.op != ast::BinaryOp::Ne && t->is_bool()) builtin_ok = false;
  if (!builtin_ok && !t->is_infer()) {
    TraitDef* tr = prog_.core_trait(trait_name);
    TraitRef want;
    want.trait = tr;
    if (!tr || !implements(t, want)) {
      error(e.span, std::string("operator '") + ast::binary_op_name(e.op) + "' is not defined for '" + type_name(t) +
                        "'; implement the trait '" + trait_name + "'");
      return ctx_.error();
    }
  }
  return is_cmp ? ctx_.bool_type() : lhs;
}

const Type* Checker::check_binary(FnCtx& fc, const ast::BinaryExpr& e, const Type* expected) {
  if (e.op == ast::BinaryOp::And || e.op == ast::BinaryOp::Or) {
    const Type* l = check_expr(fc, *e.lhs, ctx_.bool_type());
    const Type* r = check_expr(fc, *e.rhs, ctx_.bool_type());
    unify_or_error(l, ctx_.bool_type(), e.lhs->span, "logical operator");
    unify_or_error(r, ctx_.bool_type(), e.rhs->span, "logical operator");
    return ctx_.bool_type();
  }
  bool arithmetic = e.op == ast::BinaryOp::Add || e.op == ast::BinaryOp::Sub || e.op == ast::BinaryOp::Mul ||
                    e.op == ast::BinaryOp::Div || e.op == ast::BinaryOp::Rem;
  const Type* hint = arithmetic ? expected : nullptr;
  const Type* l = check_expr(fc, *e.lhs, hint);
  const Type* lh = shallow(l);
  const Type* rhint = (lh->kind == TypeKind::Prim) ? lh : hint;
  const Type* r = check_expr(fc, *e.rhs, rhint);
  return operator_result(fc, e, l, r);
}

const Type* Checker::check_cast(FnCtx& fc, const ast::CastExpr& e) {
  const Type* target = resolver_.resolve_type(*e.type, fc.type_scope);
  const Type* from = shallow(check_expr(fc, *e.operand, nullptr));
  if (from->is_infer() && from->infer_kind == InferKind::Int) unify(from, ctx_.int_type());
  if (from->is_infer() && from->infer_kind == InferKind::Float) unify(from, ctx_.double_type());
  from = shallow(from);
  auto castable = [&](const Type* t) {
    return t->is_numeric() || t->is_bool() || t->is_prim(PrimKind::Char);
  };
  if (from->is_error() || target->is_error()) return target;
  if (castable(from) && castable(target)) {
    if (target->is_bool()) {
      error(e.span, "cannot cast to bool; compare with 0 instead");
      return ctx_.error();
    }
    if (target->is_prim(PrimKind::Char) && !from->is_prim(PrimKind::Byte) && !from->is_prim(PrimKind::Char)) {
      error(e.span, "only byte can be cast to char; other integers may not be valid characters");
      return ctx_.error();
    }
    return target;
  }
  if (from->kind == TypeKind::Ptr && target->kind == TypeKind::Ptr) return target;
  if (from->kind == TypeKind::Ref && target->kind == TypeKind::Ptr && !from->inner->is_unsized() &&
      (!from->is_const || target->is_const) && try_unify(from->inner, target->inner)) return target;
  if (from->kind == TypeKind::Ptr && target->is_integer()) return target;
  if (from->is_integer() && target->kind == TypeKind::Ptr) return target;
  error(e.span, "cannot cast '" + type_name(from) + "' to '" + type_name(target) + "'");
  return ctx_.error();
}

const Type* Checker::check_try(FnCtx& fc, const ast::TryExpr& e) {
  const Type* t = shallow(check_expr(fc, *e.operand, nullptr));
  if (t->is_error()) return ctx_.error();
  const Type* ret = shallow(fc.ret);
  if (t->kind == TypeKind::Enum && t->enum_def == prog_.result_def) {
    if (ret->is_infer()) {
      // Inferred return type: it must be a Result with the same error type.
      unify(ret, ctx_.enum_type(prog_.result_def, {ctx_.fresh_infer(InferKind::Any), t->elems[1]}));
      ret = shallow(fc.ret);
    }
    if (ret->kind != TypeKind::Enum || ret->enum_def != prog_.result_def) {
      error(e.span, "'?' on a Result needs the function to return a Result, but it returns '" + type_name(fc.ret) + "'");
      return t->elems[0];
    }
    if (!unify(t->elems[1], ret->elems[1])) {
      error(e.span, "'?' error type mismatch: this Result has error type '" + type_name(t->elems[1]) +
                        "' but the function returns '" + type_name(fc.ret) + "'");
    }
    return t->elems[0];
  }
  if (t->kind == TypeKind::Enum && t->enum_def == prog_.option_def) {
    if (ret->is_infer()) {
      unify(ret, option_of(ctx_.fresh_infer(InferKind::Any)));
      ret = shallow(fc.ret);
    }
    if (ret->kind != TypeKind::Enum || ret->enum_def != prog_.option_def) {
      error(e.span, "'?' on an Option needs the function to return an Option, but it returns '" + type_name(fc.ret) + "'");
    }
    return t->elems[0];
  }
  error(e.span, "'?' needs a Result or Option, found '" + type_name(t) + "'");
  return ctx_.error();
}

const Type* Checker::check_struct_lit(FnCtx& fc, const ast::StructLitExpr& e) {
  // Resolve the path: Struct, Self, or Enum.Variant.
  StructDef* s = nullptr;
  const VariantDef* variant = nullptr;
  std::vector<const Type*> args;
  const std::vector<FieldDef>* fields = nullptr;
  const std::vector<GenericParamDef>* generics = nullptr;
  const Type* result = nullptr;

  EnumDef* en = nullptr;
  if (e.path.segments.size() == 1 && e.path.segments[0].name == "Self") {
    auto* self = fc.type_scope.find_self();
    if (self && shallow(self)->kind == TypeKind::Struct) { s = shallow(self)->struct_def; args = shallow(self)->elems; }
  } else {
    auto* symbol = resolver_.lookup_path(e.path);
    if (symbol && symbol->kind == SymbolKind::Struct) s = symbol->struct_def;
    if (symbol && symbol->kind == SymbolKind::Variant) { variant = symbol->variant; en = variant->owner; }
    if (!s && !variant && e.path.segments.size() > 1) {
      auto* owner = resolver_.lookup_path(e.path, e.path.segments.size() - 1);
      if (owner && owner->kind == SymbolKind::Enum) {
        en = owner->enum_def;
        variant = en->find_variant(e.path.segments.back().name);
      }
    }
  }
  if (s) {
    if (args.empty()) for (auto& g : s->generics) { (void)g; args.push_back(ctx_.fresh_infer(InferKind::Any)); }
    fields = &s->fields; generics = &s->generics; result = ctx_.struct_type(s, args);
  } else if (variant && variant->kind == ast::VariantKind::Struct) {
    for (auto& g : en->generics) { (void)g; args.push_back(ctx_.fresh_infer(InferKind::Any)); }
    fields = &variant->fields; generics = &en->generics; result = ctx_.enum_type(en, args);
  } else {
    error(e.path.span, "unknown struct or struct variant '" + e.path.to_string() + "'");
    for (auto& field : e.fields) if (field.value) check_expr(fc, *field.value, nullptr);
    return ctx_.error();
  }

  Subst sub;
  for (size_t i = 0; i < generics->size(); ++i) sub.bind((*generics)[i].type, args[i]);

  std::set<std::string> seen;
  for (auto& init : e.fields) {
    if (seen.count(init.name)) {
      error(init.span, "field '" + init.name + "' is given twice");
      if (init.value) check_expr(fc, *init.value, nullptr);
      continue;
    }
    seen.insert(init.name);
    const FieldDef* f = nullptr;
    for (auto& fd : *fields) if (fd.name == init.name) f = &fd;
    if (!f) {
      error(init.span, "no field '" + init.name + "' in '" + e.path.to_string() + "'");
      if (init.value) check_expr(fc, *init.value, nullptr);
      continue;
    }
    if (s && !accessible(s->module, f->is_pub, init.span, f->name)) continue;
    const Type* want = sub.apply(ctx_, f->type);
    if (init.value) {
      const Type* got = check_expr(fc, *init.value, want);
      coerce(*init.value, got, want, "struct field");
    } else {
      // Shorthand: a local with the same name.
      uint32_t id;
      const LocalVar* var;
      if (!lookup_local(fc, init.name, id, &var)) {
        error(init.span, "no variable '" + init.name + "' for field shorthand");
        continue;
      }
      unify_or_error(var->type, want, init.span, "struct field");
      result_.shorthand_locals[&init] = id;
    }
  }
  for (auto& fd : *fields) {
    if (!seen.count(fd.name)) error(e.span, "missing field '" + fd.name + "' in '" + e.path.to_string() + "'", "SNX0406");
  }
  NameRef ref;
  ref.kind = s ? RefKind::TypeName : RefKind::Variant;
  ref.struct_def = s;
  ref.variant = variant;
  ref.enum_def = variant ? variant->owner : nullptr;
  ref.owner_args = args;
  result_.refs[&e] = ref;
  return result;
}

const Type* Checker::check_if(FnCtx& fc, const ast::IfExpr& e, const Type* expected) {
  const Type* c = check_expr(fc, *e.cond, ctx_.bool_type());
  unify_or_error(c, ctx_.bool_type(), e.cond->span, "if condition");
  if (!e.else_expr) {
    const Type* t = check_block(fc, *e.then_block, ctx_.void_type());
    if (e.then_block->tail && !shallow(t)->is_void() && !shallow(t)->is_never()) {
      error(e.then_block->tail->span, "an if without else cannot produce a value; add an else branch");
    }
    return ctx_.void_type();
  }
  const Type* t = check_block(fc, *e.then_block, expected);
  const Type* f = check_expr(fc, *e.else_expr, expected ? expected : t);
  const Type* ts = shallow(t);
  const Type* fs = shallow(f);
  if (ts->is_never()) return f;
  if (fs->is_never()) return t;
  if (!unify(t, f)) {
    error(e.span, "if branches have different types: '" + type_name(t) + "' and '" + type_name(f) + "'");
    return ctx_.error();
  }
  return t;
}

const Type* Checker::check_match(FnCtx& fc, const ast::MatchExpr& e, const Type* expected) {
  const Type* scrut = check_expr(fc, *e.scrutinee, nullptr);
  const Type* result = nullptr;
  bool all_never = true;
  for (auto& arm : e.arms) {
    push_scope(fc);
    check_pattern(fc, *arm.pattern, scrut, 0);
    if (arm.guard) {
      const Type* g = check_expr(fc, *arm.guard, ctx_.bool_type());
      unify_or_error(g, ctx_.bool_type(), arm.guard->span, "match guard");
    }
    const Type* want = expected ? expected : result;
    const Type* t = check_expr(fc, *arm.body, want);
    pop_scope(fc);
    if (shallow(t)->is_never()) continue;
    all_never = false;
    if (!result) {
      result = t;
    } else if (!unify(result, t)) {
      error(arm.body->span, "match arms have different types: '" + type_name(result) + "' and '" + type_name(t) + "'");
    }
  }
  check_exhaustive(e, scrut);
  if (all_never) return e.arms.empty() ? ctx_.void_type() : ctx_.never();
  return result ? result : ctx_.void_type();
}

const Type* Checker::check_closure(FnCtx& fc, const ast::ClosureExpr& e, const Type* expected) {
  const Type* x = expected ? shallow(expected) : nullptr;
  if (x && x->kind != TypeKind::Fn) x = nullptr;
  FnCtx cc;
  cc.parent = &fc;
  cc.closure = &e;
  cc.local_start = static_cast<uint32_t>(fc.locals->size());
  cc.fn = nullptr;
  cc.type_scope = fc.type_scope;
  cc.locals = fc.locals;
  cc.unsafe_depth = fc.unsafe_depth;
  std::vector<const Type*> params;
  push_scope(cc);
  std::vector<uint32_t> ids;
  for (size_t i = 0; i < e.params.size(); ++i) {
    const Type* t;
    if (e.params[i].type) t = resolver_.resolve_type(*e.params[i].type, fc.type_scope);
    else if (x && i < x->elems.size()) t = x->elems[i];
    else t = ctx_.fresh_infer(InferKind::Any);
    params.push_back(t);
    ids.push_back(declare_local(cc, e.params[i].name, t, false, e.params[i].span));
  }
  for (const auto& capture : e.captures) {
    uint32_t id;
    const LocalVar* variable;
    if (!lookup_local(fc, capture.name, id, &variable)) {
      error(capture.span, "unknown captured variable '" + capture.name + "'", "SNX0401");
      continue;
    }
    auto& modes = result_.closure_capture_modes[&e];
    if (!modes.emplace(id, capture.mode).second) error(capture.span, "duplicate capture '" + capture.name + "'", "SNX0408");
    else result_.closure_captures[&e].push_back(id);
    if (capture.mode == ast::CaptureMode::Writable && variable->is_const)
      error(capture.span, "cannot borrow a read only captured variable as writable", "SNX0408");
  }
  result_.closure_params[&e] = ids;
  cc.ret = e.ret ? resolver_.resolve_type(*e.ret, fc.type_scope) : (x ? x->ret : ctx_.fresh_infer(InferKind::Any));
  cc.ret_inferred = !e.ret;
  const Type* body = check_expr(cc, *e.body, cc.ret);
  if (!shallow(body)->is_never()) coerce(*e.body, body, cc.ret, "closure body");
  pop_scope(cc);
  result_.closure_locals[&e] = *cc.locals;
  if (e.is_send || (x && x->is_send)) {
    for (auto id : result_.closure_captures[&e]) {
      const Type* captured = (*fc.locals)[id].type;
      auto mode = result_.closure_capture_modes[&e].find(id);
      if (mode != result_.closure_capture_modes[&e].end() && mode->second != ast::CaptureMode::Move)
        captured = ctx_.ref(captured, mode->second == ast::CaptureMode::Shared);
      for (const char* name : {"Send", "Owned"})
        add_obligation(captured, TraitRef{prog_.core_trait(name), {}}, e.span, "send closure capture " + (*fc.locals)[id].name);
    }
  }
  return ctx_.fn(std::move(params), cc.ret, e.call_mode, e.is_send || (x && x->is_send));
}

const Type* Checker::check_interp(FnCtx& fc, const ast::InterpExpr& e) {
  TraitDef* display = prog_.core_trait("Display");
  for (auto& p : e.parts) {
    if (!p.expr) continue;
    const Type* t = check_expr(fc, *p.expr, nullptr);
    if (display) {
      TraitRef r;
      r.trait = display;
      add_obligation(t, r, p.expr->span, "value in a string");
    }
  }
  return string_type();
}

const Type* Checker::check_range(FnCtx& fc, const ast::RangeExpr& e, const Type* expected) {
  if (!e.start || !e.end) {
    error(e.span, "open ranges are not supported yet; write both ends");
    if (e.start) check_expr(fc, *e.start, nullptr);
    if (e.end) check_expr(fc, *e.end, nullptr);
    return ctx_.error();
  }
  const Type* hint = nullptr;
  if (expected) {
    const Type* x = shallow(expected);
    if (x->kind == TypeKind::Struct && !x->elems.empty() &&
        (x->struct_def == prog_.range_def || x->struct_def == prog_.range_inclusive_def)) {
      hint = x->elems[0];
    }
  }
  const Type* a = check_expr(fc, *e.start, hint);
  const Type* b = check_expr(fc, *e.end, hint ? hint : a);
  unify_or_error(b, a, e.end->span, "range bounds");
  StructDef* def = e.inclusive ? prog_.range_inclusive_def : prog_.range_def;
  if (!def) return ctx_.error();
  return ctx_.struct_type(def, {a});
}

// ---------- patterns ----------

const VariantDef* Checker::resolve_variant_path(const ast::Path& path, const Type* scrutinee,
                                                std::vector<const Type*>& args, Span span) {
  EnumDef* en = nullptr;
  const VariantDef* v = nullptr;
  auto* symbol = resolver_.lookup_path(path);
  if (symbol && symbol->kind == SymbolKind::Variant) { v = symbol->variant; en = v->owner; }
  if (!v && path.segments.size() > 1) {
    auto* owner = resolver_.lookup_path(path, path.segments.size() - 1);
    if (owner && owner->kind == SymbolKind::Enum) { en = owner->enum_def; v = en->find_variant(path.segments.back().name); }
  }
  if (!v && path.segments.size() == 1) {
    const Type* base = strip_refs(scrutinee, nullptr);
    if (base->kind == TypeKind::Enum) { en = base->enum_def; v = en->find_variant(path.segments[0].name); }
  }
  if (!v) {
    error(span, "unknown variant '" + path.to_string() + "'");
    return nullptr;
  }
  for (auto& g : en->generics) { (void)g; args.push_back(ctx_.fresh_infer(InferKind::Any)); }
  return v;
}

// ref_mode: 0 by value, 1 behind &const, 2 behind &.
void Checker::check_pattern(FnCtx& fc, const ast::Pattern& p, const Type* type, int ref_mode) {
  const Type* t = shallow(type);
  auto deref_for_structure = [&]() {
    int n = 0;
    while (t->kind == TypeKind::Ref) {
      ref_mode = (ref_mode == 1 || t->is_const) ? 1 : 2;
      t = shallow(t->inner);
      ++n;
    }
    result_.pattern_derefs[&p] = n;
  };
  auto bind_type = [&](const Type* inner) {
    if (ref_mode == 0) return inner;
    return ctx_.ref(inner, ref_mode == 1);
  };

  switch (p.kind) {
    case ast::PatternKind::Wildcard:
      return;
    case ast::PatternKind::Binding: {
      auto& b = static_cast<const ast::BindingPattern&>(p);
      // A bare name that is a unit variant of the matched enum, or a prelude
      // variant like None, matches that variant.
      const Symbol* sym = prog_.find(b.name);
      const Type* base = strip_refs(t, nullptr);
      bool is_variant = false;
      if (sym && sym->kind == SymbolKind::Variant && sym->variant->kind == ast::VariantKind::Unit) is_variant = true;
      if (!is_variant && base->kind == TypeKind::Enum) {
        const VariantDef* v = base->enum_def->find_variant(b.name);
        if (v && v->kind == ast::VariantKind::Unit) is_variant = true;
      }
      if (is_variant) {
        deref_for_structure();
        std::vector<const Type*> args;
        ast::Path path;
        path.segments.push_back({b.name, b.span});
        path.span = b.span;
        const VariantDef* v = resolve_variant_path(path, t, args, b.span);
        if (v) unify_or_error(ctx_.enum_type(v->owner, args), t, b.span, "pattern");
        return;
      }
      uint32_t id = declare_local(fc, b.name, bind_type(t), false, b.span);
      result_.pattern_locals[&p] = id;
      return;
    }
    case ast::PatternKind::Literal: {
      deref_for_structure();
      auto& l = static_cast<const ast::LiteralPattern&>(p);
      const Type* lt = check_expr(fc, *l.value, t);
      // A string literal pattern matches a str behind any reference.
      if (shallow(lt)->kind == TypeKind::Ref && t->kind == TypeKind::Str) return;
      unify_or_error(lt, t, p.span, "pattern");
      return;
    }
    case ast::PatternKind::Range: {
      deref_for_structure();
      auto& r = static_cast<const ast::RangePattern&>(p);
      const Type* lo = check_expr(fc, *r.lo, t);
      const Type* hi = check_expr(fc, *r.hi, t);
      unify_or_error(lo, t, r.lo->span, "pattern");
      unify_or_error(hi, t, r.hi->span, "pattern");
      return;
    }
    case ast::PatternKind::Path: {
      deref_for_structure();
      auto& pp = static_cast<const ast::PathPattern&>(p);
      std::vector<const Type*> args;
      const VariantDef* v = resolve_variant_path(pp.path, t, args, p.span);
      if (!v) return;
      if (v->kind != ast::VariantKind::Unit) {
        error(p.span, "variant '" + pp.path.to_string() + "' carries data; match its fields");
      }
      unify_or_error(ctx_.enum_type(v->owner, args), t, p.span, "pattern");
      return;
    }
    case ast::PatternKind::TupleStruct: {
      deref_for_structure();
      auto& ts = static_cast<const ast::TupleStructPattern&>(p);
      std::vector<const Type*> args;
      const VariantDef* v = resolve_variant_path(ts.path, t, args, p.span);
      if (!v) {
        for (auto& el : ts.elems) check_pattern(fc, *el, ctx_.error(), ref_mode);
        return;
      }
      unify_or_error(ctx_.enum_type(v->owner, args), t, p.span, "pattern");
      if (v->kind != ast::VariantKind::Tuple) {
        error(p.span, "variant '" + ts.path.to_string() + "' does not have tuple fields");
        return;
      }
      if (ts.elems.size() != v->tuple_types.size()) {
        error(p.span, "variant '" + ts.path.to_string() + "' has " + std::to_string(v->tuple_types.size()) +
                          " fields, pattern has " + std::to_string(ts.elems.size()));
      }
      Subst s;
      for (size_t i = 0; i < v->owner->generics.size(); ++i) s.bind(v->owner->generics[i].type, args[i]);
      for (size_t i = 0; i < ts.elems.size() && i < v->tuple_types.size(); ++i) {
        check_pattern(fc, *ts.elems[i], s.apply(ctx_, v->tuple_types[i]), ref_mode);
      }
      return;
    }
    case ast::PatternKind::Struct: {
      deref_for_structure();
      auto& sp = static_cast<const ast::StructPattern&>(p);
      const std::vector<FieldDef>* fields = nullptr;
      ModuleDef* field_module = nullptr;
      Subst s;
      {
        const Symbol* sym = resolver_.lookup_path(sp.path);
        if (sym && sym->kind == SymbolKind::Struct) {
          std::vector<const Type*> args;
          for (auto& g : sym->struct_def->generics) { (void)g; args.push_back(ctx_.fresh_infer(InferKind::Any)); }
          unify_or_error(ctx_.struct_type(sym->struct_def, args), t, p.span, "pattern");
          for (size_t i = 0; i < args.size(); ++i) s.bind(sym->struct_def->generics[i].type, args[i]);
          fields = &sym->struct_def->fields;
          field_module = sym->struct_def->module;
        }
      }
      if (!fields) {
        std::vector<const Type*> args;
        const VariantDef* v = resolve_variant_path(sp.path, t, args, p.span);
        if (!v) return;
        if (v->kind != ast::VariantKind::Struct) {
          error(p.span, "'" + sp.path.to_string() + "' does not have named fields");
          return;
        }
        unify_or_error(ctx_.enum_type(v->owner, args), t, p.span, "pattern");
        for (size_t i = 0; i < args.size(); ++i) s.bind(v->owner->generics[i].type, args[i]);
        fields = &v->fields;
      }
      for (auto& fp : sp.fields) {
        const FieldDef* f = nullptr;
        for (auto& fd : *fields) if (fd.name == fp.name) f = &fd;
        if (!f) {
          error(fp.span, "no field '" + fp.name + "' in '" + sp.path.to_string() + "'");
          continue;
        }
        if (!accessible(field_module, f->is_pub, fp.span, f->name)) continue;
        const Type* ft = s.apply(ctx_, f->type);
        if (fp.pattern) {
          check_pattern(fc, *fp.pattern, ft, ref_mode);
        } else {
          declare_local(fc, fp.name, bind_type(ft), false, fp.span);
        }
      }
      return;
    }
    case ast::PatternKind::Tuple: {
      deref_for_structure();
      auto& tp = static_cast<const ast::TuplePattern&>(p);
      if (tp.elems.empty()) {
        unify_or_error(ctx_.void_type(), t, p.span, "pattern");
        return;
      }
      std::vector<const Type*> elems;
      for (size_t i = 0; i < tp.elems.size(); ++i) elems.push_back(ctx_.fresh_infer(InferKind::Any));
      if (!unify(ctx_.tuple(elems), t)) {
        error(p.span, "tuple pattern with " + std::to_string(tp.elems.size()) + " elements does not match '" + type_name(t) + "'");
        for (auto& el : tp.elems) check_pattern(fc, *el, ctx_.error(), ref_mode);
        return;
      }
      for (size_t i = 0; i < tp.elems.size(); ++i) check_pattern(fc, *tp.elems[i], elems[i], ref_mode);
      return;
    }
    case ast::PatternKind::Or: {
      auto& o = static_cast<const ast::OrPattern&>(p);
      for (auto& alt : o.alts) {
        size_t before = fc.locals->size();
        check_pattern(fc, *alt, type, ref_mode);
        if (fc.locals->size() != before) {
          error(alt->span, "alternatives of an or-pattern cannot bind variables yet");
        }
      }
      return;
    }
  }
}

bool Checker::pattern_is_irrefutable(const ast::Pattern& p) {
  switch (p.kind) {
    case ast::PatternKind::Wildcard: return true;
    case ast::PatternKind::Binding: {
      auto& b = static_cast<const ast::BindingPattern&>(p);
      return result_.pattern_locals.count(&p) > 0 || b.name == "_";
    }
    case ast::PatternKind::Tuple:
      for (auto& e : static_cast<const ast::TuplePattern&>(p).elems) if (!pattern_is_irrefutable(*e)) return false;
      return true;
    case ast::PatternKind::Struct: {
      auto& s = static_cast<const ast::StructPattern&>(p);
      const Symbol* sym = resolver_.lookup_path(s.path);
      if (!sym || sym->kind != SymbolKind::Struct) return false;
      for (auto& f : s.fields) if (f.pattern && !pattern_is_irrefutable(*f.pattern)) return false;
      return true;
    }
    default: return false;
  }
}

namespace {

// Flattens or-patterns into their alternatives.
void flatten(const ast::Pattern& p, std::vector<const ast::Pattern*>& out) {
  if (p.kind == ast::PatternKind::Or) {
    for (auto& a : static_cast<const ast::OrPattern&>(p).alts) flatten(*a, out);
  } else {
    out.push_back(&p);
  }
}

}  // namespace

// True when `pats` together cover every value of type `t`. `missing` gets a
// short description of one uncovered case. The check is exact for enums,
// bools, and tuples of those, and conservative elsewhere.
bool Checker::covers(const Type* t, const std::vector<const ast::Pattern*>& pats, std::string& missing) {
  for (auto* p : pats) {
    if (pattern_is_irrefutable(*p)) return true;
  }
  t = strip_refs(zonk(t), nullptr);
  if (t->is_error()) return true;

  if (t->kind == TypeKind::Enum) {
    Subst sub;
    for (size_t i = 0; i < t->enum_def->generics.size(); ++i) sub.bind(t->enum_def->generics[i].type, t->elems[i]);
    for (auto& v : t->enum_def->variants) {
      // Collect the sub-patterns of every arm that names this variant.
      bool named = false;
      std::vector<std::vector<const ast::Pattern*>> per_field(v.tuple_types.size() + v.fields.size());
      bool any_all_irrefutable = false;
      for (auto* p : pats) {
        const ast::Path* path = nullptr;
        const std::vector<ast::PatternPtr>* elems = nullptr;
        const std::vector<ast::FieldPattern>* fields = nullptr;
        std::string bare;
        switch (p->kind) {
          case ast::PatternKind::Binding: bare = static_cast<const ast::BindingPattern&>(*p).name; break;
          case ast::PatternKind::Path: path = &static_cast<const ast::PathPattern&>(*p).path; break;
          case ast::PatternKind::TupleStruct:
            path = &static_cast<const ast::TupleStructPattern&>(*p).path;
            elems = &static_cast<const ast::TupleStructPattern&>(*p).elems;
            break;
          case ast::PatternKind::Struct:
            path = &static_cast<const ast::StructPattern&>(*p).path;
            fields = &static_cast<const ast::StructPattern&>(*p).fields;
            break;
          default: break;
        }
        std::string name = path ? path->segments.back().name : bare;
        if (name != v.name) continue;
        named = true;
        if (v.kind == ast::VariantKind::Unit) {
          any_all_irrefutable = true;
        } else if (elems) {
          bool all = true;
          for (size_t i = 0; i < elems->size() && i < per_field.size(); ++i) {
            per_field[i].push_back((*elems)[i].get());
            if (!pattern_is_irrefutable(*(*elems)[i])) all = false;
          }
          if (all) any_all_irrefutable = true;
        } else if (fields) {
          bool all = true;
          for (auto& fp : *fields) {
            size_t idx = 0;
            for (; idx < v.fields.size(); ++idx) if (v.fields[idx].name == fp.name) break;
            if (idx == v.fields.size()) continue;
            if (fp.pattern) {
              per_field[idx].push_back(fp.pattern.get());
              if (!pattern_is_irrefutable(*fp.pattern)) all = false;
            }
          }
          if (all) any_all_irrefutable = true;
        }
      }
      if (!named) {
        missing = v.name;
        return false;
      }
      if (any_all_irrefutable) continue;
      // Exact for a single payload field: the sub-patterns must cover it.
      if (v.kind == ast::VariantKind::Tuple && v.tuple_types.size() == 1) {
        std::string inner;
        if (!covers(sub.apply(ctx_, v.tuple_types[0]), per_field[0], inner)) {
          missing = v.name + "(" + inner + ")";
          return false;
        }
        continue;
      }
      // Several fields: conservative. Each field must be covered on its own
      // by some arm; this accepts a few combinations it should reject, which
      // is safe because the runtime check at the end of a match still panics.
      for (size_t i = 0; i < per_field.size(); ++i) {
        const Type* ft = i < v.tuple_types.size() ? v.tuple_types[i] : v.fields[i - v.tuple_types.size()].type;
        std::string inner;
        if (!covers(sub.apply(ctx_, ft), per_field[i], inner)) {
          missing = v.name + "(.., " + inner + ", ..)";
          return false;
        }
      }
    }
    return true;
  }

  if (t->is_bool()) {
    bool has_true = false, has_false = false;
    for (auto* p : pats) {
      if (p->kind != ast::PatternKind::Literal) continue;
      auto& v = *static_cast<const ast::LiteralPattern&>(*p).value;
      if (v.kind == ExprKind::BoolLit) (static_cast<const ast::BoolLitExpr&>(v).value ? has_true : has_false) = true;
    }
    if (has_true && has_false) return true;
    missing = has_true ? "false" : "true";
    return false;
  }

  if (t->kind == TypeKind::Tuple) {
    // Each position must be covered by the sub-patterns at that position.
    for (size_t i = 0; i < t->elems.size(); ++i) {
      std::vector<const ast::Pattern*> subs;
      for (auto* p : pats) {
        if (p->kind != ast::PatternKind::Tuple) continue;
        auto& tp = static_cast<const ast::TuplePattern&>(*p);
        if (i < tp.elems.size()) subs.push_back(tp.elems[i].get());
      }
      std::string inner;
      if (!covers(t->elems[i], subs, inner)) {
        missing = "(.., " + inner + ", ..)";
        return false;
      }
    }
    return true;
  }

  missing = "_";
  return false;
}

void Checker::check_exhaustive(const ast::MatchExpr& m, const Type* scrutinee) {
  std::vector<const ast::Pattern*> pats;
  for (auto& arm : m.arms) {
    if (arm.guard) continue;  // a guarded arm covers nothing for certain
    flatten(*arm.pattern, pats);
  }
  std::string missing;
  if (covers(scrutinee, pats, missing)) return;
  if (missing == "_") error(m.span, "match is not exhaustive; add a '_' arm", "SNX0404");
  else error(m.span, "match is not exhaustive; missing: " + missing, "SNX0404");
}

// ---------- traits ----------

const std::vector<TraitRef>* Checker::bounds_of(const Type* param) {
  auto it = param_bounds_.find(param);
  return it == param_bounds_.end() ? nullptr : it->second;
}

bool Checker::builtin_implements(const Type* t, const std::string& name) {
  t = shallow(t);
  bool numeric = t->is_numeric();
  bool is_string = t->kind == TypeKind::Struct && t->struct_def == prog_.string_def;
  if (name == "Copy") return is_copy(t);
  if (name == "Display") {
    if (t->kind == TypeKind::Prim && !t->is_void()) return true;
    if (t->kind == TypeKind::Str || is_string) return true;
    if (t->kind == TypeKind::Ref) return builtin_implements(t->inner, name) || implements(t->inner, TraitRef{prog_.core_trait("Display"), {}});
    return t->is_infer() && t->infer_kind != InferKind::Any;
  }
  if (name == "Eq" || name == "Ord") {
    if (numeric || t->is_prim(PrimKind::Char) || t->kind == TypeKind::Str || is_string) return true;
    if (name == "Eq" && t->is_bool()) return true;
    if (t->kind == TypeKind::Ref) {
      const Type* inner = shallow(t->inner);
      if (inner->is_infer() && inner->infer_kind == InferKind::Any) return false;
      return implements(inner, TraitRef{prog_.core_trait(name), {}});
    }
    return false;
  }
  if (name == "Add" || name == "Sub" || name == "Mul" || name == "Div" || name == "Rem") return numeric;
  if (name == "Neg") return t->is_float() || t->is_signed_integer() || (t->is_infer() && t->infer_kind != InferKind::Any);
  if (name == "Not") return t->is_bool() || t->is_integer();
  return false;
}

ImplDef* Checker::find_impl(const Type* t, TraitDef* trait, Subst* subst_out) {
  for (auto& impl : prog_.impls) {
    if (impl->trait.trait != trait) continue;
    Subst s;
    for (auto& g : impl->generics) s.bind(g.type, ctx_.fresh_infer(InferKind::Any));
    size_t mark = snapshot();
    if (unify(s.apply(ctx_, impl->self_type), t)) {
      bool sized = true;
      for (auto& g : impl->generics) if (shallow(s.apply(ctx_, g.type))->is_unsized()) sized = false;
      if (sized) {
        if (subst_out) *subst_out = s;
        return impl.get();
      }
    }
    rollback(mark);
  }
  return nullptr;
}

bool Checker::implements(const Type* t, const TraitRef& trait) {
  if (!trait.trait) return true;
  t = shallow(t);
  if (t->is_error() || t->is_never()) return true;
  std::vector<const Type*> args;
  for (auto* arg : trait.args) args.push_back(zonk(arg));
  auto key = std::make_pair(zonk(t), ctx_.dyn_type(trait.trait, std::move(args)));
  bool auto_marker = trait.trait == prog_.core_trait("Send") || trait.trait == prog_.core_trait("Sync") || trait.trait == prog_.core_trait("Owned");
  if (checking_traits_.size() >= 128) return false;
  if (std::find(checking_traits_.begin(), checking_traits_.end(), key) != checking_traits_.end()) return auto_marker;
  checking_traits_.push_back(key);
  struct Guard {
    std::vector<std::pair<const Type*, const Type*>>& stack;
    ~Guard() { stack.pop_back(); }
  } guard{checking_traits_};
  if (t->kind == TypeKind::Param) {
    const std::vector<TraitRef>* bounds = bounds_of(t);
    if (bounds) {
      for (auto& b : *bounds) {
        if (b.trait != trait.trait || b.args.size() != trait.args.size()) continue;
        size_t mark = snapshot();
        bool match = true;
        for (size_t i = 0; match && i < b.args.size(); ++i) match = unify(b.args[i], trait.args[i]);
        if (match) return true;
        rollback(mark);
      }
    }
    return false;
  }
  if (t->kind == TypeKind::Dyn) {
    if (t->trait_def != trait.trait || t->elems.size() != trait.args.size()) return false;
    for (size_t i = 0; i < t->elems.size(); ++i) if (!try_unify(t->elems[i], trait.args[i])) return false;
    return true;
  }
  if (auto_marker) {
    const auto& name = trait.trait->name;
    auto holds = [&](const Type* field, const std::string& which) { return implements(field, TraitRef{prog_.core_trait(which), {}}); };
    bool structural = false;
    if (t->kind == TypeKind::Prim || t->kind == TypeKind::Str) structural = true;
    else if (t->kind == TypeKind::Fn) structural = name != "Sync" && t->is_send;
    else if (t->kind == TypeKind::Ref) structural = (frame_refs_ && name == "Send") || (name != "Owned" && holds(t->inner, name == "Send" && t->is_const ? "Sync" : name));
    else if (t->kind == TypeKind::Array || t->kind == TypeKind::Slice) structural = holds(t->inner, name);
    else if (t->kind == TypeKind::Tuple) {
      structural = true; for (auto* field : t->elems) structural = holds(field, name) && structural;
    } else if (t->kind == TypeKind::Struct || t->kind == TypeKind::Enum) {
      Subst subst;
      const auto& gs = t->kind == TypeKind::Struct ? t->struct_def->generics : t->enum_def->generics;
      for (size_t i = 0; i < gs.size() && i < t->elems.size(); ++i) subst.bind(gs[i].type, t->elems[i]);
      structural = true;
      if (t->kind == TypeKind::Struct) {
        for (auto& f : t->struct_def->fields) structural = holds(subst.apply(ctx_, f.type), name) && structural;
      } else for (auto& v : t->enum_def->variants) {
        for (auto* f : v.tuple_types) structural = holds(subst.apply(ctx_, f), name) && structural;
        for (auto& f : v.fields) structural = holds(subst.apply(ctx_, f.type), name) && structural;
      }
    }
    if (structural) return true;
  }
  if (!auto_marker && builtin_implements(t, trait.trait->name)) return true;
  for (auto& impl : prog_.impls) {
    if (impl->trait.trait != trait.trait || impl->trait.args.size() != trait.args.size()) continue;
    Subst s;
    for (auto& g : impl->generics) s.bind(g.type, ctx_.fresh_infer(InferKind::Any));
    size_t mark = snapshot();
    bool match = unify(s.apply(ctx_, impl->self_type), t);
    for (size_t i = 0; match && i < trait.args.size(); ++i)
      match = unify(s.apply(ctx_, impl->trait.args[i]), trait.args[i]);
    for (auto& g : impl->generics) {
      if (match && shallow(s.apply(ctx_, g.type))->is_unsized()) match = false;
      for (auto& b : g.bounds) {
        TraitRef bound{b.trait, {}};
        for (auto* a : b.args) bound.args.push_back(s.apply(ctx_, a));
        if (match && !implements(s.apply(ctx_, g.type), bound)) match = false;
      }
    }
    if (match) return true;
    rollback(mark);
  }
  return false;
}

void Checker::add_obligation(const Type* t, const TraitRef& trait, Span span, const std::string& what) {
  if (!trait.trait) return;
  obligations_.push_back({t, trait, span, what});
}

void Checker::check_obligations() {
  auto obligations = std::move(obligations_);
  obligations_.clear();
  for (auto& o : obligations) {
    const Type* t = zonk(o.type);
    if (t->contains_infer()) continue;  // already reported as unknown
    if (!implements(t, o.trait)) {
      error(o.span, "the type '" + t->to_string() + "' does not implement '" + o.trait.trait->name + "', required for " + o.what);
    }
  }
}

}  // namespace synx::sema
