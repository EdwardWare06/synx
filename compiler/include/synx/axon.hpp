// Axon: the Synx native code generator for x86-64.
//
// Linear scan keeps eligible scalar locals in registers. Address-taken
// locals, aggregates, floats, and spilled intervals use stack slots.
//
// Calling conventions:
//  - Synx to Synx: arguments are copied into an outgoing area at the bottom
//    of the caller's frame, in order, each rounded up to 8 bytes. The callee
//    reads them at [rbp + 16 + offset]. Results up to 8 bytes return in rax,
//    up to 16 in rax:rdx; larger results are written through a pointer passed
//    as the first 8 bytes of the argument area.
//  - Synx to runtime or extern C: the platform C ABI (System V on Linux and
//    macOS, Microsoft x64 on Windows) for scalar arguments. Fat pointers are
//    passed as two scalars. Struct results are written through an out
//    pointer passed as the first argument.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "synx/defs.hpp"
#include "synx/diagnostics.hpp"
#include "synx/ir.hpp"
#include "synx/layout.hpp"
#include "synx/objfile.hpp"
#include "synx/types.hpp"

namespace synx::codegen {

enum class TargetOs { Linux, Windows, MacOs };

struct Target {
  TargetOs os;
  bool register_allocation = true;
  bool checked_arithmetic = true;
  ObjectFormat format() const {
    switch (os) {
      case TargetOs::Linux: return ObjectFormat::Elf;
      case TargetOs::Windows: return ObjectFormat::Coff;
      case TargetOs::MacOs: return ObjectFormat::MachO;
    }
    return ObjectFormat::Elf;
  }
  bool win64_abi() const { return os == TargetOs::Windows; }
  const char* name() const {
    switch (os) {
      case TargetOs::Linux: return "x86_64-linux";
      case TargetOs::Windows: return "x86_64-windows";
      case TargetOs::MacOs: return "x86_64-macos";
    }
    return "?";
  }
};

Target host_target();

class Axon {
 public:
  Axon(sema::Program& program, sema::TypeContext& ctx, ir::Module& module, DiagnosticSink& sink, Target target);

  // Generates code for every lowered function plus the entry stub. Returns
  // false on error.
  bool generate(ObjectFile& out);

  // Mangles an IR function name into a linker symbol.
  static std::string symbol_for(const std::string& ir_name);

 private:
  struct Frame;
  friend struct FunctionGen;

  sema::Program& prog_;
  sema::TypeContext& ctx_;
  ir::Module& module_;
  DiagnosticSink& sink_;
  Target target_;
  LayoutTable layouts_;
  std::map<const sema::Type*, std::string> drop_symbols_;
  std::vector<const sema::Type*> drop_worklist_;
  std::string drop_symbol(const sema::Type* type);
};

}  // namespace synx::codegen
