// Name resolution for items: collects declarations from the prelude and the
// user's module, then resolves every type in every signature.
#pragma once

#include <vector>

#include "synx/ast.hpp"
#include "synx/defs.hpp"
#include "synx/diagnostics.hpp"
#include "synx/types.hpp"

namespace synx::sema {

// A chain of generic scopes used while resolving a type expression.
struct TypeScope {
  const std::vector<GenericParamDef>* generics = nullptr;
  const Type* self_type = nullptr;
  const TypeScope* parent = nullptr;

  const GenericParamDef* find(const std::string& name) const {
    for (const TypeScope* s = this; s; s = s->parent) {
      if (s->generics) {
        for (auto& g : *s->generics) if (g.name == name) return &g;
      }
    }
    return nullptr;
  }
  const Type* find_self() const {
    for (const TypeScope* s = this; s; s = s->parent) {
      if (s->self_type) return s->self_type;
    }
    return nullptr;
  }
};

class Resolver {
 public:
  Resolver(Program& program, TypeContext& ctx, DiagnosticSink& sink);

  // Declares and resolves the items of one module. Call once for the prelude
  // with is_prelude = true, then once for the user module.
  void resolve_module(const ast::Module& module, bool is_prelude);
  void declare_module(const ast::Module& module, ModuleDef* scope, bool is_prelude);
  void resolve_signatures();
  // Declaration-order worklists for the self-hosting oracle. Valid after
  // declare_module/load_modules and before resolve_signatures consumes them.
  const auto& pending_structs() const { return pending_structs_; }
  const auto& pending_enums() const { return pending_enums_; }
  const auto& pending_traits() const { return pending_traits_; }
  const auto& pending_impls() const { return pending_impls_; }
  const auto& pending_functions() const { return pending_functions_; }
  const auto& pending_consts() const { return pending_consts_; }
  const Symbol* lookup_path(const ast::Path& path, size_t count = SIZE_MAX, bool report_missing = false);

  // Resolves a type expression in a scope. Public so the checker can resolve
  // types written inside function bodies.
  const Type* resolve_type(const ast::Type& t, const TypeScope& scope);
  TraitRef resolve_bound(const ast::Bound& b, const TypeScope& scope);
  std::vector<GenericParamDef> declare_generics(const std::vector<ast::GenericParam>& gs,
                                                const void* owner);

 private:
  void declare_items(const ast::Module& module, bool is_prelude);
  bool declare_global(const std::string& name, Symbol sym);
  FunctionDef* new_function(const ast::Function& fn, FnOwner owner);
  void resolve_function_signature(FunctionDef& fn, const TypeScope& scope);


  Program& prog_;
  TypeContext& ctx_;
  DiagnosticSink& sink_;
  bool is_prelude_ = false;

  // Items declared in this call to resolve_module, resolved afterwards.
  std::vector<StructDef*> pending_structs_;
  std::vector<EnumDef*> pending_enums_;
  std::vector<TraitDef*> pending_traits_;
  std::vector<ImplDef*> pending_impls_;
  std::vector<FunctionDef*> pending_functions_;
  std::vector<ConstDef*> pending_consts_;
};

// Source text of the prelude, written in Synx.
const char* prelude_source();

}  // namespace synx::sema
