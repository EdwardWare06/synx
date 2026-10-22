#include "synx/build.hpp"

#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "synx/compile.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <spawn.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace synx {

namespace fs = std::filesystem;

static fs::path utf8_path(const std::string& text) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}
static std::string path_text(const fs::path& path) {
  auto text = path.generic_u8string();
  return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}
static int system_utf8(const std::string& command) {
#if defined(_WIN32)
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, command.c_str(), -1, nullptr, 0);
  if (!n) return -1;
  std::wstring wide(static_cast<size_t>(n), L'\0');
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, command.c_str(), -1, wide.data(), n)) return -1;
  return _wsystem(wide.c_str());
#else
  return std::system(command.c_str());
#endif
}

static std::string environment(const char* name) {
#if defined(_WIN32)
  std::wstring wide_name;
  for (const char* p = name; *p; ++p) wide_name.push_back(static_cast<wchar_t>(*p));
  size_t length = 0;
  if (_wgetenv_s(&length, nullptr, 0, wide_name.c_str()) != 0 || length <= 1) return "";
  std::wstring value(length, L'\0');
  if (_wgetenv_s(&length, value.data(), value.size(), wide_name.c_str()) != 0) return "";
  value.resize(length - 1);
  return path_text(fs::path(value));
#else
  const char* value = std::getenv(name);
  return value ? value : "";
#endif
}

#if defined(_WIN32)
// Encode one argv item using the Windows C runtime's backslash/quote rules.
// CreateProcess receives an explicit executable path; no command shell runs.
static std::wstring windows_argument(const std::wstring& argument) {
  std::wstring result = L"\"";
  size_t backslashes = 0;
  for (wchar_t ch : argument) {
    if (ch == L'\\') { ++backslashes; continue; }
    if (ch == L'"') {
      result.append(backslashes * 2 + 1, L'\\');
      result.push_back(ch);
    } else {
      result.append(backslashes, L'\\');
      result.push_back(ch);
    }
    backslashes = 0;
  }
  result.append(backslashes * 2, L'\\');
  result.push_back(L'"');
  return result;
}
#endif

// Inherit standard streams and environment. Arguments remain separate values;
// shell metacharacters in source/output paths never become commands.
static int launch(const fs::path& executable, const std::vector<std::string>& arguments) {
#if defined(_WIN32)
  std::wstring command = windows_argument(executable.native());
  for (const auto& argument : arguments) {
    command.push_back(L' ');
    command += windows_argument(utf8_path(argument).native());
  }
  if (command.size() >= 32767) {
    std::fputs("process command line exceeds the Windows limit\n", stderr);
    return -1;
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof startup;
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, 0,
                      nullptr, nullptr, &startup, &process)) {
    std::fprintf(stderr, "cannot start %s (Windows error %lu)\n", path_text(executable).c_str(), GetLastError());
    return -1;
  }
  DWORD code = 1;
  bool ok = WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0 &&
            GetExitCodeProcess(process.hProcess, &code) != 0;
  if (!ok) std::fprintf(stderr, "cannot wait for %s (Windows error %lu)\n", path_text(executable).c_str(), GetLastError());
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return ok ? static_cast<int>(code) : -1;
#else
  std::vector<std::string> storage;
  storage.push_back(executable.native());
  for (const auto& argument : arguments) storage.push_back(argument);
  std::vector<char*> argv;
  for (auto& argument : storage) argv.push_back(argument.data());
  argv.push_back(nullptr);
  pid_t child;
  int error = posix_spawn(&child, executable.c_str(), nullptr, nullptr, argv.data(), environ);
  if (error != 0) {
    std::fprintf(stderr, "cannot start %s: %s\n", path_text(executable).c_str(), std::strerror(error));
    return -1;
  }
  int status = 0;
  while (waitpid(child, &status, 0) == -1) {
    if (errno == EINTR) continue;
    std::fprintf(stderr, "cannot wait for %s: %s\n", path_text(executable).c_str(), std::strerror(errno));
    return -1;
  }
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return -1;
#endif
}

static fs::path executable_dir() {
#if defined(_WIN32)
  DWORD size = 256;
  for (;;) {
    std::wstring path(size, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, path.data(), size);
    if (!n) return fs::current_path();
    if (n < size) { path.resize(n); return fs::path(path).parent_path(); }
    if (size > MAXDWORD / 2) return fs::current_path();
    size *= 2;
  }
#elif defined(__APPLE__)
  char buf[4096];
  uint32_t size = sizeof buf;
  if (_NSGetExecutablePath(buf, &size) != 0) return fs::current_path();
  return fs::weakly_canonical(buf).parent_path();
#else
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n <= 0) return fs::current_path();
  return fs::path(std::string(buf, static_cast<size_t>(n))).parent_path();
