// Build driver: source file to native executable, and run.
#pragma once

#include <string>

#include "synx/axon.hpp"

namespace synx {

struct BuildOptions {
  std::string input;
  std::string output;        // executable path; default from input name
  bool keep_object = false;
  bool verbose = false;
  std::string linker;        // builtin/system; empty uses SYNX_LINKER or system
  codegen::Target target = codegen::host_target();
};

struct BuildResult {
  bool ok = false;
  std::string executable;
  std::string object;
};

// Compiles and links. Diagnostics go to stderr. Returns the result.
BuildResult build_program(const BuildOptions& options);

// Runs an executable, forwarding stdout and stderr. Returns its exit code.
int run_executable(const std::string& path);

std::string find_standard_library();

// Locates the runtime library next to the compiler. Empty if not found.
std::string find_runtime_library(const codegen::Target& target);

}  // namespace synx
