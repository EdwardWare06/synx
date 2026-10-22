// Definitions: the semantic view of every item in a program. Built by the
// Resolver from the AST, used by the Checker and later by IR lowering.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "synx/ast.hpp"
#include "synx/types.hpp"

namespace synx::sema {

struct StructDef;
struct EnumDef;
struct TraitDef;
struct ImplDef;
struct FunctionDef;
struct ConstDef;
struct ModuleDef;

struct TraitRef {
  TraitDef* trait = nullptr;
  std::vector<const Type*> args;
};

struct GenericParamDef {
  std::string name;
  Span span;
  const Type* type = nullptr;  // the Param type
  std::vector<TraitRef> bounds;
};

struct ParamDef {
  std::string name;
  Span span;
  const Type* type = nullptr;
  ast::SelfKind self_kind = ast::SelfKind::None;
};

// Where a function lives. Methods know their owner so Self resolves.
enum class FnOwner { Module, Struct, Enum, Trait, Impl, Extern };

struct FunctionDef {
  ModuleDef* module = nullptr;
  std::string name;
  Span span;
  const ast::Function* ast = nullptr;
  bool is_pub = false;
  bool is_builtin = false;           // provided by the compiler, no body
  bool is_prelude = false;           // declared by the prelude source
  FnOwner owner = FnOwner::Module;
  StructDef* owner_struct = nullptr;
  EnumDef* owner_enum = nullptr;
  TraitDef* owner_trait = nullptr;
  ImplDef* owner_impl = nullptr;
  const Type* self_type = nullptr;   // the type Self names inside the body
  std::vector<GenericParamDef> generics;
  std::vector<ParamDef> params;      // includes self as params[0] for methods
  const Type* ret = nullptr;         // null until inferred
  bool ret_declared = false;
  bool has_self() const { return !params.empty() && params[0].self_kind != ast::SelfKind::None; }
  bool checked = false;              // body has been type checked
  bool checking = false;             // body check in progress (recursion guard)
};

struct FieldDef {
  std::string name;
  Span span;
  const Type* type = nullptr;
  bool is_pub = false;
};

struct StructDef {
  ModuleDef* module = nullptr;
  std::string name;
  Span span;
  const ast::StructItem* ast = nullptr;
  bool is_pub = false;
  bool is_builtin_string = false;  // the prelude String type
  bool is_builtin_vec = false;     // the prelude Vec type
  std::vector<GenericParamDef> generics;
  std::vector<FieldDef> fields;
  std::vector<FunctionDef*> methods;
  const Type* self_type = nullptr;  // Name<T, U> with its own params

  const FieldDef* find_field(const std::string& n) const {
    for (auto& f : fields) if (f.name == n) return &f;
    return nullptr;
  }
  FunctionDef* find_method(const std::string& n) const {
    for (auto* m : methods) if (m->name == n) return m;
    return nullptr;
  }
};

struct VariantDef {
  std::string name;
  Span span;
  uint32_t index = 0;
  ast::VariantKind kind = ast::VariantKind::Unit;
  std::vector<const Type*> tuple_types;
  std::vector<FieldDef> fields;
  EnumDef* owner = nullptr;
};

struct EnumDef {
  ModuleDef* module = nullptr;
  std::string name;
  Span span;
  const ast::EnumItem* ast = nullptr;
  bool is_pub = false;
  std::vector<GenericParamDef> generics;
  std::vector<VariantDef> variants;
  std::vector<FunctionDef*> methods;
  const Type* self_type = nullptr;

  const VariantDef* find_variant(const std::string& n) const {
    for (auto& v : variants) if (v.name == n) return &v;
    return nullptr;
  }
  FunctionDef* find_method(const std::string& n) const {
    for (auto* m : methods) if (m->name == n) return m;
    return nullptr;
  }
};

struct TraitDef {
  ModuleDef* module = nullptr;
  std::string name;
  Span span;
  const ast::TraitItem* ast = nullptr;
  bool is_pub = false;
  std::vector<GenericParamDef> generics;
  std::vector<FunctionDef*> methods;  // ast->body null means required
  const Type* self_type = nullptr;    // a Param named Self owned by the trait

  FunctionDef* find_method(const std::string& n) const {
    for (auto* m : methods) if (m->name == n) return m;
    return nullptr;
  }
};

struct ImplDef {
  ModuleDef* module = nullptr;
  Span span;
  const ast::ImplItem* ast = nullptr;
  std::vector<GenericParamDef> generics;
  TraitRef trait;
  const Type* self_type = nullptr;
  std::vector<FunctionDef*> methods;

