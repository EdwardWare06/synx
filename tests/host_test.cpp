// Host integration through Unicode paths and native environments.
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/wait.h>
#include <spawn.h>
#include <unistd.h>
#include <fcntl.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#else
extern char** environ;
#endif
#endif
#include "test_support.hpp"
namespace fs = std::filesystem;
static std::string utf8(const fs::path& path) {
  auto bytes = path.u8string();
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
#if defined(_WIN32)
static std::wstring wide(const std::string& text) {
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(n, L'\0');
  if (n) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), n);
  return out;
}
#endif
static void environment(const char* key, const std::string* value) {
#if defined(_WIN32)
  auto name = wide(key);
  auto data = value ? wide(*value) : std::wstring();
  CHECK(SetEnvironmentVariableW(name.c_str(), value ? data.c_str() : nullptr) != 0);
#else
  CHECK((value ? setenv(key, value->c_str(), 1) : unsetenv(key)) == 0);
#endif
}
static int run(const fs::path& program, const std::vector<std::string>& args, const fs::path& log) {
#if defined(_WIN32)
  // These arguments are fixed flags and file paths; none ends in a backslash.
  std::wstring line = L"\"" + program.wstring() + L"\"";
  for (const auto& arg : args) line += L" \"" + wide(arg) + L"\"";
  SECURITY_ATTRIBUTES security{sizeof security, nullptr, TRUE};
  HANDLE output = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, 0, nullptr);
  if (output == INVALID_HANDLE_VALUE) return -1;
  STARTUPINFOW startup{};
  startup.cb = sizeof startup; startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = output; startup.hStdError = output; startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION process{};
  BOOL started = CreateProcessW(program.c_str(), line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process);
  CloseHandle(output);
  if (!started) return -1;
  DWORD code = 0;
  bool ok = WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread); CloseHandle(process.hProcess);
  return ok ? static_cast<int>(code) : -1;
#else
  std::string program_text = utf8(program), log_text = utf8(log);
  std::vector<char*> arguments{program_text.data()};
  for (const auto& arg : args) arguments.push_back(const_cast<char*>(arg.c_str()));
  arguments.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions)) return -1;
  int error = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log_text.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (!error) error = posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
  pid_t child = 0;
#if defined(__APPLE__)
  char** environment = *_NSGetEnviron();
#else
  char** environment = environ;
#endif
  if (!error) error = posix_spawn(&child, program_text.c_str(), &actions, nullptr, arguments.data(), environment);
  posix_spawn_file_actions_destroy(&actions);
  if (error) return -1;
  int code = 0;
  while (waitpid(child, &code, 0) < 0) { if (errno != EINTR) return -1; }
  return WIFEXITED(code) ? WEXITSTATUS(code) : -1;
#endif
}
static std::string read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out; out << in.rdbuf(); return out.str();
}
int main(int argc, char** argv) {
  if (argc != 3) return 1;
  fs::path compiler = fs::absolute(argv[1]), source = fs::absolute(argv[2]);
  auto previous = fs::current_path();
  auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  auto root = fs::temp_directory_path() / ("synx-host-" + std::to_string(nonce));
  fs::create_directories(root);
  auto build_log = root / "build.log";
  auto initial = root / "initial.exe";
  CHECK(run(compiler, {"build", utf8(source), "-o", utf8(initial)}, build_log) == 0);
  if (!fs::exists(initial)) { std::fputs(read(build_log).c_str(), stderr); return 1; }
  auto folder = root / fs::path(u8"space & café λ");
#if !defined(_WIN32)
  folder /= std::string(100, 'p'); folder /= std::string(100, 'q');
#endif
  fs::create_directories(folder);
  auto executable = folder / fs::path(u8"host café λ.exe");
  fs::copy_file(initial, executable);
  fs::permissions(executable, fs::status(initial).permissions());
  // A built-in-linked program loads the runtime capsule from its own folder.
  for (const char* capsule : {"synx_rt.dll", "libsynx_rt.so", "libsynx_rt.dylib"})
    if (fs::is_regular_file(root / capsule)) fs::copy_file(root / capsule, folder / capsule);
  fs::current_path(folder);
  std::ofstream("identity-a") << "identity";
  fs::create_hard_link("identity-a", "identity-b");
  std::error_code error;
  fs::create_symlink("identity-a", "identity-link", error);
  bool symlink = !error;
  if (!symlink) fs::create_hard_link("identity-a", "identity-link");
  std::string one = "1", empty, unicode_name = "unicode name";
  std::string long_value = "caf\xc3\xa9 \xce\xbb " + std::string(5000, 'x');
  environment("SYNX_HOST_CAFÉ_λ", &unicode_name);
  environment("SYNX_HOST_TEST_VALUE", &long_value);
  environment("SYNX_HOST_TEST_EMPTY", &empty);
  environment("SYNX_HOST_TEST_ABSENT_793141", nullptr);
  environment("SYNX_HOST_TEST_SYMLINK", symlink ? &one : nullptr);
  auto log = root / "run.log";
  for (const auto& args : {std::vector<std::string>{}, std::vector<std::string>{"--environment"}}) {
    int rc = run(executable, args, log);
    CHECK(rc == 0);
    if (rc) std::fputs(read(log).c_str(), stderr);
  }
#if !defined(_WIN32)
  std::string bad(1, static_cast<char>(0xff));
  environment("SYNX_HOST_TEST_BAD", &bad);
  CHECK(run(executable, {"--invalid-utf8"}, log) == 0);
#endif
  fs::current_path(previous);
  // Keep failed-test artifacts in the system temporary directory.
  return synx_test::finish("host_test");
}
