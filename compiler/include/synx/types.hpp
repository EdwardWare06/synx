// Semantic types. Every distinct type exists exactly once inside a
// TypeContext, so two types are equal when their pointers are equal.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "synx/ast.hpp"

namespace synx::sema {

struct StructDef;
struct EnumDef;
struct TraitDef;

enum class TypeKind {
  Prim,    // bool, int, double, void, ...
  Str,     // the unsized text type, always behind a reference
  Dyn,     // an unsized trait object, always behind a reference
  Struct,  // a struct with its type arguments
  Enum,    // an enum with its type arguments
  Ref,     // &T or &const T
  Ptr,     // *T or *const T
  Array,   // T[N]
  Slice,   // T[], unsized, always behind a reference
  Tuple,   // (A, B), never empty: () is void
  Fn,      // fn(A, B) -> R
  Param,   // a generic type parameter inside its owner
  Infer,   // an inference variable, only during checking
  Never,   // the type of return, break, panic
  Error    // a type that failed to resolve; suppresses follow-on errors
};

enum class InferKind { Any, Int, Float };

struct Type {
  TypeKind kind;
  ast::PrimKind prim = ast::PrimKind::Void;
  bool is_const = false;               // Ref, Ptr
  const Type* inner = nullptr;         // Ref, Ptr, Array, Slice
  uint64_t length = 0;                 // Array
  std::vector<const Type*> elems;      // Tuple elems, Fn params, Struct/Enum args
  const Type* ret = nullptr;           // Fn
  ast::CallMode call_mode = ast::CallMode::Writable;
  bool is_send = false;
  StructDef* struct_def = nullptr;     // Struct
  EnumDef* enum_def = nullptr;         // Enum
  TraitDef* trait_def = nullptr;       // Dyn; elems holds the trait arguments
  std::string name;                    // Param
  const void* owner = nullptr;         // Param: the def that declares it
  uint32_t index = 0;                  // Param index, Infer id
  InferKind infer_kind = InferKind::Any;

  bool is_prim(ast::PrimKind p) const { return kind == TypeKind::Prim && prim == p; }
  bool is_void() const { return is_prim(ast::PrimKind::Void); }
  bool is_bool() const { return is_prim(ast::PrimKind::Bool); }
  bool is_integer() const;
  bool is_signed_integer() const;
  bool is_float() const;
  bool is_numeric() const { return is_integer() || is_float(); }
  bool is_ref() const { return kind == TypeKind::Ref; }
  bool is_error() const { return kind == TypeKind::Error; }
  bool is_never() const { return kind == TypeKind::Never; }
  bool is_infer() const { return kind == TypeKind::Infer; }
  // True when the type is Str or Slice and must live behind a reference.
  bool is_unsized() const { return kind == TypeKind::Str || kind == TypeKind::Slice || kind == TypeKind::Dyn; }
  bool contains_infer() const;
  bool contains_param() const;

  std::string to_string() const;
};

class TypeContext {
 public:
  TypeContext();

  const Type* prim(ast::PrimKind p) const { return prims_[static_cast<size_t>(p)]; }
  const Type* void_type() const { return prim(ast::PrimKind::Void); }
  const Type* bool_type() const { return prim(ast::PrimKind::Bool); }
  const Type* int_type() const { return prim(ast::PrimKind::Int); }
  const Type* usize_type() const { return prim(ast::PrimKind::Usize); }
  const Type* double_type() const { return prim(ast::PrimKind::Double); }
  const Type* str() const { return str_; }
  const Type* never() const { return never_; }
  const Type* error() const { return error_; }

  const Type* ref(const Type* inner, bool is_const);
  const Type* ptr(const Type* inner, bool is_const);
  const Type* array(const Type* elem, uint64_t length);
  const Type* slice(const Type* elem);
  const Type* tuple(std::vector<const Type*> elems);  // empty gives void
  const Type* fn(std::vector<const Type*> params, const Type* ret, ast::CallMode mode = ast::CallMode::Writable, bool is_send = false);
  const Type* struct_type(StructDef* def, std::vector<const Type*> args);
  const Type* enum_type(EnumDef* def, std::vector<const Type*> args);
  const Type* dyn_type(TraitDef* def, std::vector<const Type*> args);
  const Type* param(const void* owner, uint32_t index, const std::string& name);
  const Type* fresh_infer(InferKind kind);
  uint32_t infer_count() const { return next_infer_; }

 private:
  const Type* intern(const std::string& key, Type t);

  std::map<std::string, std::unique_ptr<Type>> table_;
  std::vector<std::unique_ptr<Type>> infer_vars_;
  const Type* prims_[12];
  const Type* str_;
  const Type* never_;
  const Type* error_;
  uint32_t next_infer_ = 0;
};

// Replaces generic parameters with concrete types.
class Subst {
 public:
  void bind(const Type* param, const Type* to) { map_[param] = to; }
  const Type* lookup(const Type* param) const {
    auto it = map_.find(param);
    return it == map_.end() ? nullptr : it->second;
  }
  bool empty() const { return map_.empty(); }
  const Type* apply(TypeContext& ctx, const Type* t) const;

 private:
  std::map<const Type*, const Type*> map_;
};

}  // namespace synx::sema
