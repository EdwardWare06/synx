// Type checker. Walks every function body, infers types, resolves names,
// methods, and generics, and records the results for IR lowering.
#pragma once

#include <deque>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "synx/ast.hpp"
#include "synx/defs.hpp"
#include "synx/diagnostics.hpp"
#include "synx/resolve.hpp"
#include "synx/types.hpp"

namespace synx::sema {

struct LocalVar {
  std::string name;
  const Type* type = nullptr;
  bool is_const = false;
  Span span;
};

enum class RefKind {
  Local,      // a local variable or parameter
  Function,   // a module level or extern function
  Method,     // a method called on a value (Call expr)
  AssocFn,    // Type.function with no self
  Variant,    // an enum variant used as value or constructor
  Const,      // a module level constant
  TypeName,   // a struct or enum name used as the base of Type.item
  Field,      // a struct field (Field expr on a value)
  TupleField  // a tuple element by index
};

// What a Name, Field, or Call expression refers to.
struct NameRef {
  RefKind kind = RefKind::Local;
  uint32_t local_id = 0;
  FunctionDef* fn = nullptr;
  ImplDef* impl = nullptr;         // Method found through an impl
  TraitDef* trait = nullptr;       // Method found through a generic bound
  const VariantDef* variant = nullptr;
  ConstDef* const_def = nullptr;
  StructDef* struct_def = nullptr;
  EnumDef* enum_def = nullptr;
  uint32_t field_index = 0;
  int auto_derefs = 0;             // Field, Method: references stripped from the base
  std::vector<const Type*> type_args;  // Function, Method, AssocFn: generic args after inference
  std::vector<const Type*> owner_args; // Method, AssocFn, Variant: the owner type's args
};

enum class CoercionKind { None, AutoBorrowConst, AutoBorrowMut, MutToConst, StringToStr, ArrayToSlice, VecToSlice, ToDyn, FnMode, NeverToAny };

struct Coercion {
  CoercionKind kind = CoercionKind::None;
  bool borrow_first = false;  // AutoBorrow then deref coercion
  const Type* target = nullptr;  // ToDyn: reference to the trait object
};

// Lookup by key; iteration in first-insertion order. Whole-program checks
// that report while iterating stay independent of pointer hashing, and the
// self-hosted checker reproduces the order by inserting in the same sequence.
// References to values stay valid across insertions; iterators do not.
template <class K, class V>
class InsertionMap {
 public:
  using value_type = std::pair<K, V>;
  using iterator = typename std::deque<value_type>::iterator;
  using const_iterator = typename std::deque<value_type>::const_iterator;

  V& operator[](const K& key) {
    auto found = index_.find(key);
    if (found != index_.end()) return entries_[found->second].second;
    index_.emplace(key, entries_.size());
    entries_.emplace_back(key, V{});
    return entries_.back().second;
  }
  iterator find(const K& key) {
    auto found = index_.find(key);
    return found == index_.end() ? entries_.end() : entries_.begin() + static_cast<std::ptrdiff_t>(found->second);
  }
  const_iterator find(const K& key) const {
    auto found = index_.find(key);
    return found == index_.end() ? entries_.end() : entries_.begin() + static_cast<std::ptrdiff_t>(found->second);
  }
  V& at(const K& key) {
    auto found = index_.find(key);
    if (found == index_.end()) throw std::out_of_range("InsertionMap::at");
    return entries_[found->second].second;
  }
  const V& at(const K& key) const {
    auto found = index_.find(key);
    if (found == index_.end()) throw std::out_of_range("InsertionMap::at");
    return entries_[found->second].second;
  }
  size_t count(const K& key) const { return index_.count(key); }
  size_t size() const { return entries_.size(); }
  iterator begin() { return entries_.begin(); }
  iterator end() { return entries_.end(); }
  const_iterator begin() const { return entries_.begin(); }
  const_iterator end() const { return entries_.end(); }

 private:
  std::deque<value_type> entries_;
  std::unordered_map<K, size_t> index_;
};

struct CheckResult {
  InsertionMap<const ast::Expr*, const Type*> expr_types;
  InsertionMap<const ast::Expr*, NameRef> refs;
  InsertionMap<const ast::Expr*, Coercion> coercions;
  InsertionMap<const ast::VarDeclStmt*, uint32_t> decl_locals;
  InsertionMap<const ast::Pattern*, uint32_t> pattern_locals;
  InsertionMap<const ast::Pattern*, int> pattern_derefs;
  InsertionMap<const FunctionDef*, std::vector<LocalVar>> fn_locals;
  InsertionMap<const ConstDef*, std::vector<LocalVar>> const_locals;
  InsertionMap<const ast::Expr*, std::vector<uint32_t>> closure_params;
  InsertionMap<const ast::Expr*, std::vector<uint32_t>> closure_captures;
  InsertionMap<const ast::Expr*, std::map<uint32_t, ast::CaptureMode>> closure_capture_modes;
  InsertionMap<const ast::Expr*, std::vector<LocalVar>> closure_locals;
  InsertionMap<const ast::FieldInit*, uint32_t> shorthand_locals;

