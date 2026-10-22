#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "synx/compile.hpp"
#include "test_support.hpp"
static bool trace = false;
// With SYNX_CASE_DIR set, each case source is also written to
// <dir>/case-<n>/main.snx; tests/selfhost_modules_test.cpp compares them.
static void export_case(const std::string& src) {
  static int count = 0;
  const char* dir = std::getenv("SYNX_CASE_DIR");
  if (!dir || !*dir) return;
  auto folder = std::filesystem::u8path(dir) / ("case-" + std::to_string(count++));
  std::filesystem::create_directories(folder);
  std::ofstream out(folder / "main.snx", std::ios::binary);
  out.write(src.data(), static_cast<std::streamsize>(src.size()));
}
static void check(const char* label, const std::string& source, const std::string& expected = "") {
  if (trace) { std::printf("%s\n", label); std::fflush(stdout); }
  const std::string full = "import std.task.{yield_now,block_on_send}; import std.sync.{Arc,Mutex,Cell,AtomicUsize}; " + source;
  export_case(full);
  auto c = synx::check_source("async_pool.snx", full);
  if (!c->sink.has_errors() && !c->prelude_sink.has_errors()) c->run_lowering();
  std::string errors = c->sink.render(c->user_file) + c->prelude_sink.render(c->prelude_file);
  ++synx_test::g_checks;
  if ((expected.empty() && (c->sink.has_errors() || c->prelude_sink.has_errors())) || (!expected.empty() && errors.find(expected) == std::string::npos)) {
    ++synx_test::g_failures;
    std::printf("FAIL %s: expected '%s'\n%s",label,expected.c_str(),errors.c_str());
    std::fflush(stdout);
  }
}
int main(int argc, char**) {
  trace = argc > 1;
  check("send async return", "async send int job() { await yield_now(); return 4; } main() { SendTask<int> task = job(); }");
  check("owned input", "async send String job(String text) { await yield_now(); return text; }");
  check("borrowed input excluded", "async send int job(&const int input) { await yield_now(); return *input; }", "send async input");
  check("borrowed output excluded", "async send &const str job() { return \"text\"; }", "send async output");
  check("generic input bounds", "async send T job<T>(T value) { return value; }", "does not implement 'Send'");
  check("generic send task", "async send T job<T: Send + Owned>(T value) { await yield_now(); return value; } main() { var task = job(String.from(\"owned\")); }");
  check("owned receiver", "struct Job { int n; async send int run(self) { await yield_now(); return self.n; } } main() { var j = Job { n: 2 }; var t = j.run(); }");
  check("borrowed receiver excluded", "struct Job { int n; async send int run(&self) { await yield_now(); return self.n; } }", "send async input");
  check("send named function preserved", "async send() {} main() { Task<void> t = send(); }");
  check("raw pointer suspension", "async send usize job() { var p = 0 as *int; await yield_now(); return p as usize; }", "SNX0420");
  check("guard retained for use", "async send int job() { var m = Mutex.new(3); var g = m.lock(); await yield_now(); return *g.get(); }", "SNX0420");
  check("guard retained for drop", "async send void job() { var m = Mutex.new(3); var g = m.lock(); await yield_now(); }", "SNX0420");
  check("transient guard allowed", "async send int job() { var m = Mutex.new(3); int n = 0; { var g = m.lock(); n = *g.get(); } await yield_now(); return n; }");
  check("self borrowed cell", "async send int job() { var c = Cell.new(3); var r = &const c; await yield_now(); return r.get(); }");
  check("self borrowed aggregate", "struct Borrow { &const Cell<int> c; } async send int job() { var c = Cell.new(3); var r = Borrow { c: &const c }; await yield_now(); return r.c.get(); }");
  check("self borrowed vector", "async send int job() { var c = Cell.new(3); var r = Vec<&const Cell<int>>.new(); r.push(&const c); await yield_now(); return r[0].get(); }");
  check("non-send root through reborrow", "async send int job() { var c = Arc.new(Cell.new(3)); var a = c.get(); var r = &const *a; await yield_now(); return r.get(); }", "SNX0420");
  check("forged non-sync reference", "async send int job() { &const Cell<int> r = unsafe { &const *(123 as *Cell<int>) }; await yield_now(); return r.get(); }", "SNX0420");
  check("local task excluded", "async int local() { return 1; } async send int job() { return await local(); }", "SNX0420");
  check("plain closure excluded across await", "async send int job() { var f = || 3; await yield_now(); return f(); }", "SNX0420");
  check("send closure retained", "async send int job() { var f = send once || 3; await yield_now(); return f(); }");
  check("dead closure allowed", "async send int job() { int n = 0; { var f = || 3; n = f(); } await yield_now(); return n; }");
  check("wake is shared", "void allowed<T: Send + Sync + Owned>() {} main() { allowed<Waker>(); }");
  check("send task cannot be shared", "void allowed<T: Sync>() {} main() { allowed<SendTask<int>>(); }", "does not implement 'Sync'");
  check("local task still excluded", "void allowed<T: Send>() {} main() { allowed<Task<int>>(); }", "does not implement 'Send'");
  check("wrong wake receiver", "struct W { int value; } impl Wake for W { void wake(&self) { self.value += 1; } }", "SNX0419");
  check("missing wake receiver", "struct W {} impl Wake for W { void wake(&const W value) {} }", "SNX0419");
  check("wrong future receiver", "struct F {} impl Future<int> for F { Poll<int> poll(&const self,Waker w) { return Poll.Ready(1); } }", "SNX0419");
  check("wake callback contract", "struct W {} impl Wake for W { void wake(&const self) {} } main() { var w = Waker.from(W {}); }");
  check("wake payload must sync", "struct W { Cell<int> c; } impl Wake for W { void wake(&const self) { self.c.set(1); } } main() { var w = Waker.from(W { c: Cell.new(0) }); }", "does not implement 'Sync'");
  check("wake payload must own", "struct W { &const int p; } impl Wake for W { void wake(&const self) {} } main() { int x = 0; var w = Waker.from(W { p: &const x }); }", "does not implement 'Owned'");
  check("private frame cannot be forged", "main() { SendTask<int> task = SendTask { frame: 0 as *byte }; }", "private to module");
  check("blocking closure must own", "import std.task.pool.BlockingPool; main() { int n = 1; var r = &const n; match BlockingPool.new(1) { Ok(pool) => { pool.spawn(send once || *r); }, Err(_) => {} } }", "does not implement 'Owned'");
  check("timer future can migrate", "import std.time.Sleep; void allowed<T: Send + Owned>() {} main() { allowed<Sleep>(); }");
  check("async file can migrate", "import std.asyncio.AsyncFile; void allowed<T: Send + Sync + Owned>() {} main() { allowed<AsyncFile>(); }");
  return synx_test::finish("async_pool_test");
}
