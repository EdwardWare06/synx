#include "synx/types.hpp"

#include <sstream>

#include "synx/defs.hpp"

namespace synx::sema {

bool Type::is_integer() const {
  if (kind == TypeKind::Infer) return infer_kind == InferKind::Int;
  if (kind != TypeKind::Prim) return false;
  switch (prim) {
    case ast::PrimKind::Byte:
    case ast::PrimKind::Short:
    case ast::PrimKind::Int:
    case ast::PrimKind::Long:
    case ast::PrimKind::Uint:
    case ast::PrimKind::Ulong:
    case ast::PrimKind::Usize:
      return true;
    default:
      return false;
  }
}

bool Type::is_signed_integer() const {
  if (kind != TypeKind::Prim) return false;
  return prim == ast::PrimKind::Short || prim == ast::PrimKind::Int || prim == ast::PrimKind::Long;
}

bool Type::is_float() const {
  if (kind == TypeKind::Infer) return infer_kind == InferKind::Float;
  return kind == TypeKind::Prim && (prim == ast::PrimKind::Float || prim == ast::PrimKind::Double);
}

bool Type::contains_infer() const {
  if (kind == TypeKind::Infer) return true;
  if (inner && inner->contains_infer()) return true;
  if (ret && ret->contains_infer()) return true;
  for (auto* e : elems) if (e->contains_infer()) return true;
  return false;
}

bool Type::contains_param() const {
  if (kind == TypeKind::Param) return true;
  if (inner && inner->contains_param()) return true;
  if (ret && ret->contains_param()) return true;
  for (auto* e : elems) if (e->contains_param()) return true;
  return false;
}

std::string Type::to_string() const {
  switch (kind) {
    case TypeKind::Prim: return ast::prim_kind_name(prim);
    case TypeKind::Str: return "str";
    case TypeKind::Struct:
    case TypeKind::Enum:
    case TypeKind::Dyn: {
      std::string s = kind == TypeKind::Dyn ? "dyn " + qualified_name(trait_def->module, trait_def->name) :
                      kind == TypeKind::Struct ? qualified_name(struct_def->module, struct_def->name) : qualified_name(enum_def->module, enum_def->name);
      if (!elems.empty()) {
        s += '<';
        for (size_t i = 0; i < elems.size(); ++i) {
          if (i) s += ", ";
          s += elems[i]->to_string();
        }
        s += '>';
      }
      return s;
    }
    case TypeKind::Ref: return std::string(is_const ? "&const " : "&") + inner->to_string();
    case TypeKind::Ptr: return std::string(is_const ? "*const " : "*") + inner->to_string();
    case TypeKind::Array: return inner->to_string() + "[" + std::to_string(length) + "]";
    case TypeKind::Slice: return inner->to_string() + "[]";
    case TypeKind::Tuple: {
      std::string s = "(";
      for (size_t i = 0; i < elems.size(); ++i) {
        if (i) s += ", ";
        s += elems[i]->to_string();
      }
      return s + ")";
    }
    case TypeKind::Fn: {
      std::string s = call_mode == ast::CallMode::Shared ? "fn const(" : call_mode == ast::CallMode::Once ? "fn once(" : "fn(";
      for (size_t i = 0; i < elems.size(); ++i) {
        if (i) s += ", ";
        s += elems[i]->to_string();
      }
      s += ")";
      if (ret && !ret->is_void()) s += " -> " + ret->to_string();
      if (is_send) s.insert(s.find('('), " send");
      return s;
    }
    case TypeKind::Param: return name;
    case TypeKind::Infer:
      if (infer_kind == InferKind::Int) return "{integer}";
      if (infer_kind == InferKind::Float) return "{float}";
      return "_";
    case TypeKind::Never: return "never";
    case TypeKind::Error: return "{error}";
  }
  return "?";
}

// ---------- TypeContext ----------

static std::string ptr_key(const void* p) {
  std::ostringstream s;
  s << p;
  return s.str();
}

TypeContext::TypeContext() {
  for (int i = 0; i < 12; ++i) {
    Type t;
    t.kind = TypeKind::Prim;
    t.prim = static_cast<ast::PrimKind>(i);
    prims_[i] = intern("prim:" + std::to_string(i), std::move(t));
  }
  Type s;
  s.kind = TypeKind::Str;
  str_ = intern("str", std::move(s));
  Type n;
  n.kind = TypeKind::Never;
  never_ = intern("never", std::move(n));
  Type e;
  e.kind = TypeKind::Error;
  error_ = intern("error", std::move(e));
}

const Type* TypeContext::intern(const std::string& key, Type t) {
  auto it = table_.find(key);
  if (it != table_.end()) return it->second.get();
  auto owned = std::make_unique<Type>(std::move(t));
  const Type* out = owned.get();
  table_[key] = std::move(owned);
  return out;
}

const Type* TypeContext::ref(const Type* inner, bool is_const) {
  Type t;
  t.kind = TypeKind::Ref;
  t.inner = inner;
  t.is_const = is_const;
  return intern(std::string("ref:") + (is_const ? "c:" : "m:") + ptr_key(inner), std::move(t));
}

const Type* TypeContext::ptr(const Type* inner, bool is_const) {
  Type t;
  t.kind = TypeKind::Ptr;
  t.inner = inner;
  t.is_const = is_const;
  return intern(std::string("ptr:") + (is_const ? "c:" : "m:") + ptr_key(inner), std::move(t));
}

const Type* TypeContext::array(const Type* elem, uint64_t length) {
  Type t;
  t.kind = TypeKind::Array;
  t.inner = elem;
  t.length = length;
  return intern("array:" + std::to_string(length) + ":" + ptr_key(elem), std::move(t));
}

const Type* TypeContext::slice(const Type* elem) {
  Type t;
  t.kind = TypeKind::Slice;
  t.inner = elem;
  return intern("slice:" + ptr_key(elem), std::move(t));
}

const Type* TypeContext::tuple(std::vector<const Type*> elems) {
  if (elems.empty()) return void_type();
  std::string key = "tuple:";
  for (auto* e : elems) key += ptr_key(e) + ",";
  Type t;
  t.kind = TypeKind::Tuple;
  t.elems = std::move(elems);
  return intern(key, std::move(t));
}

const Type* TypeContext::fn(std::vector<const Type*> params, const Type* ret, ast::CallMode mode, bool is_send) {
  std::string key = "fn:" + std::to_string(static_cast<int>(mode)) + (is_send ? ":send:" : ":local:");
  for (auto* p : params) key += ptr_key(p) + ",";
  key += "->" + ptr_key(ret);
  Type t;
  t.kind = TypeKind::Fn;
  t.call_mode = mode;
  t.is_send = is_send;
  t.elems = std::move(params);
  t.ret = ret;
  return intern(key, std::move(t));
}

const Type* TypeContext::struct_type(StructDef* def, std::vector<const Type*> args) {
  std::string key = "struct:" + ptr_key(def) + ":";
  for (auto* a : args) key += ptr_key(a) + ",";
  Type t;
  t.kind = TypeKind::Struct;
  t.struct_def = def;
  t.elems = std::move(args);
  return intern(key, std::move(t));
}

const Type* TypeContext::enum_type(EnumDef* def, std::vector<const Type*> args) {
  std::string key = "enum:" + ptr_key(def) + ":";
  for (auto* a : args) key += ptr_key(a) + ",";
  Type t;
  t.kind = TypeKind::Enum;
  t.enum_def = def;
  t.elems = std::move(args);
  return intern(key, std::move(t));
}

const Type* TypeContext::dyn_type(TraitDef* def, std::vector<const Type*> args) {
  std::string key = "dyn:" + ptr_key(def) + ":";
  for (auto* a : args) key += ptr_key(a) + ",";
  Type t;
  t.kind = TypeKind::Dyn;
  t.trait_def = def;
  t.elems = std::move(args);
  return intern(key, std::move(t));
}

const Type* TypeContext::param(const void* owner, uint32_t index, const std::string& name) {
  Type t;
  t.kind = TypeKind::Param;
  t.owner = owner;
  t.index = index;
  t.name = name;
  return intern("param:" + ptr_key(owner) + ":" + std::to_string(index), std::move(t));
}

const Type* TypeContext::fresh_infer(InferKind kind) {
  auto t = std::make_unique<Type>();
  t->kind = TypeKind::Infer;
  t->index = next_infer_++;
  t->infer_kind = kind;
  const Type* out = t.get();
  infer_vars_.push_back(std::move(t));
  return out;
}

// ---------- Subst ----------

const Type* Subst::apply(TypeContext& ctx, const Type* t) const {
  if (map_.empty() || !t) return t;
  switch (t->kind) {
    case TypeKind::Param: {
      const Type* to = lookup(t);
      return to ? to : t;
    }
    case TypeKind::Ref: return ctx.ref(apply(ctx, t->inner), t->is_const);
    case TypeKind::Ptr: return ctx.ptr(apply(ctx, t->inner), t->is_const);
    case TypeKind::Array: return ctx.array(apply(ctx, t->inner), t->length);
    case TypeKind::Slice: return ctx.slice(apply(ctx, t->inner));
    case TypeKind::Tuple: {
      std::vector<const Type*> elems;
      for (auto* e : t->elems) elems.push_back(apply(ctx, e));
      return ctx.tuple(std::move(elems));
    }
    case TypeKind::Fn: {
      std::vector<const Type*> params;
      for (auto* e : t->elems) params.push_back(apply(ctx, e));
      return ctx.fn(std::move(params), apply(ctx, t->ret), t->call_mode, t->is_send);
    }
    case TypeKind::Struct: {
      std::vector<const Type*> args;
      for (auto* e : t->elems) args.push_back(apply(ctx, e));
      return ctx.struct_type(t->struct_def, std::move(args));
    }
    case TypeKind::Dyn: {
      std::vector<const Type*> args;
      for (auto* e : t->elems) args.push_back(apply(ctx, e));
      return ctx.dyn_type(t->trait_def, std::move(args));
    }
    case TypeKind::Enum: {
      std::vector<const Type*> args;
      for (auto* e : t->elems) args.push_back(apply(ctx, e));
      return ctx.enum_type(t->enum_def, std::move(args));
    }
    default:
      return t;
  }
}

}  // namespace synx::sema