#endif
}

std::string find_standard_library() {
#if defined(_WIN32)
  size_t length = 0;
  if (_wgetenv_s(&length, nullptr, 0, L"SYNX_STD_PATH") == 0 && length > 1) {
    std::wstring value(length, L'\0');
    if (_wgetenv_s(&length, value.data(), value.size(), L"SYNX_STD_PATH") == 0) {
      value.resize(length - 1);
      auto text = fs::path(value).generic_u8string();
      return std::string(reinterpret_cast<const char*>(text.data()), text.size());
    }
  }
#else
  if (const char* path = std::getenv("SYNX_STD_PATH")) return path;
#endif
  auto dir = executable_dir();
  for (int up = 0; up < 4; ++up) {
    if (fs::is_regular_file(dir / "std" / "io.snx")) {
      auto text = (dir / "std").generic_u8string();
      return std::string(reinterpret_cast<const char*>(text.data()), text.size());
    }
    dir = dir.parent_path();
  }
  return fs::is_regular_file(fs::path(std::u8string(reinterpret_cast<const char8_t*>(SYNX_STD_DIR))) / "io.snx") ? SYNX_STD_DIR : "";
}

std::string find_runtime_library(const codegen::Target& target) {
#if defined(_WIN32)
  size_t length = 0;
  if (_wgetenv_s(&length, nullptr, 0, L"SYNX_RUNTIME_LIB") == 0 && length > 1) {
    std::wstring value(length, L'\0');
    if (_wgetenv_s(&length, value.data(), value.size(), L"SYNX_RUNTIME_LIB") == 0) {
      value.resize(length - 1);
      if (fs::exists(fs::path(value))) return path_text(fs::path(value));
    }
  }
#else
  if (const char* env = std::getenv("SYNX_RUNTIME_LIB")) {
    if (fs::exists(env)) return env;
  }
#endif
  const char* name = target.os == codegen::TargetOs::Windows ? "synx_rt.lib" : "libsynx_rt.a";
  fs::path dir = executable_dir();
  // Build tree: <build>/compiler/synx next to <build>/runtime/libsynx_rt.a,
  // or <build>/compiler/Debug/synx.exe with multi config generators.
  for (int up = 0; up < 3; ++up) {
    fs::path candidate = dir / "runtime" / name;
    if (fs::exists(candidate)) return path_text(candidate);
    candidate = dir / "lib" / name;
    if (fs::exists(candidate)) return path_text(candidate);
    dir = dir.parent_path();
  }
  return "";
}

static std::string find_linker_driver() {
  auto override = environment("SYNX_LINKER_DRIVER");
  if (!override.empty()) {
    auto path = utf8_path(override);
    return fs::is_regular_file(path) ? path_text(fs::absolute(path)) : "";
  }
#if defined(_WIN32)
  constexpr const char* name = "synx-link.exe";
#else
  constexpr const char* name = "synx-link";
#endif
  auto dir = executable_dir();
  for (int up = 0; up < 5; ++up) {
    for (const auto& folder : {fs::path{}, fs::path{"compiler"}, fs::path{"libexec"}}) {
      auto candidate = dir / folder / name;
      if (fs::is_regular_file(candidate)) return path_text(candidate);
    }
    dir = dir.parent_path();
  }
  return "";
}

static std::string find_runtime_shared(const codegen::Target& target) {
  auto override = environment("SYNX_RUNTIME_SHARED");
  if (!override.empty()) {
    auto path = utf8_path(override);
    return fs::is_regular_file(path) ? path_text(fs::absolute(path)) : "";
  }
  const char* name = target.os == codegen::TargetOs::Windows ? "synx_rt.dll" :
                     target.os == codegen::TargetOs::MacOs ? "libsynx_rt.dylib" : "libsynx_rt.so";
  auto dir = executable_dir();
  for (int up = 0; up < 5; ++up) {
    for (const auto& folder : {fs::path{}, fs::path{"runtime"}, fs::path{"lib"}}) {
      auto candidate = dir / folder / name;
      if (fs::is_regular_file(candidate)) return path_text(candidate);
    }
    dir = dir.parent_path();
  }
  return "";
}

static std::string quote(const std::string& s) {
#if defined(_WIN32)
  return "\"" + s + "\"";
#else
  std::string out = "'";
  for (char c : s) { if (c == '\'') out += "'\\''"; else out += c; }
  return out + "'";
#endif
}

