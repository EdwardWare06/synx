#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "synx/compile.hpp"
#include "synx/ir.hpp"
#include "test_support.hpp"

namespace {

// With SYNX_CASE_DIR set, each case source is also written to
// <dir>/case-<n>/main.snx; tests/selfhost_modules_test.cpp compares them.
void export_case(const std::string& src) {
  static int count = 0;
  const char* dir = std::getenv("SYNX_CASE_DIR");
  if (!dir || !*dir) return;
  auto folder = std::filesystem::u8path(dir) / ("case-" + std::to_string(count++));
  std::filesystem::create_directories(folder);
  std::ofstream out(folder / "main.snx", std::ios::binary);
  out.write(src.data(), static_cast<std::streamsize>(src.size()));
}

// Runs the front end, lowering, and borrow checking.
std::unique_ptr<synx::Compilation> run(const std::string& src) {
  export_case(src);
  auto c = synx::check_source("t.snx", src);
  if (!c->sink.has_errors()) c->run_lowering();
  return c;
}

void ok(const char* label, const std::string& src) {
  auto c = run(src);
  ++synx_test::g_checks;
  if (c->sink.has_errors()) {
    ++synx_test::g_failures;
    std::printf("FAIL %s: expected no errors, got:\n%s", label, c->sink.render(c->user_file).c_str());
  }
}

void err(const char* label, const std::string& src, const std::string& needle) {
  auto c = run(src);
  ++synx_test::g_checks;
  bool found = false;
  for (auto& d : c->sink.all()) {
    if (d.severity == synx::Severity::Error && d.message.find(needle) != std::string::npos) found = true;
  }
  if (!found) {
    ++synx_test::g_failures;
    std::printf("FAIL %s: expected an error containing '%s', got:\n%s", label, needle.c_str(),
                c->sink.has_errors() ? c->sink.render(c->user_file).c_str() : "  (no errors)\n");
  }
}

void test_moves() {
  const char* s = "struct S { int a; }\nimpl Drop for S { void drop(&self) { } }\nf(S s) { }\n";
  ok("move once", std::string(s) + "main() { var x = S { a: 1 }; f(x); }");
  err("use after move", std::string(s) + "main() { var x = S { a: 1 }; f(x); f(x); }", "use of moved value `x`");
  err("read after move", std::string(s) + "main() { var x = S { a: 1 }; f(x); int a = x.a; }", "use of moved value");
  ok("copy twice", "f(int x) { }\nmain() { int x = 1; f(x); f(x); }");
  ok("reassign after move", std::string(s) + "main() { var x = S { a: 1 }; f(x); x = S { a: 2 }; f(x); }");
  err("maybe moved", std::string(s) + "main(){ var x = S { a: 1 }; if (true) { f(x); } f(x); }", "may have been moved");
  err("move in loop", std::string(s) + "main(){ var x = S { a: 1 }; while (true) { f(x); } }", "moved");
  ok("move in loop with reassign", std::string(s) + "main(){ var x = S { a: 1 }; while (true) { f(x); x = S { a: 2 }; } }");
  err("uninit use", "main() { int x; int y = x; }", "used before it is assigned");
  ok("init on both paths", "main() { int x; if (true) { x = 1; } else { x = 2; } int y = x; }");
  err("init on one path", "main() { int x; if (true) { x = 1; } int y = x; }", "may not be assigned on every path");
  err("move out of ref", std::string(s) + "g(&const S r) { f(*r); }", "cannot move");
  ok("copy out of ref", "g(&const int r) { int x = *r; }");
  ok("string move then drop", "main() { String a = String.from(\"x\"); String b = a; }");
  ok("match by value moves", "f(String? o) { match o { Some(s) => { String t = s; } None => { } } }");
}

void test_loans() {
  const char* s = "struct S { int a; void bump(&self) { self.a += 1; } int get(&const self) { return self.a; } }\nimpl Drop for S { void drop(&self) { } }\n";
  ok("shared then use", std::string(s) + "main() { var x = S { a: 1 }; var r = &const x; int a = x.a; int b = r.get(); }");
  err("assign while shared", std::string(s) + "main() { var x = S { a: 1 }; var r = &const x; x.a = 2; int b = r.get(); }", "cannot assign to `x` while it is borrowed");
  ok("assign after last use", std::string(s) + "main() { var x = S { a: 1 }; var r = &const x; int b = r.get(); x.a = 2; }");
  err("read while mut", std::string(s) + "main() { var x = S { a: 1 }; var r = &x; int a = x.a; r.bump(); }", "while it is borrowed as writable");
  err("two mut", std::string(s) + "main() { var x = S { a: 1 }; var r = &x; var q = &x; r.bump(); q.bump(); }", "as writable while it is already borrowed");
  err("mut while shared", std::string(s) + "main() { var x = S { a: 1 }; var r = &const x; x.bump(); int b = r.get(); }", "as writable while it is already borrowed");
  ok("mut then shared after", std::string(s) + "main() { var x = S { a: 1 }; var r = &x; r.bump(); var q = &const x; int b = q.get(); }");
  err("move while borrowed", std::string(s) + "f(S s) { }\nmain() { var x = S { a: 1 }; var r = &const x; f(x); int b = r.get(); }", "cannot move `x` while it is borrowed");
  ok("two phase method", "main() { Vec<int> v = Vec.new(); v.push(v.len() as int); }");
  err("return ref to local", std::string(s) + "&const S f() { var x = S { a: 1 }; return &const x; }", "returns a reference to `x`");
  ok("return ref through param", std::string(s) + "&const int f(&const S s) { return &const s.a; }");
  ok("ref result borrows args", std::string(s) +
      "&const S pick(&const S a, &const S b) { return a; }\n"
      "main() { var x = S { a: 1 }; var y = S { a: 2 }; var r = pick(&const x, &const y); int v = r.get(); x.a = 3; }");
  err("ref result keeps args borrowed", std::string(s) +
      "&const S pick(&const S a, &const S b) { return a; }\n"
      "main() { var x = S { a: 1 }; var y = S { a: 2 }; var r = pick(&const x, &const y); x.a = 3; int v = r.get(); }", "while it is borrowed");
  ok("loop borrow each iteration", std::string(s) + "main() { var x = S { a: 1 }; for (i in 0..3) { var r = &x; r.bump(); } x.a = 0; }");
  ok("reborrow through ref", std::string(s) + "f(&S s) { var r = &const s.a; int v = *r; s.bump(); }");
  ok("examples style", "struct Inv { Vec<String> items; void add(&self, String n) { self.items.push(n); } }\n"
      "main() { var inv = Inv { items: Vec.new() }; inv.add(String.from(\"a\")); var r = &inv; r.add(String.from(\"b\")); print(\"{r.items.len()}\"); print(\"{inv.items.len()}\"); }");
}

void test_closures() {
  ok("call closure twice", "main() { int x = 1; var f = |int n| { x += n; x }; int a = f(2); int b = f(3); }");
  ok("return owned closure", "fn() -> int make() { int x = 3; return || x; } main() { var f = make(); int x = f(); }");
  ok("return captured borrow", "main() { int x = 3; var f = || &const x; var r = f(); int n = *r; }");
  err("capturing moves string", "main() { String text = String.from(\"x\"); var f = || text.len(); print(text); }", "use of moved value");
  err("moved function cannot be called", "main() { var f = || 1; var g = f; int x = f(); }", "use of moved value");
  err("Copy cannot duplicate closure field", "struct S { fn() -> int f; } impl Copy for S {} main() { var s = S { f: || 1 }; var t = s; var u = s; }", "cannot implement Copy");
  err("Copy cannot duplicate enum closure", "enum E { Callback(fn() -> int) } impl Copy for E {} main() { var e = E.Callback(|| 1); var f = e; var g = e; }", "cannot implement Copy");
  err("passing function moves it", "use(fn() -> int f) { int x = f(); } main() { var f = || 1; use(f); int x = f(); }", "use of moved value");
  err("capture already borrowed", "main() { String text = String.from(\"x\"); var r = &const text; var f = || text.len(); print(r); }", "while it is borrowed");
  ok("reference capture", "main() { int x = 1; var r = &const x; var f = || *r; }");
  ok("array reference capture", "main() { int x = 1; var refs = [&const x]; var f = || *refs[0]; }");
  ok("option reference capture", "main() { int x = 1; var r = Some(&const x); var f = || r.is_some(); }");
  ok("vector reference capture", "main() { int x = 1; Vec<&const int> refs = Vec.new(); refs.push(&const x); var f = || refs.len(); }");
  err("cannot consume captured ownership", "main() { String text = String.from(\"x\"); var f = || text; }", "cannot move");
  err("nested ownership capture", "main() { String text = String.from(\"x\"); var f = || { var g = || text.len(); g() }; }", "cannot move");
  err("const array callback", "main() { var fs = [|| 1]; var r = &const fs; int n = r[0](); }", "requires writable access");
  err("const field callback", "struct S { fn() -> int f; } main() { var s = S { f: || 1 }; var r = &const s; int n = r.f(); }", "requires writable access");
  ok("exclusive array callback", "main() { var fs = [|| 1]; var r = &fs; int n = r[0](); }");
  err("cannot move callee in arguments", "int take(fn(int) -> int f) { return 1; } main() { var f = |int n| n; int x = f(take(f)); }", "use of moved value");
  err("returned borrow prevents call", "main() { int x = 1; var f = || &const x; var r = f(); var s = f(); int n = *r; }", "while it is already borrowed");
  err("returned borrow prevents replacement", "main() { int x = 1; var f = || &const x; var r = f(); f = || &const x; int n = *r; }", "while it is borrowed");
  err("temporary closure borrow", "main() { int x = 1; var r = (|| &const x)(); int n = *r; }", "is dropped while it is still borrowed");
  err("closure borrow cannot escape owner", "&const int make() { int x = 1; var f = || &const x; return f(); }", "does not live past the function");
}

void test_closure_lifetimes() {
  ok("explicit shared capture", "main() { int x=1; var f=[&const x] || x; int n=f(); }");
  ok("explicit writable capture", "main() { int x=1; { var f=[&x] || { x+=1; x }; int n=f(); } x=3; }");
  err("shared capture blocks write", "main() { int x=1; var f=[&const x] || x; x=2; int n=f(); }", "borrowed");
  err("writable capture blocks read", "main() { int x=1; var f=[&x] || { x+=1; x }; int n=x; int m=f(); }", "borrowed");
  err("reference capture escapes local", "fn() -> int make() { int x=1; return [&const x] || x; }", "does not live past");
  err("reference local escapes through fn", "fn() -> int make() { int x=1; var r=&const x; return || *r; }", "does not live past");
  ok("returned reference parameter capture", "fn() -> int make(&const int x) { return || *x; } main() { int x=1; var f=make(&const x); int n=f(); }");
  err("returned closure retains caller loan", "fn() -> int make(&const int x) { return || *x; } main() { int x=1; var f=make(&const x); x=2; int n=f(); }", "borrowed");
  err("closure retained in box", "main() { Box<fn() -> int> b; { int x=1; b=Box.new([&const x] || x); } var f=b.into_inner(); int n=f(); }", "scope");
  err("closure retained in vector", "main() { Vec<fn() -> int> v=Vec.new(); { int x=1; v.push([&const x] || x); } usize n=v.len(); }", "scope");
  err("closure retained in option", "main() { Option<fn() -> int> v=None; { int x=1; v=Some([&const x] || x); } var f=v.unwrap(); int n=f(); }", "scope");
  err("closure retained in struct", "struct Holder { fn() -> int f; } main() { Holder h; { int x=1; h=Holder { f:[&const x] || x }; } int n=h.f(); }", "scope");
  err("nested reference cannot escape", "fn() -> fn() -> int make() { int x=1; var r=&const x; return || || *r; }", "does not live past");
  err("nested borrow of outer local", "fn() -> int make() { var f=|| { int x=1; [&const x] || x }; return f(); }", "does not live past");
  err("nested borrow of outer capture", "fn() -> int make() { int x=1; var outer=|| [&const x] || x; return outer(); }", "borrowed");
  err("closure stores call argument loan", "main() { int outer=1; var r=&const outer; var f=|&const int next| { int previous=*r; r=next; }; { int inner=2; f(&const inner); } f(&const outer); }", "scope");
  err("closure stores loan into captured container", "main() { Vec<&const int> refs=Vec.new(); var f=[&refs] |&const int r| { refs.push(r); }; { int inner=2; f(&const inner); } usize count=refs.len(); }", "scope");
  err("consuming borrowed capture stores local", "main() { Vec<&const int> refs=Vec.new(); var f=[&refs] once || { int local=1; refs.push(&const local); }; f(); usize n=refs.len(); }", "does not live past");
  err("owned reference aggregate stores loan", "struct H { &Vec<&const int> refs; } void store(H h, &const int r) { h.refs.push(r); } main() { Vec<&const int> refs=Vec.new(); { int n=1; store(H { refs:&refs }, &const n); } usize count=refs.len(); }", "scope");
  err("destructor retains stored reference", "struct G { &Vec<&const int> refs; &const int value; } impl Drop for G { void drop(&self) { self.refs.push(self.value); } } main() { Vec<&const int> refs=Vec.new(); { int n=1; { var g=G { refs:&refs, value:&const n }; } } usize count=refs.len(); }", "scope");
  ok("immutable consuming value", "main() { const f=once || 1; int n=f(); }");
  err("shared aggregate cannot write through exclusive field", "struct H { &int value; } void bad(&const H h) { *h.value=2; }", "shared reference");
  err("consuming opaque callback stores local", "main() { Vec<&const int> refs=Vec.new(); var inner=[&refs] |&const int r| { refs.push(r); }; var outer=once || { int n=1; inner(&const n); }; outer(); usize count=refs.len(); }", "does not live past");
  err("returned vector closure retains loan", "Vec<fn() -> int> make(&const int x) { Vec<fn() -> int> out=Vec.new(); out.push(|| *x); return out; } main() { Vec<fn() -> int> out; { int n=1; out=make(&const n); } usize count=out.len(); }", "scope");
  err("writable callback replacement retains loan", "import std.mem; void store(&fn() -> int f, &const int r) { var previous=mem.replace(f, || *r); } main() { var f=|| 0; { int n=1; store(&f,&const n); } int value=f(); }", "scope");
  ok("owned copied capture loses input loan", R"(fn() -> usize make(&const str text) { var owned=String.from(text); return || owned.len(); } main() { fn() -> usize f; { var text=String.from("copy"); f=make(text); } usize n=f(); })");
  err("heap fn return retains reference", R"(extern "C" { *byte malloc(usize size); } fn() -> int make(&const int x) { unsafe { var p=malloc(size_of<fn() -> int>()) as *fn() -> int; *p=|| *x; return *p; } } main() { fn() -> int f; { int n=1; f=make(&const n); } int value=f(); })", "scope");
  ok("popped callback borrow stays local", "main() { Vec<fn(int) -> int> v=Vec.new(); v.push(|int n| n+1); match v.pop() { Some(callback) => { int n=callback(1); } None => {} } }");
  ok("iterated callback borrow stays local", "struct H { fn() -> int f; } main() { Vec<H> v=Vec.new(); v.push(H { f:|| 1 }); for (h in &v) { int n=h.f(); } }");
  err("parameter reference alias stores local", "void f(&Vec<&const int> v) { var w=v; int n=1; w.push(&const n); }", "does not live past");
  err("parameter captured alias stores local", "void f(&Vec<&const int> v) { var callback=|&const int r| v.push(r); int n=1; callback(&const n); }", "does not live past");
  err("parameter ref slot alias stores local", "void f(&(&const int) p) { var q=p; int n=1; *q=&const n; }", "does not live past");
  err("parameter call-result alias stores local", "T id<T>(T value) { return value; } void f(&Vec<&const int> v) { var w=id(v); int n=1; w.push(&const n); }", "does not live past");
  err("parameter wrapper alias stores local", "struct H { &Vec<&const int> value; } void f(&Vec<&const int> v) { var h=H { value:v }; int n=1; h.value.push(&const n); }", "does not live past");
  err("consuming captured alias stores local", "main() { Vec<&const int> v=Vec.new(); var callback=[&v] once || { var w=&v; int n=1; w.push(&const n); }; callback(); }", "does not live past");
  ok("parameter reference alias reads", "usize f(&Vec<&const int> v) { var w=v; return w.len(); }");
  ok("shared nested calls", "main() { const f=const |int x| x+1; int n=f(f(1)); }");
  ok("shared call through ref", "int apply(&const fn const(int) -> int f) { return f(1); } main() { const f=const |int x| x+1; int n=apply(f); }");
  err("shared capture mutation", "main() { int x=1; var f=const || { x+=1; x }; }", "read only");
  err("shared borrowed mutation", "main() { int x=1; var r=&x; var f=const || { *r=2; }; }", "shared reference");
  err("shared captured field mutation", "struct S { &int r; } main() { int x=1; var s=S { r:&x }; var f=[&const s] || { *s.r=2; }; }", "shared reference");
  err("consuming call once", "main() { var f=once || 1; int a=f(); int b=f(); }", "moved");
  err("consuming call through ref", "int apply(&fn once() -> int f) { return f(); }", "must be owned");
  ok("consuming captured ownership", "main() { var text=String.from(\"x\"); var f=once || text; var result=f(); }");
  err("consuming returns owned capture ref", "main() { int x=1; var f=once || &const x; var r=f(); }", "does not live past");
  ok("explicit drop releases closure loan", "import std.mem; main() { int x=1; var f=[&x] || x; int n=f(); mem.drop(f); x=2; }");
  err("capture list is complete", "main() { int x=1; int y=2; var f=[x] || x+y; }", "not in the capture list");
}

void test_dyn() {
  const std::string s = "trait Read { int read(&const self); &const int view(&const self); } "
      "struct S { int n; } impl Read for S { int read(&const self) { return self.n; } "
      "&const int view(&const self) { return &const self.n; } } ";
  ok("dyn ends at last use", s + "main() { var x = S { n: 1 }; &const dyn Read r = x; int n = r.read(); x.n = 2; }");
  err("dyn preserves shared loan", s + "main() { var x = S { n: 1 }; &const dyn Read r = x; x.n = 2; int n = r.read(); }", "while it is borrowed");
  err("dyn preserves exclusive loan", s + "main() { var x = S { n: 1 }; &dyn Read r = &x; int n = x.n; int m = r.read(); }", "while it is borrowed as writable");
  err("dyn result preserves loan", s + "main() { var x = S { n: 1 }; &const dyn Read r = x; var p = r.view(); x.n = 2; int n = *p; }", "while it is borrowed");
  err("dyn local cannot escape", s + "&const dyn Read f() { var x = S { n: 1 }; return &const x; }", "returns a reference");
  err("dyn temporary cannot escape", s + "&const dyn Read f() { return S { n: 1 }; }", "does not live past the function");
  err("dyn vector preserves loan", s + "main() { var x = S { n: 1 }; Vec<&const dyn Read> v = Vec.new(); v.push(&const x); x.n = 2; int n = v[0].read(); }", "while it is borrowed");
  err("dyn generic erase local", s + "&const dyn Read erase<T: Read>(&const T t) { return t; } "
      "&const dyn Read f() { var x = S { n: 1 }; return erase(&const x); }", "returns a reference");
  err("dyn exclusive conversion moves reference", s + "main() { var x = S { n: 1 }; var r = &x; &dyn Read d = r; r.n = 2; int n = d.read(); }", "use of moved value");
  err("stored local cannot escape parameter", "void fill(&Vec<&const int> v) { int n = 1; v.push(&const n); }", "does not live past the function");
  err("assigned local cannot escape parameter", "void fill(&(&const int) p) { int n = 1; *p = &const n; }", "does not live past the function");
  err("returned local reference parameter address", "&const (&const int) f(&const int p) { return &const p; }", "does not live past the function");
  ok("return copied dereference", "int f() { int n = 1; var r = &const n; return *r; }");
  ok("write disjoint field through reference", "struct P { int a; int b; } void f(&P p) { var r = &p.a; p.b = 2; int n = *r; }");
  err("helper preserves stored loan", "void fill(&Vec<&const int> v, &const int n) { v.push(n); } "
      "main() { int x = 1; Vec<&const int> v = Vec.new(); fill(&v, &const x); x = 2; int n = *v[0]; }", "while it is borrowed");
  err("reference alias retains stored loan", "main() { int x = 1; Vec<&const int> v = Vec.new(); var p = &v; p.push(&const x); x = 2; int n = *v[0]; }", "while it is borrowed");
  err("assignment through reference retains loan", "main() { int x = 1; int y = 2; var a = Some(&const x); var p = &a; *p = Some(&const y); y = 3; bool b = a.is_some(); }", "while it is borrowed");
  ok("append transfers contents without source borrow", "void append(&Vec<&const int> a, &Vec<&const int> b) { "
      "match b.pop() { Some(v) => { a.push(v); } None => {} } } "
      "main() { int x = 1; int y = 2; Vec<&const int> a = Vec.new(); Vec<&const int> b = Vec.new(); "
      "b.push(&const x); append(&a, &b); b.push(&const y); int n = *a[0]; }");
  err("stored container reference keeps owner borrowed", "main() { int x = 1; Vec<&const int> b = Vec.new(); b.push(&const x); "
      "Vec<&const Vec<&const int>> a = Vec.new(); a.push(&const b); b.push(&const x); usize n = a.len(); }", "while it is already borrowed");
  err("dyn mismatched impl ABI", "trait T { int f(&const self, int n); } struct S {} "
      "impl T for S { int f(&const self, long n) { return 1; } } main() { var s = S {}; &const dyn T t = s; }", "does not match its trait signature");
}

void test_scope_ends() {
  err("reborrow alias does not extend local", "main() { &const int result; { int n=1; var alias=&const n; result=&const *alias; } int value=*result; }", "scope");
  ok("reference alias can end before external pointee", "&const int f(&const int value) { var alias=value; return &const *alias; }");
  err("inner scalar dies", "main() { &const int r; { int n = 1; r = &const n; } int value = *r; }", "does not live past this scope");
  err("inner aggregate dies", "struct S { int n; } main() { &const S r; { var s = S { n: 1 }; r = &const s; } int n = r.n; }", "does not live past this scope");
  err("vector holds block local", "main() { Vec<&const int> refs = Vec.new(); { int n = 1; refs.push(&const n); } int n = *refs[0]; }", "does not live past this scope");
  err("vector holds loop local", "main() { Vec<&const int> refs = Vec.new(); for (n in 0..3) { refs.push(&const n); } int n = *refs[0]; }", "scope");
  ok("borrow ends inside block", "main() { { int n = 1; var r = &const n; int value = *r; } }");
  ok("outer value survives inner ref", "main() { int n = 1; { var r = &const n; int value = *r; } n = 2; }");
  ok("temporary receiver scope", "struct S { int n; &const int get(&const self) { return &const self.n; } } main() { var r = S { n: 1 }.get(); int n = *r; }");
}

void test_boxes() {
  err("raw cast moves exclusive reference", "main() { int x=1; var r=&x; var p=r as *int; int n=*r; }", "moved");
  err("box move", "main() { var b = Box<int>.new(1); var c = b; int n = *b.as_ref(); }", "moved");
  err("box explicit Copy", "impl<T> Copy for Box<T> {} main() { var b = Box<int>.new(1); var c = b; int n = *b.as_ref(); }", "cannot implement Copy");
  err("box shared loan", "main() { var b = Box<int>.new(1); var r = b.as_ref(); *b.as_mut() = 2; int n = *r; }", "borrowed");
  err("box loan blocks extraction", "main() { var b = Box<int>.new(1); var r = b.as_ref(); int n = b.into_inner(); int m = *r; }", "borrowed");
  err("box contains local loan", "main() { Box<&const int> b; { int n = 1; b = Box.new(&const n); } int n = **b.as_ref(); }", "scope");
  err("box borrowed value escape", "&const int f() { var b = Box<int>.new(1); return b.as_ref(); }", "does not live past");
  ok("box loan ends", "main() { var b = Box<int>.new(1); { var r = b.as_ref(); int n = *r; }; *b.as_mut() = 2; }");
  ok("recursive value capture", "struct Node { Vec<Node> children; } main() { var n = Node { children: Vec<Node>.new() }; var f = || n.children.len(); usize count = f(); }");
  err("expanding owner cleanup", "struct Grow<T> { Box<Grow<Vec<T>>>? next; } main() { Grow<int> g = Grow { next: None }; }", "nested type arguments");
}

void test_self_references() {
  const std::string s = "struct S { int x; (&const int)? r; } ";
  err("store own field", s + "void init(&S owner) { owner.r=Some(&const owner.x); }", "same owner");
  err("store own field through alias", s + "void init(&S owner) { var alias=owner; alias.r=Some(&const alias.x); }", "same owner");
  err("reassigned alias keeps provenance", s + "void init(&S owner, &const S other) { var alias=&const *owner; var r=&const alias.x; alias=other; owner.r=Some(r); }", "same owner");
  err("rebound owner keeps stored loan", s + "void init(&S owner, &S other) { owner.r=Some(&const owner.x); owner=other; }", "same owner");
  err("rebound output keeps escaped local", "void save(&Vec<&const int> output, &Vec<&const int> other) { int n=1; output.push(&const n); output=other; }", "does not live past");
  err("store returned own field", s + "&const int view(&const S owner) { return &const owner.x; } void init(&S owner) { owner.r=Some(view(&const *owner)); }", "same owner");
  err("store own field through helper", s + "void save(&S owner, &const int r) { owner.r=Some(r); } void init(&S owner) { var r=&const owner.x; save(&*owner,r); }", "same owner");
  err("store own field in closure", "struct S { int x; (fn const() -> int)? read; void init(&self) { var r=&const self.x; self.read=Some(const || *r); } }", "same owner");
  ok("store external field referent", "struct S { &const int external; (&const int)? r; void init(&self) { self.r=Some(&const *self.external); } }");
  ok("alias receiver does not store itself", "struct S { Vec<&const int> refs; void add(&self, &const int value) { var refs=&self.refs; refs.push(value); refs.clear(); } }");
  err("destructor stores own field externally", "struct S { (&const int)? r; } struct Hold { int x; &S target; } impl Drop for Hold { void drop(&self) { self.target.r=Some(&const self.x); } }", "same owner");
  err("reusable closure stores borrowed owner field", "struct S { int x; (&const int)? r; void init(&self) { var f=|| { self.r=Some(&const self.x); }; f(); } }", "same owner");
  err("consuming closure stores borrowed owner field", "struct S { int x; (&const int)? r; void init(&self) { var f=once || { self.r=Some(&const self.x); }; f(); } }", "same owner");
  err("disjoint arguments cannot create movable self-reference", "struct S { int x; Vec<&const int> refs; } void save(&Vec<&const int> refs, &const int value) { refs.push(value); } main() { var s=S { x:1, refs:Vec.new() }; save(&s.refs,&const s.x); var moved=s; }", "borrowed");
  err("copied self-reference cannot outlive original", s + "main() { S outer; { var inner=S { x:1,r:None }; inner.r=Some(&const inner.x); outer=inner; } match outer.r { Some(r) => print(\"{*r}\"), None => {} } }", "scope");
  ok("copied self-reference stays within original lifetime", s + "main() { var inner=S { x:1,r:None }; inner.r=Some(&const inner.x); var copy=inner; match copy.r { Some(r) => print(\"{*r}\"), None => {} } }");
  ok("return own field", s + "&const int view(&const S owner) { return &const owner.x; }");
  ok("store other parameter", s + "void save(&S owner, &const int other) { owner.r=Some(other); }");
  ok("store borrowed other field", s + "void save(&S owner, &const S other) { owner.r=Some(&const other.x); }");
}

void test_async() {
  ok("async borrows local across await", "import std.task.yield_now; async int f() { int n=1; var r=&const n; await yield_now(); return *r; }");
  ok("async returns input borrow", "import std.task.yield_now; async &const int f(&const int r) { await yield_now(); return r; }");
  ok("await generic future", "async T f<T,F:Future<T>>(F future) { return await future; } main() {}");
  err("task blocks caller mutation", "async int f(&const int r) { return *r; } main() { int n=1; var task=f(&const n); n=2; }", "borrowed");
  err("task holds moved reference", "async int f(&int r) { return *r; } main() { int n=1; var task=f(&n); int value=n; }", "writable");
  err("task cannot escape block local", "async int f(&const int r) { return *r; } Task<int> g() { int n=1; return f(&const n); }", "does not live past");
  err("async cannot return frame reference", "async &const int f() { int n=1; return &const n; }", "does not live past");
  err("async cannot return closure borrowing frame", "async fn const() -> int f() { int n=1; return [&const n] const || n; }", "does not live past");
  err("async cannot use consumed task", "async int f() { return 1; } async void g() { var task=f(); await task; await task; }", "moved");
  err("async output signature mismatch", "struct S {} impl Future<int> for S { int poll(&self,Waker wake) { return 1; } } async void f() { await S {}; }", "does not match");
  err("cancellation cannot export local", "import std.task.yield_now; struct H { &Vec<&const int> out; &const int value; } impl Drop for H { void drop(&self) { self.out.push(self.value); } } async void f(&Vec<&const int> out) { int n=1; var h=H { out:out,value:&const n }; await yield_now(); }", "does not live past");
  ok("drop unpolled task releases loan", "import std.mem; async int f(&const int r) { return *r; } main() { int n=1; var task=f(&const n); mem.drop(task); n=2; }");
  ok("await Task borrowed output can leave child", "import std.task.yield_now; async &const int f(&const int r) { await yield_now(); return r; } async &const int g(&const int r) { return await f(r); }");
  ok("executor scope ends task loans", "import std.task.LocalExecutor; async void f(&const int r) { print(\"{*r}\"); } main() { int n=1; { var executor=LocalExecutor.new(); executor.spawn(f(&const n)); executor.run(); } n=2; }");
  err("executor cannot hold block local", "import std.task.LocalExecutor; async void f(&const int r) { print(\"{*r}\"); } main() { var executor=LocalExecutor.new(); { int n=1; executor.spawn(f(&const n)); } executor.run(); }", "scope");
  const std::string manual = "struct F { int n; } impl Future<&const int> for F { Poll<&const int> poll(&self,Waker wake) { return Poll.Ready(&const self.n); } } ";
  err("owned manual future cannot escape frame", manual + "async &const int f() { return await F { n:1 }; }", "does not live past");
  ok("borrowed manual future output is external", manual + "async &const int f(&F future) { return await future; }");
  err("cancellation rejects conservative cycle", "import std.task.yield_now; async void f(bool side) { fn const()->int first=const || 1; fn const()->int second=const || 2; if (side) { second=[&const first] const || first(); } else { first=[&const second] const || second(); } loop { await yield_now(); } }", "destructors borrow each other");
}

void test_effect_origins() {
  ok("cleared optional result releases argument loan",
     "Option<&const int> clear(&const int key) { Option<&const int> result=Some(key); result=None; return result; } "
     "main() { Option<&const int> saved=None; { int local=7; saved=clear(local); } match saved { Some(r)=>print(\"{*r}\"), None=>{} } }");
  err("local reference alias preserves returned argument",
      "&const int relay(&const int value) { Option<&const int> owner=None; var alias=&owner; *alias=Some(value); return owner.unwrap(); } "
      "main() { &const int saved; { int inner=5; saved=relay(inner); } print(\"{*saved}\"); }", "scope");
  err("effect join retains both argument origins",
      "&const int choose(bool side,&const int left,&const int right) { var value=left; if(side) { value=right; } return value; } "
      "main() { int outer=1; &const int saved; { int inner=2; saved=choose(false,outer,inner); } print(\"{*saved}\"); }", "scope");
  err("recursive effects reach the full argument set",
      "&const int recurse(int n,&const int left,&const int right) { if(n==0) { return left; } return recurse(n-1,right,left); } "
      "main() { int outer=1; &const int saved; { int inner=2; saved=recurse(0,outer,inner); } print(\"{*saved}\"); }", "scope");
}

void test_drop_flags() {
  auto c = run("struct S { String s; }\nf(S s) { }\nmain() { var x = S { s: String.from(\"a\") }; if (true) { f(x); } }");
  ++synx_test::g_checks;
  if (c->sink.has_errors()) {
    ++synx_test::g_failures;
    std::printf("FAIL drop flags: %s", c->sink.render(c->user_file).c_str());
    return;
  }
  std::string d = synx::ir::dump(*c->ir_module.entry);
  ++synx_test::g_checks;
  if (d.find("drop _1 if _") == std::string::npos) {
    ++synx_test::g_failures;
    std::printf("FAIL drop flags: no conditional drop in\n%s", d.c_str());
  }
  // A plainly moved value gets no drop at all.
  c = run("struct S { String s; }\nf(S s) { }\nmain() { var x = S { s: String.from(\"a\") }; f(x); }");
  d = synx::ir::dump(*c->ir_module.entry);
  ++synx_test::g_checks;
  if (d.find("drop _1") != std::string::npos) {
    ++synx_test::g_failures;
    std::printf("FAIL drop elaboration: moved value still dropped in\n%s", d.c_str());
  }
}

void test_indexed_moves_and_names() {
  const std::string d = "struct D { int id; } impl Drop for D { void drop(&self) {} } ";
  err("vec element cannot move out", d + "main() { Vec<D> v = Vec.new(); v.push(D { id: 1 }); var n = v[0]; }",
      "cannot move an element out of `v`");
  err("array element cannot move out", d + "main() { D[2] a = [D { id: 1 }, D { id: 2 }]; var n = a[0]; }",
      "cannot move an element out of `a`");
  ok("copy element can be read by index", "main() { Vec<int> v = Vec.new(); v.push(1); int n = v[0]; int[2] a = [1, 2]; int m = a[1]; }");
  ok("element can move out through swap and pop", "import std.mem; " + d + "main() { Vec<D> v = Vec.new(); v.push(D { id: 1 }); var n = v.pop(); D[1] a = [D { id: 2 }]; var m = mem.replace(&a[0], D { id: 3 }); }");
  err("moved field is named", "struct S { String name; int bpm; } main() { var a = S { name: String.from(\"x\"), bpm: 5 }; var b = a; print(\"{a.bpm}\"); }",
      "use of moved value `a.bpm`");
  err("nested and tuple fields are named", "struct I { String s; } struct O { I inner; } main() { var o = O { inner: I { s: String.from(\"a\") } }; var x = o.inner.s; print(o.inner.s); }",
      "use of moved value `o.inner.s`");
}

}  // namespace

int main() {
  test_moves();
  test_loans();
  test_closures();
  test_closure_lifetimes();
  test_dyn();
  test_scope_ends();
  test_boxes();
  test_self_references();
  test_async();
  test_effect_origins();
  test_drop_flags();
  test_indexed_moves_and_names();
  return synx_test::finish("borrow_test");
}
