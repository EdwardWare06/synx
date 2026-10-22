// Command line driver for the Synx compiler.
// Reports build information and runs the inspection, checking, and native
// build commands. Compiler passes live in synx_core.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

#include "synx/ast.hpp"
#include "synx/build.hpp"
#include "synx/compile.hpp"
#include "synx/diagnostics.hpp"
#include "synx/lexer.hpp"
#include "synx/parser.hpp"
#include "synx/source.hpp"
#include "synx/version.hpp"

static void print_usage() {
  std::puts("usage: synx <command> [options]\n"
            "commands:\n"
            "  version            print the compiler version and host platform\n"
            "  about              describe this build of the compiler\n"
            "  lex <file.snx>     print the token stream of a file\n"
            "  parse <file.snx>   print the syntax tree (--strict adds spans and every field)\n"
            "  modules <file.snx> print declarations and bound module namespaces (--signatures, --check, --lower, --borrow, --async stop after that stage; --machine adds layouts and registers)\n"
            "  check <file.snx>   check names, types, and ownership without building\n"
            "  ir <file.snx>      print the Dendrite IR of a program\n"
            "  build <file.snx>   compile to a native executable (-o out, --release, --keep-obj, --no-regalloc, --linker=builtin, --linker=system, -v)\n"
            "  run <file.snx>     build and run");
}

static int cmd_build(int argc, char** argv, bool run) {
  synx::BuildOptions opt;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-o" && i + 1 < argc) opt.output = argv[++i];
    else if (a == "--keep-obj") opt.keep_object = true;
    else if (a == "--no-regalloc") opt.target.register_allocation = false;
    else if (a == "--release") opt.target.checked_arithmetic = false;
    else if (a == "--linker=builtin") opt.linker = "builtin";
    else if (a == "--linker=system") opt.linker = "system";
    else if (a == "-v" || a == "--verbose") opt.verbose = true;
    else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "unknown option: %s\n", a.c_str()); return 1; }
    else opt.input = a;
  }
  if (opt.input.empty()) {
    std::fputs("build needs a file argument\n", stderr);
    return 1;
  }
  synx::BuildResult r = synx::build_program(opt);
  if (!r.ok) return 1;
  if (run) return synx::run_executable(r.executable);
  std::printf("Built %s\n", r.executable.c_str());
  return 0;
}

static int cmd_ir(synx::SourceFile file) {
  synx::Compilation c(synx::SourceFile("<prelude>", synx::sema::prelude_source()), std::move(file));
  bool ok = c.run_front_end();
  if (c.prelude_sink.has_errors()) {
    std::fputs("internal error: the prelude does not check\n", stderr);
    std::fputs(c.prelude_sink.render(c.prelude_file).c_str(), stderr);
    return 2;
  }
  if (ok) ok = c.run_lowering();
  std::printf("%s", synx::ir::dump(c.ir_module).c_str());
  if (!ok) {
    std::fputs(c.sink.render(c.user_file).c_str(), stderr);
    std::fprintf(stderr, "%zu error%s\n", c.sink.error_count(), c.sink.error_count() == 1 ? "" : "s");
    return 1;
  }
  return 0;
}

static int cmd_about() {
  // Keep implementation claims tied to this build. Change the compiler and
  // lineage rows only after the bootstrap checks pass.
  std::printf("  o--o--o   Synx %s\n"
              "            Created by Edward Ware\n\n", synx::version());
  std::printf("  This build\n"
              "    Target      %s / x86-64\n", synx::host_platform());
  std::puts("    Compiler    C++ bootstrap\n"
            "    Library     Synx\n"
            "    Runtime     C platform adapters\n"
            "\n"
            "  Origins and direction\n"
            "    Started with C and C++.\n"
            "    Planned transition: compiler written in Synx.\n"
            "    C remains for startup, allocation, I/O, number conversion/formatting,\n"
            "    threads, synchronization, clocks, and host services.\n"
            "\n"
            "  Toolchain\n"
            "    Front end   lexer, parser, modules, and type checking\n"
            "    Dendrite    typed IR, borrow and move checks\n"
            "    Axon        x86-64 code generation and register allocation\n"
            "    Objects     ELF / COFF / Mach-O\n"
            "    Linker      system toolchain\n"
            "\n"
            "  Planned\n"
            "    Myelin package manager; ARM64 targets\n"
            "\n"
            "  This is not an Arasaka product choom");
  return 0;
}

static int cmd_lex(const synx::SourceFile& file) {
  synx::DiagnosticSink sink;
  synx::Lexer lexer(file, sink);
  auto tokens = lexer.tokenize();
  for (const auto& t : tokens) std::printf("%s\n", synx::token_to_string(t).c_str());
  if (sink.has_errors()) {
    std::fputs(sink.render(file).c_str(), stderr);
    return 1;
  }
  return 0;
}

static int cmd_parse(const synx::SourceFile& file, bool strict) {
  synx::DiagnosticSink sink;
  synx::Lexer lexer(file, sink);
  auto tokens = lexer.tokenize();
  synx::Parser parser(file, std::move(tokens), sink);
  synx::ast::Module module = parser.parse_module();
  std::printf("%s\n", (strict ? synx::ast::dump_strict(module) : synx::ast::dump(module)).c_str());
  if (sink.has_errors()) {
    std::fputs(sink.render(file).c_str(), stderr);
    return 1;
  }
  return 0;
}

