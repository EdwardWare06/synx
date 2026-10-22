#include <string>
#include "synx/compile.hpp"
#include "test_support.hpp"

static void check(const char* label, const std::string& source, const std::string& expected = "") {
  auto c = synx::check_source("threads.snx", "import std.thread; import std.sync.{Arc, Mutex, MutexGuard, Condvar, AtomicUsize, Cell}; import std.channel; " + source);
  if (!c->sink.has_errors() && !c->prelude_sink.has_errors()) c->run_lowering();
  std::string errors = c->sink.render(c->user_file) + c->prelude_sink.render(c->prelude_file);
  ++synx_test::g_checks;
  if ((expected.empty() && (c->sink.has_errors() || c->prelude_sink.has_errors())) ||
      (!expected.empty() && errors.find(expected) == std::string::npos)) {
    ++synx_test::g_failures;
    std::printf("FAIL %s: expected '%s'\n%s", label, expected.c_str(), errors.c_str());
  }
}
int main() {
  check("owned captures", R"(main() { var text = String.from("x"); thread.spawn(send once || { print(text); }); })");
  check("contextual qualification", "main() { int n = 3; thread.spawn(once || n); }");
  check("named function", "int job() { return 3; } main() { thread.spawn(job); }");
  check("send is still an identifier", "int send(int n) { return n; } main() { int send = 4; int n = send; }");
  check("send bitwise expression", "main() { int send = 1; int n = send | 2; }");
  check("send boolean expression", "main() { bool send = true; bool n = send || false; bool m = false || send || false; }");
  check("send function storage", "fn once send() -> int make() { return send once || 7; } main() { var f = make(); thread.spawn(f); }");
  check("send qualifier can be erased", "main() { fn once() -> int f = send once || 4; int n = f(); }");
  check("erased qualifier cannot be restored", "main() { fn once() -> int f = once || 4; thread.spawn(f); }", "type mismatch");
  check("returned local function rejected", "fn once() -> int local() { return once || 1; } main() { thread.spawn(local()); }", "type mismatch");
  check("borrow capture", "main() { int n = 3; thread.spawn([&const n] send once || n); }", "does not implement 'Owned'");
  check("moved reference capture", "main() { int n = 3; var r = &const n; thread.spawn(send once || *r); }", "does not implement 'Owned'");
  check("reference output", "main() { thread.spawn(send once || \"borrowed\"); }", "does not implement 'Owned'");
  check("raw capture", "main() { var p = 0 as *int; thread.spawn(send once || { usize n = p as usize; }); }", "does not implement 'Send'");
  check("raw result", "main() { thread.spawn(send once || 0 as *int); }", "does not implement 'Send'");
  check("local task capture", "async int job() { return 2; } main() { var task = job(); thread.spawn(send once || { var owned = task; }); }", "does not implement 'Send'");
  check("atomic wake capture", "main() { var wake = Waker.new(); thread.spawn(send once || wake.wake()); }");
  check("erased callback capture", "main() { var callback = once || 1; thread.spawn(send once || callback()); }", "does not implement 'Send'");
  check("function aggregate", "struct S { fn once() callback; } main() { var s = S { callback: once || {} }; thread.spawn(send once || s); }", "does not implement 'Send'");
  check("dyn capture", "trait A {} struct S {} impl A for S {} main() { var s = S {}; &const dyn A a = s; thread.spawn(send once || { var moved = a; }); }", "does not implement 'Send'");
  check("recursive owners", "enum List { End, Link(int, Box<List>) } void sendable<T: Send + Sync + Owned>() {} main() { sendable<List>(); }");
  check("nested borrowed owner", "struct S { Vec<(&const int)?> refs; } void owned<T: Owned>() {} main() { owned<S>(); }", "does not implement 'Owned'");
  check("shared integer reference traits", "void transfer<T: Send + Sync>() {} main() { transfer<&const int>(); transfer<&int>(); }");
  check("shared cell reference rejected", "void transfer<T: Send>() {} main() { transfer<&const Cell<int>>(); }", "does not implement 'Send'");
  check("exclusive cell transfer", "void transfer<T: Send>() {} main() { transfer<&Cell<int>>(); }");
  check("cell transfer", "main() { var c = Cell.new(2); thread.spawn(send once || c.get()); }");
  check("shared cell rejected", "main() { var c = Arc.new(Cell.new(2)); thread.spawn(send once || c.get().get()); }", "does not implement 'Send'");
  check("mutex permits cell", "void valid<T: Send + Sync + Owned>() {} main() { valid<Arc<Mutex<Cell<int>>>>(); }");
  check("guard cannot transfer", "void valid<T: Send>() {} main() { valid<MutexGuard<int>>(); }", "does not implement 'Send'");
  check("guard cannot be owned", "void valid<T: Owned>() {} main() { valid<MutexGuard<int>>(); }", "does not implement 'Owned'");
  check("guard holds mutex", "main() { var mutex = Mutex.new(1); var guard = mutex.lock(); var moved = mutex; print(\"{*guard.get()}\"); }", "while it is borrowed");
  check("guard keeps value borrowed", "void discard<T>(T value) {} main() { var mutex = Mutex.new(1); var guard = mutex.lock(); var value = guard.get_mut(); discard(guard); *value = 2; }", "while it is borrowed");
  check("wait needs exclusive guard", "main() { var m = Mutex.new(1); var c = Condvar.new(); var g = m.lock(); var value = g.get(); c.wait(&g); print(\"{*value}\"); }", "already borrowed");
  check("guard output cannot escape", "MutexGuard<int> bad() { var m = Mutex.new(1); return m.lock(); }", "this returns a reference");
  check("marker impl requires unsafe", "struct S { *byte raw; } impl Send for S {}", "require unsafe impl");
  check("unsafe contract accepted", "struct S { *byte raw; } unsafe impl Send for S {} unsafe impl Owned for S {} main() { var s = S { raw: 0 as *byte }; thread.spawn(send once || s); }");
  check("unsafe non-marker impl rejected", "struct S {} trait A {} unsafe impl A for S {}", "unsafe impl requires");
  check("generic captures need bounds", "void launch<T>(T x) { thread.spawn(send once || x); }", "does not implement 'Send'");
  check("generic owned captures", "void launch<T: Send + Owned>(T x) { thread.spawn(send once || x); }");
  check("atomics are move only", "main() { var a = AtomicUsize.new(0); var b = a; a.load(); }", "moved value");
  check("atomics cannot copy through shared borrow", "main() { var a = AtomicUsize.new(0); var r = &const a; var b = *r; }", "out of a reference");
  check("channel traits", "void valid<T: Send + Sync + Owned>() {} main() { valid<channel.Sender<String>>(); valid<channel.Receiver<String>>(); }");
  check("channel result owns its references", "void valid<T: Owned>() {} main() { valid<channel.Sender<&const int>>(); }", "does not implement 'Owned'");
  check("cell cannot retain borrowed storage", "main() { int n = 1; var c = Cell.new(&const n); }", "does not implement 'Owned'");
  check("mutex tracks stored loans", "main() { int anchor = 1; var m = Mutex.new(&const anchor); { int local = 2; var g = m.lock(); *g.get_mut() = &const local; } { var g = m.lock(); print(\"{**g.get()}\"); } }", "does not live past this scope");
  check("shared ownership excludes borrowed payloads", "main() { var a = Arc.new(Mutex.new(Vec<&const int>.new())); var b = a.clone(); }", "does not implement 'Owned'");
  check("map transfer bounds", "import std.collections.HashMap; void valid<T: Send + Sync + Owned>() {} main() { valid<HashMap<int,String>>(); }");
  check("map retains borrowed type", "import std.collections.HashMap; void valid<T: Owned>() {} main() { valid<HashMap<int,&const int>>(); }", "does not implement 'Owned'");
  check("file transfer", "import std.fs.File; void valid<T: Send + Owned>() {} main() { valid<File>(); }");
  check("file is not shared", "import std.fs.File; void valid<T: Sync>() {} main() { valid<File>(); }", "does not implement 'Sync'");
  check("nested closure captures are checked", "main() { int n = 1; var r = &const n; thread.spawn(send once || { var inner = once || *r; return inner(); }); }", "does not implement 'Owned'");
  check("send callbacks are not Sync", "void shared<T: Sync>() {} main() { shared<fn const send()>(); }", "does not implement 'Sync'");
  return synx_test::finish("threads_test");
}
