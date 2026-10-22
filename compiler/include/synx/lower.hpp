// Lowers the checked syntax tree to Dendrite IR, one concrete function at a
// time, starting from main and following calls.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "synx/check.hpp"
#include "synx/defs.hpp"
#include "synx/diagnostics.hpp"
#include "synx/ir.hpp"
#include "synx/types.hpp"

namespace synx::ir {

class Lowerer {
 public:
  Lowerer(sema::Program& program, sema::TypeContext& ctx, const sema::CheckResult& checked,
          DiagnosticSink& sink);

  // Lowers every non-generic user function plus everything reachable from
  // them. With require_main, a missing main is an error. Returns false on
  // error.
  bool lower_program(Module& module, bool require_main);

  // Lowers one function instantiation on demand; used by tests too.
  Function* instance(const sema::FunctionDef& def, std::vector<const Type*> owner_args,
                     std::vector<const Type*> type_args);

 private:
  struct LoopCtx {
    uint32_t break_block;
    uint32_t continue_block;
    size_t scope_depth;
  };

  // State while lowering one function body.
  struct FnCtx {
    Function* fn = nullptr;
    const sema::ConstDef* constant = nullptr;
    sema::Subst subst;
    uint32_t current = 0;                 // block being appended to
    std::vector<uint32_t> local_map;      // checker local id -> ir local
    std::map<uint32_t, Place> captures;
    std::vector<LoopCtx> loops;
    uint32_t return_block = 0;
    // Locals declared in each open scope, innermost last, for drops.
    std::vector<std::vector<uint32_t>> scopes;
  };

  void lower_body(Function& f);
  void lower_async_factory(FnCtx& c);
  Operand lower_await(FnCtx& c, const ast::AwaitExpr& e);
  void drain();

  // ----- building blocks -----
  uint32_t new_block(FnCtx& c);
  uint32_t new_temp(FnCtx& c, const Type* t, Span span);
  void emit(FnCtx& c, Statement s);
  void assign(FnCtx& c, Place p, Rvalue r, Span span);
  void set_term(FnCtx& c, Terminator t);
  void goto_block(FnCtx& c, uint32_t target);
  bool block_terminated(FnCtx& c) const;
  void push_scope(FnCtx& c);
  void scope_temporary(FnCtx& c, uint32_t local);
  void pop_scope(FnCtx& c, Span span);
  void emit_drops_to(FnCtx& c, size_t scope_depth, Span span);

  // ----- types -----
  const Type* type_of(FnCtx& c, const ast::Expr& e);
  const Type* subst_type(FnCtx& c, const Type* t);
  bool needs_drop(const Type* t);
  std::map<const Type*, bool> drop_cache_;
  bool is_copy_type(const Type* t);

  // ----- expressions -----
  Operand lower_operand(FnCtx& c, const ast::Expr& e);
  Operand lower_const(FnCtx& c, const sema::ConstDef& constant);
  Operand lower_closure(FnCtx& c, const ast::ClosureExpr& e);
  Place checker_place(FnCtx& c, uint32_t id);
  const std::vector<sema::LocalVar>& checker_locals(FnCtx& c);
  // Read-only views of the checker's closure tables. A closure without an
  // entry captures nothing; a capture without a mode moves.
  const std::vector<uint32_t>& captures_of(const ast::Expr* closure) const;
  ast::CaptureMode capture_mode(const ast::Expr* closure, uint32_t id) const;
  void lower_into(FnCtx& c, const ast::Expr& e, Place dest);
  bool lower_place(FnCtx& c, const ast::Expr& e, Place& out);
  Operand operand_from_place(FnCtx& c, Place p, const Type* t);
  Operand apply_coercion(FnCtx& c, const ast::Expr& e, Operand op, const Type* from);
  Operand lower_call(FnCtx& c, const ast::CallExpr& e, Place* dest);
  Operand lower_method_call(FnCtx& c, const ast::CallExpr& e, const ast::FieldExpr& callee,
                            const sema::NameRef& ref, Place* dest);
  Operand lower_binary(FnCtx& c, const ast::BinaryExpr& e, Place* dest);
  // -x or !x on a user type: calls Neg.neg or Not.not with the operand by value.
  // Separate from lower_operand to keep its recursive frame small.
  Operand lower_unary_trait(FnCtx& c, const ast::UnaryExpr& u, const sema::Type* operand_type,
                            const sema::Type* t, Span span);
  void lower_if(FnCtx& c, const ast::IfExpr& e, Place* dest);
  void lower_match(FnCtx& c, const ast::MatchExpr& e, Place* dest);
  void lower_interp(FnCtx& c, const ast::InterpExpr& e, Place dest);
  Operand lower_try(FnCtx& c, const ast::TryExpr& e);
  void lower_block_into(FnCtx& c, const ast::Block& b, Place* dest);
  void lower_stmt(FnCtx& c, const ast::Stmt& s);
  void lower_for(FnCtx& c, const ast::ForStmt& s);
  void lower_return(FnCtx& c, const ast::Expr* value, Span span);
  bool assigns_through_raw_pointer(FnCtx& c, const ast::Expr& target);
  void append_display(FnCtx& c, Place string_place, Operand value, const Type* t, Span span);
  void emit_bounds_check(FnCtx& c, uint32_t index_local, Place container, const Type* container_t, Span span);

  // ----- patterns -----
  // Emits tests for `p` against the value in `place` of type `t`. Control
  // continues in the returned block on success; on failure it jumps to
  // `fail`. ref_const is set when matching through a &const.
  uint32_t lower_pattern_test(FnCtx& c, const ast::Pattern& p, Place place, const Type* t, uint32_t fail);
  void lower_pattern_bind(FnCtx& c, const ast::Pattern& p, Place place, const Type* t, int ref_mode);
  Place auto_deref(FnCtx& c, Place place, const Type*& t, int& ref_mode, int count);

  // ----- calls -----
  Function* resolve_call_target(FnCtx& c, const sema::NameRef& ref, const Type* receiver_base);
  Function* resolve_trait_method(const sema::FunctionDef& trait_method, const Type* self_type,
                                 std::vector<const Type*> fn_args, Span span,
                                 const std::vector<const Type*>* trait_args = nullptr);
  Vtable* vtable_for(const Type* concrete, const Type* dyn_type, Span span);
  std::string instance_name(const sema::FunctionDef& def, const std::vector<const Type*>& owner_args,
                            const std::vector<const Type*>& type_args);
  void call(FnCtx& c, Callee callee, std::vector<Operand> args, Place dest, bool diverges, Span span);

  void error(Span span, const std::string& msg) { sink_.error(span, msg, "SNX0601"); ok_ = false; }

  sema::Program& prog_;
  sema::TypeContext& ctx_;
  const sema::CheckResult& checked_;
  DiagnosticSink& sink_;
  Module* module_ = nullptr;
  std::map<std::string, Function*> instances_;
  std::map<const ast::ClosureExpr*, uint32_t> closure_ids_;
  std::vector<Function*> worklist_;
  bool ok_ = true;
};

}  // namespace synx::ir
