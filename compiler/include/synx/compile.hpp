// Owns one compilation: sources, modules, semantic definitions, and Dendrite.
// The driver and tests use this API to run passes in the required order.
#pragma once

#include <memory>
#include <string>

#include "synx/ast.hpp"
#include "synx/check.hpp"
#include "synx/defs.hpp"
#include "synx/diagnostics.hpp"
#include "synx/ir.hpp"
#include "synx/resolve.hpp"
#include "synx/source.hpp"
#include "synx/types.hpp"

namespace synx {

struct ModuleSource {
  SourceFile file;
  ast::Module ast;
  explicit ModuleSource(SourceFile source) : file(std::move(source)) {}
};

struct Compilation {
  SourceFile prelude_file;
  SourceFile user_file;
  DiagnosticSink prelude_sink;  // errors in the prelude are compiler bugs
  DiagnosticSink sink;
  ast::Module prelude_ast;
  ast::Module user_ast;
  std::vector<std::unique_ptr<ModuleSource>> imported_sources;
  sema::TypeContext types;
  sema::Program program;
  std::unique_ptr<sema::Resolver> resolver;
  std::unique_ptr<sema::Checker> checker;
  ir::Module ir_module;

  Compilation(SourceFile prelude, SourceFile user);
  // Diagnostics and semantic records keep addresses into these owners.
  // The compilation must remain at one address until all passes finish.
  Compilation(const Compilation&) = delete;
  Compilation& operator=(const Compilation&) = delete;
  Compilation(Compilation&&) = delete;
  Compilation& operator=(Compilation&&) = delete;

  // Lexes and parses the entry/prelude, loads imports, then resolves and
  // checks declarations and bodies. Imported sources remain owned here.
  bool run_front_end();
  // Parses source, registers builtins, declares modules, and binds imports.
  // Stops before signatures. Retains partial state and diagnostics on failure.
  // Call once on a fresh Compilation; run_front_end runs this first.
  bool run_declarations();
  // run_declarations, then resolve_signatures when declarations succeed.
  // Stops before body checks. Retains partial state and diagnostics.
  bool run_signatures();
  bool load_modules();
  // Lowers logical bodies, checks ownership, then converts async frames.
  // Call after run_front_end succeeds. require_main requires an entry point.
  bool run_lowering(bool require_main = false);
  // The three passes of run_lowering, for stage oracles. Call in order, each
  // only after the previous one succeeds.
  bool lower_ir(bool require_main);
  bool check_ownership();
  bool convert_async();
};

// Stable declaration oracle. IDs use arena kind/index, never pointer addresses.
// Includes all modules, symbols, definitions, prelude links, and diagnostics.
// After run_signatures it also holds fields, variant payloads, generic bounds,
// impl traits, parameters, and return types.
std::string dump_declarations(const Compilation& compilation);
// Checker oracle: every CheckResult table in insertion order, with nodes
// printed as kind and span. Diagnostics are in dump_declarations.
std::string dump_check(const Compilation& compilation);
// Dendrite oracle: every function, local, statement, terminator, and module
// table with spans, float bits, and type parameter identities. Diagnostics are
// in dump_declarations. Drop tables print sorted by type text.
std::string dump_ir_strict(const Compilation& compilation);
// Axon oracle: per function, the register allocation (register number or -1,
// then the saved callee registers) and the layout of every local, with each
// variant payload of an enum local; then each vtable's concrete layout. Call
// only after convert_async succeeds; allocation assumes complete IR.
std::string dump_machine(Compilation& compilation);
// Axon object oracle: for Linux, Windows, and macOS with the given flags, one
// line with the target name, generation result, object size, and FNV-1a 32
// hash of the complete object file. Generation errors go to the sink.
std::string dump_objects(Compilation& compilation, bool register_allocation, bool checked_arithmetic);

// Convenience for tests: check a source string. Returns the compilation so
// the caller can inspect diagnostics and results.
std::unique_ptr<Compilation> check_source(const std::string& path, const std::string& text);

}  // namespace synx