  const Type* type_of(const ast::Expr& e) const {
    auto it = expr_types.find(&e);
    return it == expr_types.end() ? nullptr : it->second;
  }
};

class Checker {
 public:
  Checker(Program& program, TypeContext& ctx, Resolver& resolver, DiagnosticSink& sink);

  // Registers compiler provided functions such as print and panic. Call
  // before the prelude is resolved.
  void register_builtins();
  const auto& builtin_str_methods() const { return builtin_str_methods_; }
  void check_program();
  CheckResult& result() { return result_; }
  const CheckResult& result() const { return result_; }
  bool has_marker(const Type* type, const std::string& name, bool frame_refs = false) {
    bool saved = frame_refs_; frame_refs_ = frame_refs;
    bool result = implements(type, TraitRef{prog_.core_trait(name), {}});
    frame_refs_ = saved; return result;
  }

 private:
  struct Scope {
    std::vector<std::pair<std::string, uint32_t>> names;
  };

  // Per function (or closure) checking state.
  struct FnCtx {
    FunctionDef* fn = nullptr;      // null inside a closure
    const Type* ret = nullptr;
    bool ret_inferred = false;
    bool saw_return = false;
    bool in_const_initializer = false;
    int loop_depth = 0;
    // One entry per open loop, innermost last: true once a break targets it.
    std::vector<bool> loop_breaks;
    int unsafe_depth = 0;
    TypeScope type_scope;
    std::vector<LocalVar>* locals = nullptr;
    std::vector<Scope> scopes;
    FnCtx* parent = nullptr;        // closures see the enclosing locals
    const ast::ClosureExpr* closure = nullptr;
    uint32_t local_start = 0;
  };

  struct Obligation {
    const Type* type;
    TraitRef trait;
    Span span;
    std::string what;
  };

  // ----- inference -----
  const Type* shallow(const Type* t) const;
  const Type* zonk(const Type* t);
  bool unify(const Type* a, const Type* b);
  bool unify_or_error(const Type* a, const Type* b, Span span, const char* context);
  size_t snapshot() const { return trail_.size(); }
  void rollback(size_t mark);
  bool try_unify(const Type* a, const Type* b);
  bool coerce(const ast::Expr& e, const Type* actual, const Type* expected, const char* context);
  bool coerce_dyn(const ast::Expr& e, const Type* concrete, const Type* target, bool borrow_first);
  std::string type_name(const Type* t);

  // ----- functions -----
  void check_function(FunctionDef& fn);
  void ensure_signature(FunctionDef& fn, Span use_span);
  TypeScope owner_scope(const FunctionDef& fn, TypeScope& storage);
  void check_const(ConstDef& c);

  // ----- scopes and locals -----
  void push_scope(FnCtx& fc);
  void pop_scope(FnCtx& fc);
  uint32_t declare_local(FnCtx& fc, const std::string& name, const Type* type, bool is_const, Span span);
  bool lookup_local(FnCtx& fc, const std::string& name, uint32_t& id_out, const LocalVar** var_out,
                    bool record_capture = true);

  // ----- statements -----
  const Type* check_block(FnCtx& fc, const ast::Block& block, const Type* expected);
  void check_stmt(FnCtx& fc, const ast::Stmt& stmt, bool& diverged);
  void check_var_decl(FnCtx& fc, const ast::VarDeclStmt& s);
  void check_assign(FnCtx& fc, const ast::AssignStmt& s);

  // ----- expressions -----
  const Type* check_expr(FnCtx& fc, const ast::Expr& e, const Type* expected);
  const Type* check_expr_inner(FnCtx& fc, const ast::Expr& e, const Type* expected);
  const Type* check_name(FnCtx& fc, const ast::NameExpr& e);
  const Type* check_symbol(FnCtx& fc, const ast::Expr& e, const Symbol& symbol,
                           const std::vector<ast::TypePtr>& type_args, const std::string& name);
  ModuleDef* module_expr(FnCtx& fc, const ast::Expr& e);
  const Symbol* module_member(ModuleDef& module, const std::string& name, Span span);
  bool accessible(ModuleDef* module, bool is_pub, Span span, const std::string& name);
  bool method_accessible(FunctionDef& fn, Span span);
  const Type* check_field(FnCtx& fc, const ast::FieldExpr& e);
  const Type* check_call(FnCtx& fc, const ast::CallExpr& e, const Type* expected);
  const Type* check_index(FnCtx& fc, const ast::IndexExpr& e);
  const Type* check_unary(FnCtx& fc, const ast::UnaryExpr& e, const Type* expected);
  const Type* check_binary(FnCtx& fc, const ast::BinaryExpr& e, const Type* expected);
  const Type* check_cast(FnCtx& fc, const ast::CastExpr& e);
  const Type* check_try(FnCtx& fc, const ast::TryExpr& e);
  const Type* check_struct_lit(FnCtx& fc, const ast::StructLitExpr& e);
  const Type* check_if(FnCtx& fc, const ast::IfExpr& e, const Type* expected);
  const Type* check_match(FnCtx& fc, const ast::MatchExpr& e, const Type* expected);
  const Type* check_closure(FnCtx& fc, const ast::ClosureExpr& e, const Type* expected);
  const Type* check_interp(FnCtx& fc, const ast::InterpExpr& e);
  const Type* check_range(FnCtx& fc, const ast::RangeExpr& e, const Type* expected);

