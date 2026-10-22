// Driver core records and standalone CLI parity. Mode bits match the other
// self-hosting tests: stack allocation and release arithmetic.
#include <array>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include "selfhost_support.hpp"
#include "synx/build.hpp"
#include "synx/lexer.hpp"
#include "synx/parser.hpp"
#include "test_support.hpp"

using namespace selfhost_test;

static fs::path from_utf8(std::string_view text) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

static void write_file(const fs::path& file, const std::string& data) {
  std::ofstream stream(file, std::ios::binary);
  stream.write(data.data(), static_cast<std::streamsize>(data.size()));
  if (!stream) throw std::runtime_error("cannot write " + utf8(file));
}
static std::string core_reference(const fs::path& source) {
  std::string out; blob(out, read(source / "compiler/prelude.snx"));
  for (auto os : {synx::codegen::TargetOs::Linux, synx::codegen::TargetOs::Windows, synx::codegen::TargetOs::MacOs}) {
    const bool windows = os == synx::codegen::TargetOs::Windows;
    for (const auto& input : {"main.snx", "dir/with space.snx", "dir/.hidden", "dir/name.", "caf\xC3\xA9/\xCE\xBB.snx"}) {
      auto path = from_utf8(input); auto executable = path.parent_path() / path.stem();
      if (windows) executable += ".exe";
      auto text = executable.generic_u8string();
      std::string name(reinterpret_cast<const char*>(text.data()), text.size());
      blob(out, name); blob(out, name + (windows ? ".obj" : ".o"));
    }
    std::vector<std::string> args;
    blob(out, windows ? "link" : "cc");
    if (windows) args = {"/nologo", "/subsystem:console", "/stack:8388608", "/entry:wmainCRTStartup", "/include:wmain",
                         "objects/a quote' &.o", "runtime/lib caf\xC3\xA9.a", "legacy_stdio_definitions.lib", "/out:outputs/\xCE\xBB $(literal)"};
    else {
      args = {"objects/a quote' &.o", "runtime/lib caf\xC3\xA9.a", "-pthread", "-o", "outputs/\xCE\xBB $(literal)"};
      if (os == synx::codegen::TargetOs::Linux) args.push_back("-no-pie");
      else { args.push_back("-arch"); args.push_back("x86_64"); }
    }
    integer(out, args.size(), 8); for (const auto& arg : args) blob(out, arg);
    // Fixed parsing cases: defaults, flags/last input, unknown, missing output,
    // missing input, empty input, and an option-shaped output argument.
    integer(out, 1, 1); blob(out, "test.snx"); blob(out, "");
    for (bool flag : {false, false, true, true}) integer(out, flag, 1);
    integer(out, 1, 1); blob(out, "last.snx"); blob(out, "literal & output");
    for (bool flag : {true, true, false, false}) integer(out, flag, 1);
    for (const auto& error : {"unknown option: --unknown\n", "unknown option: -o\n", "build needs a file argument\n", "build needs a file argument\n"}) {
      integer(out, 0, 1); blob(out, error);
    }
    integer(out, 1, 1); blob(out, "main.snx"); blob(out, "--release");
    for (bool flag : {false, true, true, true}) integer(out, flag, 1);
  }
  return out;
}

struct Capture { int code = -1; std::string out, err; };
#if defined(_WIN32)
static constexpr const char* program_stdout = "driver caf\xC3\xA9\r\n";
#else
static constexpr const char* program_stdout = "driver caf\xC3\xA9\n";
#endif
// Keep streams separate: diagnostic placement is part of CLI parity.
static Capture capture(const fs::path& program, const std::vector<std::string>& args, const fs::path& directory) {
  const auto stdout_file = directory / "stdout.bin", stderr_file = directory / "stderr.bin";
  Capture result;
#if defined(_WIN32)
  SECURITY_ATTRIBUTES security{sizeof security, nullptr, TRUE};
  HANDLE output = CreateFileW(stdout_file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, 0, nullptr);
  HANDLE error = CreateFileW(stderr_file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, 0, nullptr);
  if (output == INVALID_HANDLE_VALUE || error == INVALID_HANDLE_VALUE) {
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    if (error != INVALID_HANDLE_VALUE) CloseHandle(error);
    throw std::runtime_error("cannot capture compiler streams");
  }
  std::wstring line = L"\"" + program.wstring() + L"\"";
  for (const auto& arg : args) line += L" \"" + wide(arg) + L"\"";
  STARTUPINFOW startup{}; startup.cb = sizeof startup; startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = output; startup.hStdError = error; startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION process{};
  const BOOL started = CreateProcessW(program.c_str(), line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process);
  CloseHandle(output); CloseHandle(error);
  if (started) {
    DWORD code = 0;
    if (WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &code)) result.code = static_cast<int>(code);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
  }
