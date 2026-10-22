// Borrow checker over Dendrite IR.
//
// Runs three analyses on every lowered function:
//  1. Move analysis: a forward dataflow over locals (initialized, moved,
//     uninitialized). Reports use after move and use before assignment.
//  2. Drop elaboration: removes drops of moved values and adds a drop flag
//     for values that may or may not have been moved.
//  3. Loan checking: every `&place` creates a loan. Loans flow through
//     locals, aggregates, and calls that return references. A loan is active
//     while any local carrying it is live. Conflicting access to the borrowed
//     place while a loan is active is an error.
#pragma once

#include <string>
#include <vector>

#include "synx/diagnostics.hpp"
#include "synx/ir.hpp"
#include "synx/types.hpp"

namespace synx::ir {

struct FunctionEffects {
  std::set<uint32_t> writes;
  std::set<uint32_t> returns;
};

class BorrowChecker {
 public:
  BorrowChecker(sema::Program& program, sema::TypeContext& ctx, DiagnosticSink& sink);

  // Checks and rewrites every function in the module. Returns false on error.
  bool check_module(Module& module);
  bool check_function(Function& f);

 private:
  sema::Program& prog_;
  sema::TypeContext& ctx_;
  DiagnosticSink& sink_;
  std::map<const Function*, FunctionEffects> write_effects_;
};

}  // namespace synx::ir
