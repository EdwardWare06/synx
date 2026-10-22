// End to end tests: build each tests/run/*.snx with the synx executable, run
// it, and compare stdout plus exit code with the .expected file. A nonzero
// exit code is written as a first line "exit N"; the rest is the exact stdout.
// A file without that line expects exit code 0.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>
#include <vector>
#include <chrono>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/wait.h>
#endif

#include "test_support.hpp"

namespace fs = std::filesystem;

static std::string read_file(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

static std::string quote(const std::string& s) { return "\"" + s + "\""; }

static int command(const std::string& text) {
#if defined(_WIN32)
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (!n) return -1;
  std::wstring wide(n, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), n);
  return _wsystem(wide.c_str());
#else
  return std::system(text.c_str());
#endif
}

static std::string normalize_line_endings(const std::string& text) {
  std::string result;
  result.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;
    result += text[i];
  }
  return result;
}

int main(int argc, char** argv) {
  CHECK(normalize_line_endings("a\r\nb\nc\rd\r") == "a\nb\nc\rd\r");
  CHECK(normalize_line_endings("\r\r\n") == "\r\n");
  if (argc < 3) {
    std::puts("usage: test_run <synx executable> <tests/run directory>");
    return 1;
  }
  std::string synx = fs::absolute(argv[1]).string();
  fs::path dir = fs::absolute(argv[2]);
  bool stack_only = false, release = false;
  for (int i = 3; i < argc; ++i) {
    if (std::string(argv[i]) == "--no-regalloc") stack_only = true;
    else if (std::string(argv[i]) == "--release") release = true;
    else { std::printf("unknown native-test option: %s\n", argv[i]); return 1; }
  }
  auto nonce = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  fs::path tmp = fs::temp_directory_path() / ("synx_run_tests_" + dir.filename().string() + (stack_only ? "_stack_" : "_register_") + std::to_string(nonce));
  fs::create_directories(tmp);

  std::vector<fs::path> sources;
  for (auto& e : fs::directory_iterator(dir)) {
    if (e.path().extension() == ".snx") sources.push_back(e.path());
    else if (e.is_directory() && fs::is_regular_file(e.path() / "main.snx")) sources.push_back(e.path() / "main.snx");
  }
  std::sort(sources.begin(), sources.end());

  for (auto& src : sources) {
    ++synx_test::g_checks;
    std::string case_name = src.filename() == "main.snx" ? src.parent_path().filename().string() : src.stem().string();
    bool case_release = release;
    fs::path flags_path = src; flags_path.replace_extension(".flags");
    std::istringstream flags(read_file(flags_path));
    bool valid_flags = true;
    for (std::string flag; flags >> flag;) {
      if (flag == "--release") case_release = true;
      else { std::printf("FAIL %s: unknown build flag '%s'\n", case_name.c_str(), flag.c_str()); valid_flags = false; }
    }
    if (!valid_flags) { ++synx_test::g_failures; continue; }
    fs::path expected_path = src;
    expected_path.replace_extension(".expected");
    fs::path release_expected = src; release_expected.replace_extension(".release.expected");
    if (case_release && fs::is_regular_file(release_expected)) expected_path = release_expected;
    std::string expected = read_file(expected_path);
    int want_exit = 0;
    std::string want_out = expected;
    if (expected.rfind("exit ", 0) == 0) {
      size_t nl = expected.find('\n');
      want_exit = std::atoi(expected.c_str() + 5);
      want_out = nl == std::string::npos ? "" : expected.substr(nl + 1);
    }
#if defined(_WIN32)
    fs::path exe = tmp / (case_name + ".exe");
#else
    fs::path exe = tmp / case_name;
#endif
    fs::path out_file = tmp / (case_name + ".stdout");
    fs::path err_file = tmp / (case_name + ".stderr");
    std::string build = quote(synx) + " build " + quote(src.string()) + " -o " + quote(exe.string());
    if (stack_only) build += " --no-regalloc";
    if (case_release) build += " --release";
#if defined(_WIN32)
    // cmd.exe drops the outer quotes of a command that starts with a quote.
    build = "cmd /c " + quote(build);
#endif
    int rc = command(build);
    if (rc != 0) {
      ++synx_test::g_failures;
      std::printf("FAIL %s: build failed\n", src.filename().string().c_str());
      continue;
    }
    std::string run = quote(exe.string()) + " > " + quote(out_file.string()) + " 2> " + quote(err_file.string());
    fs::path args_path = src; args_path.replace_extension(".args");
    std::istringstream args(read_file(args_path));
    for (std::string arg; std::getline(args, arg);) {
      if (!arg.empty() && arg.back() == '\r') arg.pop_back();
      run += " " + quote(arg);
    }
    fs::path stdin_path = src; stdin_path.replace_extension(".stdin");
    if (fs::is_regular_file(stdin_path)) run += " < " + quote(stdin_path.string());
#if defined(_WIN32)
    run = "cmd /c \"" + run + "\"";
#endif
    // Each case has a private working folder for file I/O fixtures.
    fs::path work = tmp / (case_name + "_files");
    fs::create_directories(work);
    fs::path previous = fs::current_path();
    fs::current_path(work);
    rc = command(run);
    fs::current_path(previous);
#if !defined(_WIN32)
    if (rc != -1 && WIFEXITED(rc)) rc = WEXITSTATUS(rc);
#endif
    std::string got = read_file(out_file);
    // Normalize Windows line endings on both sides: git may check the
    // expected files out with CRLF on Windows.
    std::string norm = normalize_line_endings(got);
    want_out = normalize_line_endings(want_out);
    fs::path stderr_path = src; stderr_path.replace_extension(".stderr");
    fs::path release_stderr = src; release_stderr.replace_extension(".release.stderr");
    if (case_release && fs::is_regular_file(release_stderr)) stderr_path = release_stderr;
    std::string want_err = normalize_line_endings(read_file(stderr_path));
    while (!want_err.empty() && want_err.back() == '\n') want_err.pop_back();
    std::string got_err = normalize_line_endings(read_file(err_file));
    bool err_ok = !fs::is_regular_file(stderr_path) ||
                  (want_err.empty() ? got_err.empty() : got_err.find(want_err) != std::string::npos);
    if (norm != want_out || rc != want_exit || !err_ok) {
      ++synx_test::g_failures;
      std::printf("FAIL %s: exit %d (want %d)\n--- got ---\n%s--- want ---\n%s---\n",
                  src.filename().string().c_str(), rc, want_exit, norm.c_str(), want_out.c_str());
      std::printf("--- stderr ---\n%s--- expected stderr fragment ---\n%s\n", got_err.c_str(), want_err.c_str());
    }
  }
  fs::remove_all(tmp);
  return synx_test::finish("run_test");
}