#else
  std::string program_text = utf8(program), output_text = utf8(stdout_file), error_text = utf8(stderr_file);
  std::vector<char*> arguments{program_text.data()};
  for (const auto& arg : args) arguments.push_back(const_cast<char*>(arg.c_str()));
  arguments.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  int error = posix_spawn_file_actions_init(&actions);
  if (error) throw std::runtime_error("cannot initialize process capture");
  error = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, output_text.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (!error) error = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, error_text.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
#if defined(__APPLE__)
  char** environment = *_NSGetEnviron();
#else
  char** environment = environ;
#endif
  pid_t process = 0;
  if (!error) error = posix_spawn(&process, program_text.c_str(), &actions, nullptr, arguments.data(), environment);
  posix_spawn_file_actions_destroy(&actions);
  if (!error) {
    int code = 0; pid_t waited;
    do { waited = waitpid(process, &code, 0); } while (waited < 0 && errno == EINTR);
    if (waited > 0) result.code = WIFEXITED(code) ? WEXITSTATUS(code) : WIFSIGNALED(code) ? 128 + WTERMSIG(code) : -1;
  }
#endif
  result.out = read(stdout_file); result.err = read(stderr_file); return result;
}
static void parity(const fs::path& oracle, const fs::path& compiler, const std::vector<std::string>& args, const fs::path& directory) {
  const auto expected = capture(oracle, args, directory), actual = capture(compiler, args, directory);
  CHECK(expected.code == actual.code); CHECK(expected.out == actual.out); CHECK(expected.err == actual.err);
  if (expected.code != actual.code || expected.out != actual.out || expected.err != actual.err) {
    write_file(directory / "expected-stdout.bin", expected.out); write_file(directory / "expected-stderr.bin", expected.err);
    write_file(directory / "actual-stdout.bin", actual.out); write_file(directory / "actual-stderr.bin", actual.err);
    std::string command;
    for (const auto& arg : args) { command += ' '; command += arg; }
    throw std::runtime_error("CLI mismatch:" + command + " (status " + std::to_string(expected.code) + '/' + std::to_string(actual.code) + ')');
  }
}
// Environment changes stay inside this single-process test, then restore.
struct EnvironmentGuard {
  std::string name, value; bool present = false;
#if defined(_WIN32)
  std::wstring wide_value;
#endif
  explicit EnvironmentGuard(std::string key) : name(std::move(key)) {
#if defined(_WIN32)
    wchar_t* old = nullptr; size_t size = 0;
    if (!_wdupenv_s(&old, &size, wide(name).c_str()) && old) { present = true; wide_value = old; }
    std::free(old); _wputenv_s(wide(name).c_str(), L"");
#else
    if (const char* old = std::getenv(name.c_str())) { present = true; value = old; }
    unsetenv(name.c_str());
#endif
  }
  void set(const std::string& replacement) {
#if defined(_WIN32)
    if (_wputenv_s(wide(name).c_str(), wide(replacement).c_str())) throw std::runtime_error("cannot set test environment");
#else
    if (setenv(name.c_str(), replacement.c_str(), 1)) throw std::runtime_error("cannot set test environment");
#endif
  }
  ~EnvironmentGuard() {
#if defined(_WIN32)
    _wputenv_s(wide(name).c_str(), present ? wide_value.c_str() : L"");
#else
    if (present) setenv(name.c_str(), value.c_str(), 1); else unsetenv(name.c_str());
#endif
  }
};
static void cli(const fs::path& oracle, const fs::path& compiler, const fs::path& source, const fs::path& directory) {
  const auto runtime_library = synx::find_runtime_library(synx::codegen::host_target());
  EnvironmentGuard std_source("SYNX_STD_PATH"), runtime_source("SYNX_RUNTIME_LIB"),
      shared_source("SYNX_RUNTIME_SHARED"), linker_source("SYNX_LINKER"), linker_driver_source("SYNX_LINKER_DRIVER");
  std_source.set(utf8(source / "std")); runtime_source.set(runtime_library);
  const auto entry = directory / from_utf8("entry caf\xC3\xA9 \xCE\xBB.snx");
  const auto broken = directory / "broken.snx", malformed = directory / "malformed.snx", bad_bytes = directory / "bytes.snx";
  write_file(entry, "import std.io; struct Item { int value; } int main() { double x = 1.234567890123; char c = '\\u{3bb}'; io.print(\"driver caf\xC3\xA9\"); return 7; }\n");
  write_file(broken, "int main() { int x = true; return x; }\n");
  write_file(malformed, "struct Broken { int value;\nmain( {\n");
  write_file(bad_bytes, std::string("main(){ var x = \"a\\0b\"; var \xFF = 1; }\n"));
  for (const auto& args : std::vector<std::vector<std::string>>{{}, {"version"}, {"--version"}, {"unknown"}, {"check"}, {"build"},
        {"build", "-o"}, {"build", "--bad"}, {"check", utf8(directory / "missing.snx")},
        {"lex", utf8(entry)}, {"parse", utf8(entry)}, {"parse", utf8(entry), "--strict"},
        {"lex", utf8(bad_bytes)}, {"parse", utf8(bad_bytes), "--strict"},
        {"parse", utf8(malformed), "--strict"}, {"check", utf8(entry)}, {"check", utf8(broken)},
        {"ir", utf8(entry)}, {"ir", utf8(broken)}}) parity(oracle, compiler, args, directory);
  for (const auto& flag : {"", "--signatures", "--check", "--lower", "--borrow", "--async", "--machine"}) {
    std::vector<std::string> args{"modules", utf8(entry)}; if (*flag) args.push_back(flag); parity(oracle, compiler, args, directory);
  }
  // The pretty AST printer is new; exercise its node forms over the checked
  // repository corpus. Skip malformed trees because stage 0's human printer
  // assumes required children exist. Recovery is compared through --strict.
  for (const auto& corpus : {"tests/run", "examples"}) {
    for (const auto& file : fs::recursive_directory_iterator(source / corpus)) {
      if (!file.is_regular_file() || file.path().extension() != ".snx") continue;
      auto input = synx::SourceFile::load_utf8(utf8(file.path())); if (!input) continue;
      synx::DiagnosticSink sink; synx::Lexer lexer(*input, sink);
      synx::Parser parser(*input, lexer.tokenize(), sink); auto tree = parser.parse_module();
      if (!sink.has_errors()) parity(oracle, compiler, {"parse", utf8(file.path())}, directory);
    }
  }
  const auto about = capture(compiler, {"about"}, directory);
  CHECK(about.code == 0); CHECK(about.err.empty());
  CHECK(about.out.find("Created by Edward Ware") != std::string::npos);
  CHECK(about.out.find("Synx compiler port") != std::string::npos);
  CHECK(about.out.find("This is not an Arasaka product choom") != std::string::npos);
  // Compile the same source to the same output path so both stdout and object
  // bytes can be compared without normalizing paths or symbols.
  const auto output = directory / "program.exe";
  for (unsigned mode = 0; mode < 4; ++mode) {
    std::vector<std::string> args{"build", utf8(entry), "--keep-obj", "-o", utf8(output)};
    if (mode & 1) args.push_back("--no-regalloc");
    if (mode & 2) args.push_back("--release");
    const auto baseline = capture(oracle, args, directory); CHECK(baseline.code == 0);
    if (baseline.code) throw std::runtime_error(baseline.err);
#if defined(_WIN32)
    const auto object = fs::path(utf8(output) + ".obj");
#else
    const auto object = fs::path(utf8(output) + ".o");
#endif
    const auto expected_object = read(object); write_file(directory / ("expected-object-" + std::to_string(mode) + ".bin"), expected_object);
    const auto actual = capture(compiler, args, directory);
    CHECK(actual.code == 0); CHECK(actual.out == baseline.out); CHECK(actual.err == baseline.err);
    CHECK(read(object) == expected_object);
    const auto program = capture(output, {}, directory); CHECK(program.code == 7);
    CHECK(program.out == program_stdout); CHECK(program.err.empty());
  }
  parity(oracle, compiler, {"run", utf8(entry), "-o", utf8(output)}, directory);
#if defined(_WIN32)
  CHECK(!fs::exists(fs::path(utf8(output) + ".obj")));
#else
  CHECK(!fs::exists(fs::path(utf8(output) + ".o")));
#endif
  // Relocated package with spaces/Unicode, no environment overrides, and path
  // characters that a shell would parse. Only the Synx driver uses Command.
  const auto package = directory / from_utf8("installed caf\xC3\xA9 \xCE\xBB");
  fs::create_directories(package / "bin"); fs::create_directories(package / "lib");
  fs::create_directories(package / "libexec");
  const auto installed = package / "bin/synx.exe";
  fs::copy_file(compiler, installed);
  const auto runtime = from_utf8(runtime_library);
  CHECK(!runtime.empty()); fs::copy_file(runtime, package / "lib" / runtime.filename());
#if defined(_WIN32)
  const char* capsule_name = "synx_rt.dll";
  const char* helper_name = "synx-link.exe";
#elif defined(__APPLE__)
  const char* capsule_name = "libsynx_rt.dylib";
  const char* helper_name = "synx-link";
#else
  const char* capsule_name = "libsynx_rt.so";
  const char* helper_name = "synx-link";
#endif
  const auto capsule = runtime.parent_path() / capsule_name;
  if (!fs::is_regular_file(capsule)) throw std::runtime_error("driver package test needs the shared runtime: " + utf8(capsule));
  fs::copy_file(capsule, package / "bin" / capsule.filename());
  fs::copy_file(capsule, package / "lib" / capsule.filename());
  // C++ bridge packages also carry the Synx linker helper. Native Synx
  // compilers invoke their own linker module and do not launch this file.
  const auto helper = oracle.parent_path() / helper_name;
  if (fs::is_regular_file(helper)) fs::copy_file(helper, package / "libexec" / helper_name);
  fs::copy(source / "std", package / "std", fs::copy_options::recursive);
  EnvironmentGuard std_path("SYNX_STD_PATH"), runtime_path("SYNX_RUNTIME_LIB"),
      shared_path("SYNX_RUNTIME_SHARED"), linker_path("SYNX_LINKER"), linker_driver_path("SYNX_LINKER_DRIVER");
  const auto literal = directory / from_utf8("literal & '$(name) \xCE\xBB.exe");
  auto result = capture(installed, {"build", utf8(entry), "-o", utf8(literal)}, directory);
  CHECK(result.code == 0); if (result.code) throw std::runtime_error(result.err);
  result = capture(literal, {}, directory); CHECK(result.code == 7); CHECK(result.out == program_stdout);
}
int main(int argc, char** argv) {
  if (argc != 4 && argc != 5) return 1;
  const unsigned mode = static_cast<unsigned>(std::stoi(argv[3])); const bool full = argc == 5 && std::string(argv[4]) == "cli";
  const bool provided = argc == 5 && !full;
  const auto directory = fs::temp_directory_path() / ("synx-driver-" + std::to_string(mode) + '-' +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    fs::create_directories(directory); const fs::path compiler = fs::absolute(argv[1]), source = fs::absolute(argv[2]);
    const auto probe = directory / "driver-probe.exe", report = directory / "actual.bin", log = directory / "run.log";
    std::vector<std::string> args{"build", utf8(source / "selfhost/driver_probe.snx"), "-o", utf8(probe)};
    if (mode & 1) args.push_back("--no-regalloc");
    if (mode & 2) args.push_back("--release");
    const fs::path builder = provided ? fs::absolute(argv[4]) : compiler;
    int rc = run(builder, args, log); CHECK(rc == 0); if (rc) throw std::runtime_error(read(log));
    rc = run(probe, {utf8(report)}, log); CHECK(rc == 0); if (rc) throw std::runtime_error(read(log));
    const auto expected = core_reference(source), actual = read(report); CHECK(actual == expected);
    if (actual != expected) { write_file(directory / "expected.bin", expected); throw std::runtime_error("driver core parity mismatch"); }
    if (full || provided) {
      const auto standalone = provided ? fs::absolute(argv[4]) : directory / "synx-stage1.exe";
      args = {"build", utf8(source / "selfhost/main.snx"), "-o", utf8(standalone)};
      if (mode & 1) args.push_back("--no-regalloc");
      if (mode & 2) args.push_back("--release");
      if (!provided) { rc = run(compiler, args, log); CHECK(rc == 0); if (rc) throw std::runtime_error(read(log)); }
      cli(compiler, standalone, source, directory);
    }
    std::printf("driver mode %u: %zu core bytes%s\n", mode, actual.size(), full || provided ? "; standalone CLI and object parity" : "");
    const int status = synx_test::finish("selfhost_driver");
    if (!status) fs::remove_all(directory);
    else std::fprintf(stderr, "artifacts: %s\n", utf8(directory).c_str());
    return status;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\nartifacts: %s\n", error.what(), utf8(directory).c_str()); return 1;
  }
}
