#pragma once
// Helpers shared by the self-hosting parity tests: process launch, file reads,
// and the little-endian report encoding.

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "synx/source.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#else
extern char** environ;
#endif
#endif

namespace selfhost_test {

namespace fs = std::filesystem;

inline std::string utf8(const fs::path& path) {
  auto bytes = path.u8string();
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

#if defined(_WIN32)
inline std::wstring wide(const std::string& text) {
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(n, L'\0');
  if (n) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), n);
  return out;
}
#endif

// Runs a program with fixed flags and file paths, writing its output to log.
// Arguments must contain no quotes and must not end in a backslash.
using Environment = std::vector<std::pair<std::string, std::string>>;
// Environment entries override inherited values for this child only; names in
// removed are absent from the child's environment.
inline int run(const fs::path& program, const std::vector<std::string>& args, const fs::path& log,
               const Environment& overrides = {}, const std::vector<std::string>& removed = {}) {
#if defined(_WIN32)
  struct CaseLess { bool operator()(const std::wstring& a, const std::wstring& b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; } };
  std::vector<wchar_t> block;
  if (!overrides.empty() || !removed.empty()) {
    std::map<std::wstring, std::wstring, CaseLess> values;
    wchar_t* inherited = GetEnvironmentStringsW();
    if (!inherited) return -1;
    for (const wchar_t* entry = inherited; *entry; entry += std::wcslen(entry) + 1) {
      std::wstring text(entry);
      const auto split = text.find(L'=', text[0] == L'=' ? 1 : 0);
      if (split != std::wstring::npos) values[text.substr(0, split)] = text.substr(split + 1);
    }
    FreeEnvironmentStringsW(inherited);
    for (auto& [key, value] : overrides) values[wide(key)] = wide(value);
    for (auto& key : removed) values.erase(wide(key));
    for (auto& [key, value] : values) {
      const auto entry = key + L'=' + value;
      block.insert(block.end(), entry.begin(), entry.end()); block.push_back(0);
    }
    block.push_back(0);
  }
  std::wstring line = L"\"" + program.wstring() + L"\"";
  for (const auto& arg : args) line += L" \"" + wide(arg) + L"\"";
  SECURITY_ATTRIBUTES security{sizeof security, nullptr, TRUE};
  HANDLE output = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, 0, nullptr);
  if (output == INVALID_HANDLE_VALUE) return -1;
  STARTUPINFOW startup{};
  startup.cb = sizeof startup;
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = output;
  startup.hStdError = output;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION process{};
  BOOL started = CreateProcessW(program.c_str(), line.data(), nullptr, nullptr, TRUE,
                               block.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT,
                               block.empty() ? nullptr : block.data(), nullptr, &startup, &process);
  CloseHandle(output);
  if (!started) return -1;
  DWORD code = 0;
  bool ok = WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
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
  std::vector<std::string> environment_storage;
  std::vector<char*> environment_pointers;
  if (!overrides.empty() || !removed.empty()) {
    std::map<std::string, std::string> values;
    for (char** entry = environment; *entry; ++entry) {
      const std::string text(*entry); const auto split = text.find('=');
      if (split != std::string::npos) values[text.substr(0, split)] = text.substr(split + 1);
    }
    for (auto& [key, value] : overrides) values[key] = value;
    for (auto& key : removed) values.erase(key);
    for (auto& [key, value] : values) environment_storage.push_back(key + '=' + value);
    for (auto& entry : environment_storage) environment_pointers.push_back(entry.data());
    environment_pointers.push_back(nullptr); environment = environment_pointers.data();
  }
  if (!error) error = posix_spawn(&child, program_text.c_str(), &actions, nullptr, arguments.data(), environment);
  posix_spawn_file_actions_destroy(&actions);
  if (error) return -1;
  int code = 0;
  while (waitpid(child, &code, 0) < 0) {
    if (errno != EINTR) return -1;
  }
  return WIFEXITED(code) ? WEXITSTATUS(code) : -1;
#endif
}

// Prints the seconds since the previous mark, so test logs show which phase
// dominates a long parity run.
struct PhaseTimer {
  std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
  void mark(const char* phase) {
    auto now = std::chrono::steady_clock::now();
    std::printf("phase %s: %.1fs\n", phase, std::chrono::duration<double>(now - last).count());
    std::fflush(stdout); last = now;
  }
};

inline std::string read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

inline void integer(std::string& out, uint64_t value, unsigned width) {
  for (unsigned i = 0; i < width; ++i) out += static_cast<char>(value >> (i * 8));
}

inline void blob(std::string& out, std::string_view value) {
  integer(out, value.size(), 8);
  out.append(value);
}

inline void span(std::string& out, synx::Span value) {
  integer(out, value.start, 4);
  integer(out, value.end, 4);
  integer(out, value.file, 4);
}

}  // namespace selfhost_test