  FunctionDef* find_method(const std::string& n) const {
    for (auto* m : methods) if (m->name == n) return m;
    return nullptr;
  }
};

struct ConstDef {
  ModuleDef* module = nullptr;
  bool checking = false;
  bool checked = false;
  std::string name;
  Span span;
  const ast::ConstItem* ast = nullptr;
  bool is_pub = false;
  const Type* type = nullptr;
};

enum class SymbolKind { Struct, Enum, Trait, Function, Const, Variant, Module };

struct Symbol {
  SymbolKind kind = SymbolKind::Function;
  Symbol() = default;
  explicit Symbol(SymbolKind k) : kind(k) {}
  StructDef* struct_def = nullptr;
  EnumDef* enum_def = nullptr;
  TraitDef* trait_def = nullptr;
  FunctionDef* fn_def = nullptr;
  ConstDef* const_def = nullptr;
  const VariantDef* variant = nullptr;  // for prelude variants like Some
  Span span;
  ModuleDef* module_def = nullptr;
  bool is_pub = true;
};

struct ModuleDef {
  std::string name;
  uint32_t file = 0;
  bool is_prelude = false;
  std::map<std::string, Symbol> symbols;
  // Importers get private namespace overlays. Loading a child in another
  // module must not add it to this module's visible namespace.
  ModuleDef* base_module = nullptr;
  ModuleDef* namespace_owner = nullptr;
  const Symbol* find(const std::string& item_name, const ModuleDef** owner = nullptr) const {
    auto it = symbols.find(item_name);
    if (it != symbols.end()) { if (owner) *owner = this; return &it->second; }
    return base_module ? base_module->find(item_name, owner) : nullptr;
  }
};

inline std::string qualified_name(const ModuleDef* module, const std::string& name) {
  return !module || module->is_prelude || module->name.empty() ? name : module->name + "." + name;
}

// Everything the compiler knows about one program.
struct Program {
  std::vector<std::unique_ptr<StructDef>> structs;
  std::vector<std::unique_ptr<EnumDef>> enums;
  std::vector<std::unique_ptr<TraitDef>> traits;
  std::vector<std::unique_ptr<ImplDef>> impls;
  std::vector<std::unique_ptr<FunctionDef>> functions;  // all, including methods
  std::vector<std::unique_ptr<ConstDef>> consts;
  std::map<std::string, Symbol> globals;  // compiler builtins and prelude
  std::vector<std::unique_ptr<ModuleDef>> modules;
  ModuleDef* root_module = nullptr;
  ModuleDef* active_module = nullptr;

  // Well known prelude items, filled by the resolver.
  EnumDef* option_def = nullptr;
  EnumDef* result_def = nullptr;
  StructDef* string_def = nullptr;
  StructDef* vec_def = nullptr;
  StructDef* range_def = nullptr;
  StructDef* range_inclusive_def = nullptr;
  StructDef* task_def = nullptr;
  StructDef* send_task_def = nullptr;
  StructDef* waker_def = nullptr;
  EnumDef* poll_def = nullptr;
  std::map<std::string, TraitDef*> core_traits;  // "Add", "Eq", "Display", ...

  const Symbol* find(const std::string& name) const {
    const ModuleDef* scope = active_module ? active_module : root_module;
    if (scope) {
      auto local = scope->symbols.find(name);
      if (local != scope->symbols.end()) return &local->second;
    }
    auto it = globals.find(name);
    return it == globals.end() ? nullptr : &it->second;
  }
  TraitDef* core_trait(const std::string& name) const {
    auto it = core_traits.find(name);
    return it == core_traits.end() ? nullptr : it->second;
  }
};

// Restores the caller's module when checking an inferred signature recursively.
struct ModuleScope {
  Program& program;
  ModuleDef* saved;
  ModuleScope(Program& p, ModuleDef* module) : program(p), saved(p.active_module) { p.active_module = module; }
  ~ModuleScope() { program.active_module = saved; }
};

// True when values of the type are copied instead of moved: primitives,
// read only references, raw pointers, and aggregates whose fields are all
// Copy and that have no Drop implementation.
bool is_copy_type(Program& prog, TypeContext& ctx, const Type* t);

}  // namespace synx::sema