  const Type* instantiate_function(FunctionDef& fn, const std::vector<const Type*>& explicit_args,
                                   const Subst& owner_subst, Span span, NameRef& ref);
  bool resolve_type_name(FnCtx& fc, const ast::Expr& e, StructDef*& s, EnumDef*& en,
                         std::vector<const Type*>& args, Span span);
  std::vector<const Type*> resolve_explicit_args(FnCtx& fc, const std::vector<ast::TypePtr>& args);
  const Type* variant_value_type(const VariantDef& v, std::vector<const Type*> enum_args, NameRef& ref);

  struct MethodHit {
    FunctionDef* fn = nullptr;
    ImplDef* impl = nullptr;
    TraitDef* trait = nullptr;
    int derefs = 0;
    const Type* base = nullptr;     // receiver type after stripping references
    Subst owner_subst;
    std::vector<const Type*> owner_args;
  };
  bool lookup_method(const Type* recv, const std::string& name, MethodHit& hit);
  bool method_in_impls(const Type* base, const std::string& name, MethodHit& hit);
  const Type* check_method_call(FnCtx& fc, const ast::CallExpr& call, const ast::FieldExpr& callee,
                                const Type* expected);
  void check_receiver(FnCtx& fc, const ast::Expr& recv_expr, const Type* recv_type,
                      const MethodHit& hit, Span span);
  const Type* check_args(FnCtx& fc, const std::vector<ast::ExprPtr>& args, const Type* fn_type,
                         size_t skip_params, Span span);

  // ----- places -----
  bool is_place(FnCtx& fc, const ast::Expr& e);
  bool place_is_mutable(FnCtx& fc, const ast::Expr& e);

  // ----- patterns -----
  void check_pattern(FnCtx& fc, const ast::Pattern& p, const Type* type, int ref_mode);
  const VariantDef* resolve_variant_path(const ast::Path& path, const Type* scrutinee,
                                         std::vector<const Type*>& args, Span span);
  bool pattern_is_irrefutable(const ast::Pattern& p);
  bool covers(const Type* t, const std::vector<const ast::Pattern*>& pats, std::string& missing);
  void check_exhaustive(const ast::MatchExpr& m, const Type* scrutinee);

  // ----- traits -----
  bool implements(const Type* t, const TraitRef& trait);
  bool builtin_implements(const Type* t, const std::string& trait_name);
  ImplDef* find_impl(const Type* t, TraitDef* trait, Subst* subst_out);
  const std::vector<TraitRef>* bounds_of(const Type* param);
  void add_obligation(const Type* t, const TraitRef& trait, Span span, const std::string& what);
  void check_obligations();
  const Type* operator_result(FnCtx& fc, const ast::BinaryExpr& e, const Type* lhs, const Type* rhs);

  // ----- helpers -----
  const Type* record(const ast::Expr& e, const Type* t);
  const Type* option_of(const Type* t);
  const Type* string_type();
  const Type* strip_refs(const Type* t, int* count);
  bool is_copy(const Type* t);
  void finish_function_types();
  void error(Span span, const std::string& msg, const char* code = "") { sink_.error(span, msg, code); }

  Program& prog_;
  TypeContext& ctx_;
  Resolver& resolver_;
  DiagnosticSink& sink_;
  CheckResult result_;
  bool frame_refs_ = false;

  std::vector<const Type*> bindings_;            // infer var id -> bound type
  std::vector<uint32_t> trail_;                  // ids bound since last snapshot
  std::vector<Obligation> obligations_;
  std::map<const Type*, const std::vector<TraitRef>*> param_bounds_;
  std::map<TraitDef*, std::vector<TraitRef>> self_bounds_;
  std::vector<std::pair<const Type*, const Type*>> checking_traits_;
  std::vector<const ast::Expr*> exprs_in_function_;
  std::vector<std::pair<const ast::Expr*, Span>> infer_sites_;
  std::vector<FunctionDef*> builtin_str_methods_;
  FunctionDef* print_fn_ = nullptr;
  FunctionDef* panic_fn_ = nullptr;
};

}  // namespace synx::sema
