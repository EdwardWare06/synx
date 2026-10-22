// Deterministic wake regression for all three synchronous task drivers.
// The private runtime hook emits a notification after a false event read and
// before the reference-count read. Revert only the fix in a copied std tree to
// prove that the fixture detects the old behavior. Never modify source inputs.
#include "../selfhost_support.hpp"
#include <stdexcept>

using namespace selfhost_test;

static fs::path from_utf8(const char* value) {
  std::u8string text;
  for (; *value; ++value) text += static_cast<char8_t>(static_cast<unsigned char>(*value));
  return fs::path(text);
}

static void write(const fs::path& path, const std::string& data) {
  std::ofstream out(path, std::ios::binary);
  out << data;
  if (!out) throw std::runtime_error("cannot write " + utf8(path));
}

static std::string old_task(std::string text) {
  // Git may check out the source with CRLF. Replacement uses LF consistently.
  std::string normalized;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;
    normalized += text[i];
  }
  text = std::move(normalized);
  const std::string old =
      "        if (!wake.take_ready()) {\n"
      "            if (!wake.is_retained()) { panic(\"local executor has no ready task\"); }\n"
      "            wake.wait(); continue;\n"
      "        }\n";
  size_t cursor = 0;
  for (int i = 0; i < 2; ++i) {
    const auto start = text.find("        if (!wake.take_ready()) {", cursor);
    if (start == std::string::npos) throw std::runtime_error("missing block_on readiness check");
    const auto end = text.find("        match task.poll", start);
    if (end == std::string::npos) throw std::runtime_error("missing block_on poll");
    text.replace(start, end - start, old);
    cursor = start + old.size();
  }
  const std::string fixed =
      "                if (!retained) {\n"
      "                    if (!self.signal.take_ready()) { panic(\"local executor has no ready task\"); }\n"
      "                } else { self.signal.wait(); }";
  const auto start = text.find(fixed);
  if (start == std::string::npos) throw std::runtime_error("missing LocalExecutor readiness check");
  text.replace(start, fixed.size(),
      "                if (!retained) { panic(\"local executor has no ready task\"); }\n"
      "                self.signal.wait();");
  return text;
}

int main(int argc, char** argv) {
  if (argc != 5) {
    std::puts("usage: test_task_wake_race <compiler> <root> <hook-runtime> <mode>");
    return 1;
  }
  fs::path work;
  try {
    const auto compiler = fs::absolute(from_utf8(argv[1]));
    const auto root = fs::absolute(from_utf8(argv[2]));
    const auto runtime = fs::absolute(from_utf8(argv[3]));
    const int mode = std::stoi(argv[4]);
    if (mode < 0 || mode > 3) throw std::runtime_error("invalid mode");
    const auto nonce = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    work = fs::temp_directory_path() / ("synx_wake_race_" + std::to_string(mode) + "_" + std::to_string(nonce));
    fs::create_directories(work);
    const auto baseline = work / "baseline-std";
    fs::copy(root / "std", baseline, fs::copy_options::recursive);
    write(baseline / "task.snx", old_task(read(baseline / "task.snx")));
    const char* cases[] = {"send", "local", "executor", "silent-send", "silent-local", "silent-executor"};
    for (int old = 1; old >= 0; --old) {
#if defined(_WIN32)
      const auto program = work / (old ? "baseline.exe" : "fixed.exe");
#else
      const auto program = work / (old ? "baseline" : "fixed");
#endif
      std::vector<std::string> arguments{"build", utf8(root / "tests/task_wake_race/probe.snx"), "-o", utf8(program)};
      if (mode & 1) arguments.push_back("--no-regalloc");
      if (mode & 2) arguments.push_back("--release");
      Environment environment{{"SYNX_STD_PATH", utf8(old ? baseline : root / "std")},
                              {"SYNX_RUNTIME_LIB", utf8(runtime)}};
      const auto log = work / "build.log";
      if (run(compiler, arguments, log, environment) != 0) throw std::runtime_error("probe build failed:\n" + read(log));
      for (int i = 0; i < 6; ++i) {
        const bool panic = old || i >= 3;
        const auto output = work / (std::string(old ? "baseline-" : "fixed-") + cases[i] + ".log");
        const int status = run(program, {cases[i]}, output);
        const auto text = read(output);
        if (status != (panic ? 101 : 0) ||
            (panic ? text.find("local executor has no ready task") == std::string::npos : !text.empty())) {
          throw std::runtime_error(std::string(cases[i]) + " exited " + std::to_string(status) + ":\n" + text);
        }
      }
    }
    fs::remove_all(work);
    std::puts("task wake race: baseline failures and fixed handoffs/diagnostics pass");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\nartifacts: %s\n", error.what(), utf8(work).c_str());
    return 1;
  }
}