BuildResult build_program(const BuildOptions& options) {
  BuildResult result;
  std::string linker = options.linker.empty() ? environment("SYNX_LINKER") : options.linker;
  if (linker.empty()) linker = "system";
  if (linker != "builtin" && linker != "system") {
    std::fprintf(stderr, "invalid linker: %s (expected builtin or system)\n", linker.c_str());
    return result;
  }
  auto file = SourceFile::load_utf8(options.input);
  if (!file) {
    std::fprintf(stderr, "cannot read file: %s\n", options.input.c_str());
    return result;
  }
  Compilation c(SourceFile("<prelude>", sema::prelude_source()), std::move(*file));
  bool ok = c.run_front_end();
  if (c.prelude_sink.has_errors()) {
    std::fputs("internal error: the prelude does not check\n", stderr);
    std::fputs(c.prelude_sink.render(c.prelude_file).c_str(), stderr);
    return result;
  }
  if (ok) ok = c.run_lowering(true);
  codegen::ObjectFile obj;
  if (ok) {
    codegen::Axon axon(c.program, c.types, c.ir_module, c.sink, options.target);
    ok = axon.generate(obj);
  }
  if (!ok) {
    std::fputs(c.sink.render(c.user_file).c_str(), stderr);
    std::fprintf(stderr, "%zu error%s\n", c.sink.error_count(), c.sink.error_count() == 1 ? "" : "s");
    return result;
  }

  fs::path input = utf8_path(options.input);
  bool windows = options.target.os == codegen::TargetOs::Windows;
  fs::path exe = options.output.empty() ? input.parent_path() / input.stem() : utf8_path(options.output);
  if (options.output.empty() && windows) exe += ".exe";
  fs::path objpath = exe;
  objpath += windows ? ".obj" : ".o";

  std::vector<uint8_t> bytes = codegen::write_object(obj, options.target.format());
  {
    std::ofstream out(objpath, std::ios::binary);
    if (!out) {
      std::fprintf(stderr, "cannot write %s\n", path_text(objpath).c_str());
      return result;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  if (options.verbose) {
    std::fprintf(stderr, "Dendrite IR: %zu functions\n", c.ir_module.functions.size());
    std::fprintf(stderr, "Axon code generator: %s, %zu bytes of code\n", options.target.name(), obj.text.size());
  }

  int rc;
  if (linker == "builtin") {
    std::string driver = find_linker_driver();
    if (driver.empty()) {
      std::fputs("cannot find synx-link (set SYNX_LINKER_DRIVER)\n", stderr);
      return result;
    }
    std::string runtime = find_runtime_shared(options.target);
    if (runtime.empty()) {
      std::fputs("cannot find the Synx runtime capsule (set SYNX_RUNTIME_SHARED)\n", stderr);
      return result;
    }
    const char* os = windows ? "windows" : options.target.os == codegen::TargetOs::MacOs ? "macos" : "linux";
    std::vector<std::string> arguments{os, path_text(fs::absolute(objpath)), path_text(fs::absolute(exe)), runtime};
    if (options.verbose) {
      std::fprintf(stderr, "link: %s", quote(driver).c_str());
      for (const auto& argument : arguments) std::fprintf(stderr, " %s", quote(argument).c_str());
      std::fputc('\n', stderr);
    }
    rc = launch(utf8_path(driver), arguments);
  } else {
    std::string rt = find_runtime_library(options.target);
    if (rt.empty()) {
      std::fputs("cannot find the Synx runtime library (set SYNX_RUNTIME_LIB)\n", stderr);
      return result;
    }
    std::string cmd;
    if (windows) {
      // Pull the runtime entry from its archive even when user code calls no
      // runtime adapter. Its object supplies the CRT default-library directives.
      cmd = "link /nologo /subsystem:console /stack:8388608 /entry:wmainCRTStartup /include:wmain " + quote(path_text(objpath)) + " " + quote(rt) +
            " legacy_stdio_definitions.lib /out:" + quote(path_text(exe));
    } else {
      cmd = "cc " + quote(path_text(objpath)) + " " + quote(rt) + " -pthread -o " + quote(path_text(exe));
#if defined(__linux__)
      cmd += " -no-pie";
#endif
      if (options.target.os == codegen::TargetOs::MacOs) cmd += " -arch x86_64";
    }
    if (options.verbose) std::fprintf(stderr, "link: %s\n", cmd.c_str());
    rc = system_utf8(cmd);
  }
  if (rc != 0) {
    std::fprintf(stderr, "linking failed (exit %d)\n", rc);
    return result;
  }
  if (!options.keep_object) {
    std::error_code ec;
    fs::remove(objpath, ec);
  }
  result.ok = true;
  result.executable = path_text(exe);
  result.object = path_text(objpath);
  return result;
}

int run_executable(const std::string& path) {
  return launch(fs::absolute(utf8_path(path)), {});
}

}  // namespace synx