static int cmd_check(synx::SourceFile file) {
  synx::Compilation c(synx::SourceFile("<prelude>", synx::sema::prelude_source()), std::move(file));
  bool ok = c.run_front_end();
  if (c.prelude_sink.has_errors()) {
    std::fputs("internal error: the prelude does not check\n", stderr);
    std::fputs(c.prelude_sink.render(c.prelude_file).c_str(), stderr);
    return 2;
  }
  // Lowering runs the borrow checker, so ownership errors are reported too.
  if (ok) ok = c.run_lowering();
  if (!ok) {
    std::fputs(c.sink.render(c.user_file).c_str(), stderr);
    std::fprintf(stderr, "%zu error%s\n", c.sink.error_count(), c.sink.error_count() == 1 ? "" : "s");
    return 1;
  }
  std::printf("%s: ok\n", c.user_file.path().c_str());
  return 0;
}

static int driver_main(int argc, char** argv) {
  if (argc < 2) {
    print_usage();
    return 1;
  }
  const char* cmd = argv[1];
  if (std::strcmp(cmd, "version") == 0 || std::strcmp(cmd, "--version") == 0) {
    std::printf("Synx %s (%s)\n", synx::version(), synx::host_platform());
    return 0;
  }
  if (std::strcmp(cmd, "about") == 0) return cmd_about();
  if (std::strcmp(cmd, "build") == 0) return cmd_build(argc, argv, false);
  if (std::strcmp(cmd, "run") == 0) return cmd_build(argc, argv, true);
  bool known = std::strcmp(cmd, "lex") == 0 || std::strcmp(cmd, "parse") == 0 || std::strcmp(cmd, "check") == 0 ||
              std::strcmp(cmd, "ir") == 0 || std::strcmp(cmd, "modules") == 0;
  if (!known) {
    std::fprintf(stderr, "unknown command: %s\n", cmd);
    print_usage();
    return 1;
  }
  if (argc < 3) {
    std::fprintf(stderr, "%s needs a file argument\n", cmd);
    return 1;
  }
  auto file = synx::SourceFile::load_utf8(argv[2]);
  if (!file) {
    std::fprintf(stderr, "cannot read file: %s\n", argv[2]);
    return 1;
  }
  if (std::strcmp(cmd, "lex") == 0) return cmd_lex(*file);
  if (std::strcmp(cmd, "parse") == 0) return cmd_parse(*file, argc > 3 && std::strcmp(argv[3], "--strict") == 0);
  if (std::strcmp(cmd, "modules") == 0) {
    synx::Compilation c(synx::SourceFile("<prelude>", synx::sema::prelude_source()), std::move(*file));
    const char* stage = argc > 3 ? argv[3] : "";
    bool check = std::strcmp(stage, "--check") == 0;
    // Dendrite stages: lowering, then ownership checks, then async frames.
    // --machine also prints layouts and register allocation after async frames.
    int ir_stage = std::strcmp(stage, "--lower") == 0 ? 1 : std::strcmp(stage, "--borrow") == 0 ? 2 : std::strcmp(stage, "--async") == 0 ? 3
                 : std::strcmp(stage, "--machine") == 0 ? 4 : 0;
    bool ok;
    if (ir_stage) {
      ok = c.run_front_end() && c.lower_ir(false);
      if (ok && ir_stage >= 2) ok = c.check_ownership();
      if (ok && ir_stage >= 3) ok = c.convert_async();
    } else {
      ok = check ? c.run_front_end() : std::strcmp(stage, "--signatures") == 0 ? c.run_signatures() : c.run_declarations();
    }
    std::printf("%s", synx::dump_declarations(c).c_str());
    if (check) std::printf("%s", synx::dump_check(c).c_str());
    if (ir_stage) std::printf("%s", synx::dump_ir_strict(c).c_str());
    if (ir_stage >= 4 && ok) std::printf("%s", synx::dump_machine(c).c_str());
    return ok ? 0 : 1;
  }
  if (std::strcmp(cmd, "ir") == 0) return cmd_ir(std::move(*file));
  return cmd_check(std::move(*file));
}

int main(int argc, char** argv) {
#if defined(_WIN32)
  // Source paths and driver options use UTF-8, independent of the ANSI codepage.
  int count = 0;
  wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
  if (!wide) return 1;
  std::vector<std::string> storage;
  for (int i = 0; i < count; ++i) {
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, nullptr, 0, nullptr, nullptr);
    if (!n) { LocalFree(wide); std::fputs("command line is not valid Unicode\n", stderr); return 1; }
    std::string text(static_cast<size_t>(n), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, text.data(), n, nullptr, nullptr)) { LocalFree(wide); std::fputs("command line is not valid Unicode\n", stderr); return 1; }
    text.pop_back(); storage.push_back(std::move(text));
  }
  LocalFree(wide);
  std::vector<char*> pointers;
  for (auto& text : storage) pointers.push_back(text.data());
  pointers.push_back(nullptr);
  (void)argc; (void)argv;
  return driver_main(count, pointers.data());
#else
  return driver_main(argc, argv);
#endif
}
