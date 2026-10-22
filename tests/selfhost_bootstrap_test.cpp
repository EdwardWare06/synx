// Optional long bootstrap validation. Builds stages 1-3, compares objects,
// checks stage 2 as an installed package, then runs native and self-hosting
// harnesses with the stage-2 compiler.
#include <chrono>
#include <stdexcept>
#include "selfhost_support.hpp"
#include "synx/build.hpp"
#include "test_support.hpp"
using namespace selfhost_test;
static fs::path from_utf8(std::string_view text) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

int main(int argc, char** argv) {
  if (argc < 4) return 1;
  fs::path artifacts;
  try {
    const fs::path stage0 = fs::absolute(argv[1]), source = fs::absolute(argv[2]), build = fs::absolute(argv[3]);
    bool quick = false;
    std::vector<std::string> compiler_flags;
    for (int i = 4; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--quick") quick = true;
      else if (option == "--release" || option == "--no-regalloc") compiler_flags.push_back(option);
      else throw std::runtime_error("unknown bootstrap test option: " + option);
    }
    artifacts = build / from_utf8("bootstrap validation caf\xC3\xA9 \xCE\xBB " +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(artifacts);
#if defined(_WIN32)
    const std::string suffix = ".exe";
#else
    const std::string suffix;
#endif
    const auto runner = artifacts / ("bootstrap-runner" + suffix);
    const auto runtime = synx::find_runtime_library(synx::codegen::host_target());
    if (runtime.empty()) throw std::runtime_error("bootstrap needs the compiled runtime");
#if defined(_WIN32)
    const auto capsule_name = "synx_rt.dll";
#elif defined(__APPLE__)
    const auto capsule_name = "libsynx_rt.dylib";
#else
    const auto capsule_name = "libsynx_rt.so";
#endif
    const auto capsule = from_utf8(runtime).parent_path() / capsule_name;
    if (!fs::is_regular_file(capsule)) throw std::runtime_error("bootstrap needs the shared runtime capsule");
    const Environment environment{{"SYNX_STD_PATH", utf8(source / "std")}, {"SYNX_RUNTIME_LIB", runtime},
        {"SYNX_RUNTIME_SHARED", utf8(capsule)}, {"SYNX_LINKER", "builtin"}};
    auto checked_run = [&](const fs::path& program, const std::vector<std::string>& args, const std::string& name) {
      const auto step_log = artifacts / (name + ".log");
      std::printf("bootstrap: %s\n", name.c_str()); std::fflush(stdout);
      const int code = run(program, args, step_log, environment); CHECK(code == 0);
      if (code) throw std::runtime_error(name + " failed (" + std::to_string(code) + ")\n" + read(step_log));
    };
    checked_run(stage0, {"build", utf8(source / "selfhost/bootstrap.snx"), "-o", utf8(runner), "--linker=system"}, "build-runner");
    std::vector<std::string> arguments{utf8(stage0), utf8(source), utf8(artifacts)};
    arguments.insert(arguments.end(), compiler_flags.begin(), compiler_flags.end());
    checked_run(runner, arguments, "fixed-point");
    CHECK(read(artifacts / "bootstrap.txt").find("objects_identical=true") != std::string::npos);
#if defined(_WIN32)
    const std::string object_suffix = ".obj";
#else
    const std::string object_suffix = ".o";
#endif
    const auto object1 = read(artifacts / ("synx-stage1" + suffix + object_suffix));
    const auto object2 = read(artifacts / ("synx-stage2" + suffix + object_suffix));
    const auto object3 = read(artifacts / ("synx-stage3" + suffix + object_suffix));
    CHECK(!object1.empty()); CHECK(object1 == object2); CHECK(object2 == object3);
    if (object1.empty() || object1 != object2 || object2 != object3) throw std::runtime_error("independent bootstrap object comparison failed");
    const auto stage2 = artifacts / ("synx-stage2" + suffix);
    // Installed package: the install layout (bin, lib, std) in a relocated
    // tree, with SYNX_STD_PATH and SYNX_RUNTIME_LIB removed so the compiler
    // finds both from its own path. The installed stage 2 rebuilds the
    // compiler; that build must match stage 2 run with explicit paths to the
    // same tree, and the rebuilt compiler must build and run the workbench.
    const auto package = artifacts / from_utf8("installed caf\xC3\xA9 \xCE\xBB");
    fs::create_directories(package / "bin"); fs::create_directories(package / "lib");
    const auto installed = package / "bin" / ("synx" + suffix);
    fs::copy_file(stage2, installed);
    fs::copy_file(capsule, package / "bin" / capsule_name);
    fs::copy_file(capsule, package / "lib" / capsule_name);
    const auto runtime_path = from_utf8(runtime);
    fs::copy_file(runtime_path, package / "lib" / runtime_path.filename());
    fs::copy(source / "std", package / "std", fs::copy_options::recursive);
    const std::vector<std::string> unset{"SYNX_STD_PATH", "SYNX_RUNTIME_LIB", "SYNX_RUNTIME_SHARED", "SYNX_LINKER", "SYNX_LINKER_DRIVER"};
    auto package_run = [&](const fs::path& program, const std::vector<std::string>& args, const std::string& name, bool explicit_paths) {
      const auto step_log = artifacts / (name + ".log");
      std::printf("bootstrap: %s\n", name.c_str()); std::fflush(stdout);
      const Environment paths{{"SYNX_STD_PATH", utf8(package / "std")}, {"SYNX_RUNTIME_SHARED", utf8(package / "lib" / capsule_name)}, {"SYNX_LINKER", "builtin"}};
      const int code = explicit_paths ? run(program, args, step_log, paths) : run(program, args, step_log, {}, unset);
      CHECK(code == 0);
      if (code) throw std::runtime_error(name + " failed (" + std::to_string(code) + ")\n" + read(step_log));
    };
    auto compiler_build = [&](const fs::path& output) {
      std::vector<std::string> args{"build", utf8(source / "selfhost/main.snx"), "-o", utf8(output), "--keep-obj"};
      args.insert(args.end(), compiler_flags.begin(), compiler_flags.end()); return args;
    };
    const auto rebuilt = artifacts / ("synx-installed" + suffix), reference = artifacts / ("synx-package-reference" + suffix);
    package_run(installed, {"version"}, "installed-version", false);
    package_run(installed, compiler_build(rebuilt), "installed-build", false);
    package_run(stage2, compiler_build(reference), "package-reference", true);
    const auto rebuilt_object = read(fs::path(utf8(rebuilt) + object_suffix));
    CHECK(!rebuilt_object.empty()); CHECK(rebuilt_object == read(fs::path(utf8(reference) + object_suffix)));
    if (rebuilt_object.empty() || rebuilt_object != read(fs::path(utf8(reference) + object_suffix)))
      throw std::runtime_error("installed compiler object differs from the explicit-path build");
    fs::copy_file(rebuilt, installed, fs::copy_options::overwrite_existing);
    const auto workbench = artifacts / ("workbench-installed" + suffix);
    std::vector<std::string> workbench_args{"build", utf8(source / "examples/workbench/main.snx"), "-o", utf8(workbench)};
    workbench_args.insert(workbench_args.end(), compiler_flags.begin(), compiler_flags.end());
    package_run(installed, workbench_args, "installed-workbench", false);
    package_run(workbench, {}, "installed-workbench-run", false);
    const auto tests = build / "tests";
    auto executable = [&](const std::string& name) { return tests / (name + suffix); };
    for (unsigned mode = 0; mode < 4; ++mode) {
      const auto m = std::to_string(mode);
      checked_run(executable("test_selfhost_driver"), {utf8(stage0), utf8(source), m, utf8(stage2)}, "driver-" + m);
      if (quick) break;
      std::vector<std::string> flags;
      if (mode & 1) flags.push_back("--no-regalloc");
      if (mode & 2) flags.push_back("--release");
      for (const auto& corpus : {"tests/run", "examples"}) {
        std::vector<std::string> args{utf8(stage2), utf8(source / corpus)}; args.insert(args.end(), flags.begin(), flags.end());
        checked_run(executable("test_run"), args, (std::string(corpus) == "examples" ? "examples-" : "native-") + m);
      }
      checked_run(executable("test_selfhost_types"), {utf8(stage2), utf8(source), m}, "types-" + m);
      checked_run(executable("test_selfhost_parser"), {utf8(stage2), utf8(source), m}, "parser-" + m);
      checked_run(executable("test_selfhost_backend"), {utf8(stage2), utf8(source), m}, "backend-" + m);
      checked_run(executable("test_selfhost_modules"), {utf8(stage2), utf8(source), m, "all",
          utf8(executable("test_borrow")), utf8(executable("test_async_pool"))}, "frontend-" + m);
    }
    if (!quick) {
      checked_run(executable("test_selfhost_lexer"), {utf8(stage2), utf8(source)}, "lexer");
      checked_run(executable("test_host"), {utf8(stage2), utf8(source / "tests/run/host.snx")}, "host");
    }
    std::printf("bootstrap %s checks passed; retained artifacts: %s\n", quick ? "quick" : "full", utf8(artifacts).c_str());
    return synx_test::finish("selfhost_bootstrap");
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\nartifacts: %s\n", error.what(), utf8(artifacts).c_str()); return 1;
  }
}
