#pragma once

#include "synx/defs.hpp"
#include "synx/check.hpp"
#include "synx/diagnostics.hpp"
#include "synx/ir.hpp"

namespace synx::ir {

// Run after borrow checking and drop elaboration. Logical async locals become
// fields of a heap frame; poll and cancellation use the same stored flags.
bool lower_async(sema::Program& program, sema::TypeContext& types, Module& module, DiagnosticSink& sink, sema::Checker& checker);

}  // namespace synx::ir
