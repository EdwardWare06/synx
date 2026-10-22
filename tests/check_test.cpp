#include <string>

#include "synx/compile.hpp"
#include "test_support.hpp"

namespace {

// Checks a program and expects no errors.
void ok(const char* label, const std::string& src) {
  auto c = synx::check_source("t.snx", src);
  ++synx_test::g_checks;
  if (c->prelude_sink.has_errors()) {
    ++synx_test::g_failures;
    std::printf("FAIL %s: prelude errors\n%s", label, c->prelude_sink.render(c->prelude_file).c_str());
    return;
  }
  if (c->sink.has_errors()) {
    ++synx_test::g_failures;
    std::printf("FAIL %s: expected no errors, got:\n%s", label, c->sink.render(c->user_file).c_str());
  }
}

// Checks a program and expects an error whose message contains `needle`.
void err(const char* label, const std::string& src, const std::string& needle) {
  auto c = synx::check_source("t.snx", src);
  ++synx_test::g_checks;
  if (c->prelude_sink.has_errors()) {
    ++synx_test::g_failures;
    std::printf("FAIL %s: prelude errors\n%s", label, c->prelude_sink.render(c->prelude_file).c_str());
    return;
  }
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

// Returns the resolved return type of a module level function.
std::string ret_type(const std::string& src, const std::string& fn) {
  auto c = synx::check_source("t.snx", src);
  const synx::sema::Symbol* s = c->program.find(fn);
  if (!s || !s->fn_def || !s->fn_def->ret) return "<none>";
  return s->fn_def->ret->to_string();
}

void test_prelude_checks() {
  ok("empty program", "");
  ok("hello", "main() { print(\"hi\"); }");
}

void test_functions() {
  ok("declared ret", "int add(int a, int b) { return a + b; }");
  ok("inferred ret", "add(int a, int b) { return a + b; }");
  CHECK_EQ(ret_type("add(int a, int b) { return a + b; }", "add"), std::string("int"));
  CHECK_EQ(ret_type("f() { }", "f"), std::string("void"));
  CHECK_EQ(ret_type("f() { 1.5 }", "f"), std::string("double"));
  CHECK_EQ(ret_type("f() { return \"x\"; }", "f"), std::string("&const str"));
  CHECK_EQ(ret_type("g() { return 1; }\nf() { return g(); }", "f"), std::string("int"));
  err("recursive needs type", "f(int n) { if (n == 0) { return 1; } return f(n - 1); }", "calls itself");
  err("missing return value", "int f() { }", "missing return value");
  err("wrong return type", "int f() { return \"x\"; }", "type mismatch in return value");
  err("pub needs ret", "pub f() { }", "pub function must write its return type");
  err("arg count", "f(int a) { }\nmain() { f(1, 2); }", "expected 1 argument, got 2");
  err("arg type", "f(int a) { }\nmain() { f(\"x\"); }", "type mismatch in argument");
  ok("return in branches", "int f(bool b) { if (b) { return 1; } else { return 2; } }");
  err("falls off", "f(bool b) { if (b) { return 1; } }", "falls off the end");
  err("unknown name", "main() { x = 1; }", "unknown name 'x'");
  err("unknown fn", "main() { nope(); }", "unknown name 'nope'");
  ok("extern unsafe", "extern \"C\" { int puts(*const byte s); }\nmain() { unsafe { puts(0 as *const byte); } }");
}

void test_variables() {
  ok("typed", "main() { int x = 5; x = x + 1; }");
  ok("inferred", "main() { var x = 5; x += 1; double d = 1.5; }");
  err("const assign", "main() { const x = 5; x = 6; }", "read only");
  err("const typed assign", "main() { const int x = 5; x += 1; }", "read only");
  err("init type", "main() { int x = \"s\"; }", "type mismatch in variable initializer");
  err("void var", "f() { }\nmain() { var x = f(); }", "void value");
  err("unsized local", "main() { str s = \"x\"; }", "unsized type");
  ok("shadow in block", "main() { int x = 1; { int x = 2; } x = 3; }");
  err("use before decl", "main() { x = 1; int x = 2; }", "unknown name 'x'");
  ok("literal defaults", "main() { var a = 1; var b = 2.0; a = a + 1; b = b * 2.0; }");
  err("int plus float", "main() { var a = 1 + 2.0; }", "same type");
  ok("long literal", "main() { long x = 42L; var y = 42L; y = x; }");
  ok("literal takes type", "main() { byte b = 255; usize n = 3; }");
}

void test_structs() {
  const char* point =
      "struct Point { double x; double y;\n"
      "  double len(&const self) { return self.x * self.x + self.y * self.y; }\n"
      "  void scale(&self, double k) { self.x *= k; self.y *= k; }\n"
      "  Point origin() { return Point { x: 0.0, y: 0.0 }; }\n"
      "}\n";
  ok("struct basics", std::string(point) +
      "main() { var p = Point { x: 1.0, y: 2.0 }; var l = p.len(); p.scale(2.0); var o = Point.origin(); p.x = 3.0; }");
  err("missing field", std::string(point) + "main() { var p = Point { x: 1.0 }; }", "missing field 'y'");
  err("extra field", std::string(point) + "main() { var p = Point { x: 1.0, y: 2.0, z: 3.0 }; }", "no field 'z'");
  err("field type", std::string(point) + "main() { var p = Point { x: 1, y: 2.0 }; }", "type mismatch");
  err("unknown field", std::string(point) + "main() { var p = Point.origin(); p.z = 1.0; }", "no field 'z'");
  err("method as field", std::string(point) + "main() { var p = Point.origin(); var l = p.len; }", "is a method");
  err("const receiver", std::string(point) + "main() { const p = Point.origin(); p.scale(2.0); }", "read only");
  err("const ref receiver", std::string(point) + "f(&const Point p) { p.scale(2.0); }", "read only");
  ok("mut ref receiver", std::string(point) + "f(&Point p) { p.scale(2.0); p.x = 1.0; }");
  err("assign through const ref", std::string(point) + "f(&const Point p) { p.x = 1.0; }", "read only");
  err("assoc as method", std::string(point) + "main() { var p = Point.origin(); var q = p.origin(); }", "associated function");
  err("method as assoc", std::string(point) + "main() { var l = Point.len(); }", "is a method");
  ok("self struct lit", "struct P { int a; P make() { return Self { a: 1 }; } P copy(&const self) { return P { a: self.a }; } }");
  ok("shorthand", "struct P { int a; }\nmain() { int a = 1; var p = P { a }; }");
  ok("tuple fields", "main() { var t = (1, 2.5); int a = t.0; double b = t.1; }");
  err("tuple index", "main() { var t = (1, 2); var c = t.2; }", "no element 2");
}

void test_generics_and_traits() {
  ok("grouped comparisons", "void f(int a,int b,int c,int d) { bool first=a < (b+1) && c > (d); "
     "if (a < (b) && c > (d)) {} for (i in 0..(b)) {} bool last=a < (b); }");
  err("equality must not guess a type", "struct S {} impl Eq for S { bool eq(&const self,&const Self other) { return true; } } "
      "T unknown<T>() { panic(\"unknown\"); } main() { var a=unknown(); var b=unknown(); bool same=(&const a)==(&const b); }", "cannot infer");
  err("impl arguments require sized", "trait Marker {} impl<T> Marker for &const T {} "
      "void take<X:Marker>(X value) {} main() { take(\"text\"); }", "does not implement 'Marker'");
  ok("explicit unsized reference impl", "trait Marker {} impl<T> Marker for &const T {} "
     "impl Marker for &const str {} void take<X:Marker>(X value) {} main() { take(\"text\"); }");
  ok("generic grouped arguments", "T id<T>(T value) { return value; } main() { int n=1; var r=&const n; "
     "var rr=&const r; var same=id<&const (&const int)>(rr); var tuple=id<(int,bool)>((1,true)); "
     "var callback=id<fn(int)->int>(|int x| x+1); }");
  err("bounded struct unused argument", "struct Bad {} struct S<T: Eq> { void f() {} } main() { S<Bad>.f(); }", "does not implement 'Eq'");
  err("bounded struct annotation", "struct S<T: Eq> {} main() { S<fn()> value; }", "does not implement 'Eq'");
  err("bounded struct field", "struct S<T: Eq> {} struct Outer { S<fn()> field; }", "does not implement 'Eq'");
  err("bounded struct signature", "struct S<T: Eq> {} void f(S<fn()> value) {}", "does not implement 'Eq'");
  err("bounded struct nested", "struct S<T: Eq> {} void f(Vec<S<fn()>> value) {}", "does not implement 'Eq'");
  err("unbounded wrapper", "struct S<T: Eq> {} struct Outer<T> { S<T> field; }", "does not implement 'Eq'");
  ok("bounded wrapper", "struct S<T: Eq> {} struct Outer<T: Eq> { S<T> field; } main() { Outer<int> value; }");
  err("bounded enum", "struct Bad {} enum E<T: Eq> { Empty } main() { var value=E<Bad>.Empty; }", "does not implement 'Eq'");
  ok("nominal dependent bound", "trait Pair<T> {} struct Good {} impl Pair<int> for Good {} "
     "struct S<A: Pair<B>, B> {} main() { S<Good,int> value; }");
  const char* shapes =
      "trait Shape { double area(&const self); &const str name(&const self) { return \"shape\"; } }\n"
      "struct Circle { double r; }\n"
      "struct Sq { double s; }\n"
      "impl Shape for Circle { double area(&const self) { return 3.0 * self.r * self.r; } }\n"
      "impl Shape for Sq { double area(&const self) { return self.s * self.s; } &const str name(&const self) { return \"sq\"; } }\n";
  ok("trait impl calls", std::string(shapes) +
      "main() { var c = Circle { r: 1.0 }; var a = c.area(); var n = c.name(); var s = Sq { s: 2.0 }; var m = s.name(); }");
  ok("generic bound", std::string(shapes) +
      "double total<T: Shape>(&const T a, &const T b) { return a.area() + b.area(); }\n"
      "main() { var c = Circle { r: 1.0 }; var d = Circle { r: 2.0 }; var t = total(&const c, &const d); }");
  err("bound not met", std::string(shapes) +
      "double area<T: Shape>(&const T a) { return a.area(); }\n"
      "main() { var x = 5; var t = area(&const x); }", "does not implement 'Shape'");
  err("missing impl method", "trait T { void f(&const self); }\nstruct S { }\nimpl T for S { }", "missing method 'f'");
  err("extra impl method", "trait T { }\nstruct S { }\nimpl T for S { void g(&const self) { } }", "not a member of trait");
  err("method on unbounded", "void f<T>(&const T x) { x.area(); }", "no method 'area'");
  ok("generic fn explicit", "T first<T: Copy>(&const T[] items) { return items[0]; }\n"
                           "main() { int[3] a = [1, 2, 3]; var x = first<int>(&const a); var y = first(&const a); }");
  ok("generic struct", "struct Pair<A, B> { A a; B b; Pair<A, B> make(A a, B b) { return Pair { a: a, b: b }; } }\n"
                      "main() { var p = Pair.make(1, \"x\"); int x = p.a; &const str s = p.b; Pair<int, bool> q = Pair { a: 1, b: true }; }");
  ok("operator trait", "struct V { int x; }\nimpl Add for V { V add(self, V o) { return V { x: self.x + o.x }; } }\n"
                      "main() { var a = V { x: 1 }; var b = V { x: 2 }; var c = a + b; }");
  err("no operator", "struct V { int x; }\nmain() { var a = V { x: 1 }; var c = a + a; }", "implement the trait 'Add'");
  ok("eq trait", "struct V { int x; }\nimpl Eq for V { bool eq(&const self, &const V o) { return self.x == o.x; } }\n"
                "main() { var a = V { x: 1 }; bool b = a == a; }");
  ok("display in interp", "struct V { int x; }\nimpl Display for V { String to_string(&const self) { return String.from(\"v\"); } }\n"
                         "main() { var v = V { x: 1 }; print(\"{v} and {v.x}\"); }");
  err("no display", "struct V { int x; }\nmain() { var v = V { x: 1 }; print(\"{v}\"); }", "does not implement 'Display'");
  ok("copy impl", "struct P { int a; }\nimpl Copy for P { }\nf(P p) { }\nmain() { var p = P { a: 1 }; f(p); f(p); }");
  err("copy impl non-copy field", "struct S { String s; }\nimpl Copy for S { }\nmain() { }", "cannot implement Copy");
  err("copy impl with drop", "struct D { int a; }\nimpl Drop for D { void drop(&self) { } }\nimpl Copy for D { }\nmain() { }", "cannot implement Copy");
  err("copy impl unbounded param", "struct W<T> { T v; }\nimpl<T> Copy for W<T> { }\nmain() { }", "cannot implement Copy");
  ok("copy impl bounded param", "struct W<T> { T v; }\nimpl<T: Copy> Copy for W<T> { }\nmain() { var w = W { v: 1 }; var a = w; var b = w; }");
  ok("copy impl enum", "enum E { A(int), B }\nimpl Copy for E { }\nmain() { }");
}

void test_not_trait() {
  const std::string flag =
      "struct Flag { bool value; }\n"
      "impl Not for Flag { Flag not(self) { return Flag { value: !self.value }; } }\n";
  ok("not struct", flag + "main() { Flag value = !Flag { value: true }; }");
  ok("not inferred result", flag + "main() { var value = !Flag { value: true }; bool b = value.value; }");
  ok("not enum", "enum Switch { On, Off }\n"
      "impl Not for Switch { Switch not(self) { return match self { On => Switch.Off, Off => Switch.On }; } }\n"
      "main() { Switch value = !Switch.On; }");
  ok("not generic bound", flag +
      "T invert<T: Not>(T value) { return !value; } main() { Flag value = invert(Flag { value: true }); }");
  err("not needs implementation", "struct Plain {} main() { var value = !Plain {}; }", "operator '!'");
  err("not result is self", flag + "main() { bool value = !Flag { value: true }; }", "type mismatch");
  ok("not primitive operands", "main() { bool b = !true; int bits = 1; int other = !bits; }");
}

void test_enums_and_match() {
  const char* token = "enum Token { Number(int), Plus, End }\n";
  // Matching through a reference binds references: n is &const int.
  ok("enum match", std::string(token) +
      "int f(&const Token t) { match t { Token.Number(n) => return *n, Token.Plus => return 1, Token.End => return 2 } }");
  err("enum match ref binding", std::string(token) +
      "int f(&const Token t) { match t { Token.Number(n) => return n, _ => return 1 } }", "found '&const int'");
  ok("nested coverage", "enum E { A, B }\nenum W { Ok(E), No }\n"
      "int f(W w) { match w { W.Ok(E.A) => return 1, W.Ok(E.B) => return 2, W.No => return 0 } }");
  err("nested missing", "enum E { A, B }\nenum W { Ok(E), No }\n"
      "int f(W w) { match w { W.Ok(E.A) => return 1, W.No => return 0 } }", "missing: Ok(B)");
  ok("tuple match", "int f(bool a, bool b) { match (a, b) { (true, true) => return 1, (true, false) => return 2, (false, _) => return 3 } }");
  ok("bare variant names", std::string(token) +
      "int f(Token t) { match t { Number(n) => return n, Plus => return 1, End => return 2 } }");
  err("not exhaustive", std::string(token) + "int f(Token t) { match t { Token.Plus => return 1, Token.End => return 2 } }",
      "missing: Number");
  ok("wildcard", std::string(token) + "int f(Token t) { match t { Token.Number(n) => return n, _ => return 0 } }");
  ok("match value", std::string(token) + "int f(Token t) { return match t { Token.Number(n) => n, _ => 0 }; }");
  err("arm types", std::string(token) + "main() { var t = Token.Plus; var x = match t { Token.Plus => 1, _ => \"a\" }; }", "different types");
  err("wrong payload count", std::string(token) + "main() { var t = Token.Plus; match t { Token.Number(a, b) => { } _ => { } } }", "has 1 fields");
  ok("option", "main() { int? a = Some(5); int? b = None; var c = a ?? 0; bool s = a.is_some(); }");
  ok("option match ref", "f(&const int? a) { match a { Some(v) => { int x = *v; } None => { } } }");
  ok("result and try", "enum E { Bad }\nResult<int, E> p(int n) { if (n < 0) { return Err(E.Bad); } return Ok(n); }\n"
                      "Result<int, E> q() { int a = p(1)?; int b = p(2)?; return Ok(a + b); }");
  err("try wrong error", "enum E { Bad }\nenum F { Worse }\nResult<int, E> p() { return Ok(1); }\n"
                        "Result<int, F> q() { int a = p()?; return Ok(a); }", "error type mismatch");
  err("try outside result", "enum E { Bad }\nResult<int, E> p() { return Ok(1); }\nint q() { return p()?; }", "needs the function to return a Result");
  ok("try inferred ret", "enum E { Bad }\nResult<int, E> p() { return Ok(1); }\nq() { int a = p()?; return Ok(a); }");
  ok("struct variant", "enum S { Rect { double w; double h; }, Dot }\n"
                      "double f(S s) { match s { S.Rect { w, h } => return w * h, S.Dot => return 0.0 } }\n"
                      "main() { var r = S.Rect { w: 1.0, h: 2.0 }; }");
  ok("bool match", "int f(bool b) { match b { true => return 1, false => return 0 } }");
  err("int match needs wildcard", "int f(int n) { match n { 0 => return 1, 1 => return 2 } }", "add a '_' arm");
  ok("int match", "int f(int n) { match n { 0 => return 1, 1 | 2 => return 2, 3..=9 => return 3, _ => return 0 } }");
  ok("guard", "int f(int n) { match n { x if x > 10 => return 1, _ => return 0 } }");
  ok("optional chain", "struct P { int a; }\nmain() { P? p = Some(P { a: 1 }); int? a = p?.a; }");
  ok("optional method", "struct P { int a; int get(&const self) { return self.a; } }\nmain() { P? p = None; int? a = p?.get(); }");
}

void test_control_flow() {
  ok("loops", "main() { int i = 0; while (i < 10) { i += 1; } loop { if (i > 20) { break; } i += 1; } for (j in 0..10) { i += j; } }");
  err("break outside", "main() { break; }", "break outside a loop");
  err("cond not bool", "main() { if (1) { } }", "type mismatch in if condition");
  ok("if value", "main() { int x = 5; var sign = if (x < 0) { -1 } else { 1 }; }");
  err("if no else value", "main() { var s = if (true) { 1 }; }", "cannot produce a value");
  ok("for array", "main() { int[3] a = [1, 2, 3]; for (x in a) { int y = x; } for (r in &const a) { int z = *r; } }");
  ok("for vec", "main() { Vec<int> v = Vec.new(); v.push(1); for (x in &const v) { int y = *x; } }");
  err("for non iterable", "main() { for (x in 5) { } }", "cannot iterate");
  ok("inclusive range", "main() { for (i in 0..=10) { int x = i; } }");
  ok("closure", "main() { var add = |int a, int b| a + b; int c = add(1, 2); var inc = |x| x + 1; int d = inc(1); }");
  ok("closure captures", "main() { int base = 10; var f = |int a| a + base; int c = f(1); }");
  ok("nested blocks", "int f() { var x = { int a = 1; a + 1 }; return x; }");
  err("unused value stmt", "main() { if (true) { 1 } else { 2 } print(\"x\"); }", "not used");
  // The last expression without ';' is the value of the function body.
  CHECK_EQ(ret_type("f() { if (true) { 1 } else { 2 } }", "f"), std::string("int"));
  ok("optional ref type", "&const str? f(&const str s) { return Some(s); }\nmain() { var x = f(\"a\") ?? \"b\"; }");
  // A loop that no break targets diverges, so nothing falls off the end.
  ok("fn ends in loop", "int f() { loop { return 1; } }\nmain() { f(); }");
  ok("fn ends in endless loop", "int f() { int i = 0; loop { i += 1; } }\nmain() { }");
  ok("inner break targets inner loop", "int f() { loop { while (true) { break; } return 2; } }\nmain() { f(); }");
  err("break ends the loop", "int f() { loop { break; } }\nmain() { }", "missing return value");
  // A body that only diverges infers void.
  CHECK_EQ(ret_type("main() { panic(\"x\"); }", "main"), std::string("void"));
  CHECK_EQ(ret_type("f() { panic(\"x\") }", "f"), std::string("void"));
  CHECK_EQ(ret_type("f() { loop { } }", "f"), std::string("void"));
}

void test_references() {
  ok("auto borrow const", "f(&const int x) { }\nmain() { int a = 1; f(a); f(&const a); }");
  err("mut needs explicit", "f(&int x) { }\nmain() { int a = 1; f(a); }", "expected a writable reference");
  ok("mut explicit", "f(&int x) { *x = 2; }\nmain() { int a = 1; f(&a); }");
  err("mut of const", "f(&int x) { }\nmain() { const int a = 1; f(&a); }", "read only");
  err("mut of temp", "f(&int x) { }\nmain() { f(&5); }", "temporary");
  ok("string to str", "f(&const str s) { }\nmain() { String s = String.from(\"x\"); f(s); f(&const s); f(\"lit\"); }");
  ok("array to slice", "f(&const int[] s) { }\nmain() { int[2] a = [1, 2]; f(a); f(&const a); }");
  ok("vec to slice", "f(&const int[] s) { }\nmain() { Vec<int> v = Vec.new(); f(v); }");
  ok("deref assign", "main() { int a = 1; var r = &a; *r = 2; }");
  err("deref const assign", "main() { int a = 1; var r = &const a; *r = 2; }", "read only");
  ok("slice index", "f(&const int[] s) { int x = s[0]; usize n = 0; int y = s[n]; }");
  err("index type", "f(&const int[] s) { int x = s[\"a\"]; }", "type mismatch in index");
  ok("mut to const ref", "f(&const int x) { }\nmain() { int a = 1; var r = &a; f(r); }");
  ok("compare strs", "main() { bool b = \"a\" == \"b\"; String s = String.from(\"x\"); bool c = s == s; }");
  ok("casts", "main() { int a = 1; long b = a as long; double d = a as double; byte c = 65; char ch = c as char; }");
  err("bad cast", "main() { var s = \"a\" as int; }", "cannot cast");
}

void test_dyn() {
  const std::string shape = "trait Shape { int area(&const self); void grow(&self, int n); } "
      "struct Sq { int side; } impl Shape for Sq { int area(&const self) { return self.side * self.side; } "
      "void grow(&self, int n) { self.side += n; } } ";
  ok("dyn conversions", shape + "read(&const dyn Shape s) { int n = s.area(); } "
      "write(&dyn Shape s) { s.grow(2); } main() { var s = Sq { side: 2 }; read(s); read(&const s); write(&s); }");
  ok("dyn reborrow", shape + "main() { var s = Sq { side: 2 }; &dyn Shape r = &s; "
      "var q = &const r; int a = q.area(); r.grow(2); }");
  err("dyn missing impl", shape + "main() { int x = 1; &const dyn Shape d = &const x; }", "does not implement");
  err("dyn read only receiver", shape + "f(&const dyn Shape s) { s.grow(2); }", "read only");
  err("dyn exclusive needs reference", shape + "main() { var s = Sq { side: 2 }; &dyn Shape d = s; }", "expected a writable reference");
  err("dyn cannot strengthen reference", shape + "main() { var s = Sq { side: 2 }; &dyn Shape d = &const s; }", "expected a writable reference");
  err("dyn by value", shape + "f(dyn Shape s) {}", "must be a reference");
  err("dyn generic method", "trait T { int f<U>(&const self, U v); } f(&const dyn T t) {}", "cannot be used as dyn");
  err("dyn Self result", "trait T { Self f(&const self); } f(&const dyn T t) {}", "cannot be used as dyn");
  err("dyn Self argument", "trait T { void f(&const self, &const Self other); } f(&const dyn T t) {}", "cannot be used as dyn");
  err("dyn value receiver", "trait T { void f(self); } f(&const dyn T t) {}", "cannot be used as dyn");
  err("dyn static method", "trait T { int f(); } f(&const dyn T t) {}", "cannot be used as dyn");
  err("dyn unused field safety", "trait Bad { Self f(&const self); } struct S { &const dyn Bad field; }", "cannot be used as dyn");
  err("dyn enum safety", "trait Bad { Self f(&const self); } enum E { Ref(&const dyn Bad) }", "cannot be used as dyn");
  err("dyn vec requires sized", "trait T {} main() { Vec<dyn T> v = Vec.new(); }", "unsized");
  err("dyn generic requires sized", shape + "int size<T: Shape>(&const T t) { return size_of<T>() as int; } "
      "main() { var s = Sq { side: 2 }; &const dyn Shape t = s; int n = size(t); }", "unsized");
  err("dyn unsized return", "trait T {} dyn T f() { panic(\"no\"); }", "unsized");
  err("dyn recursive impl bound", "trait A {} impl<T: A> A for T {} main() { int n = 1; &const dyn A a = n; }", "does not implement");
  err("dyn bound generic args", "trait T<U> {} f<X: T<int>>(&const X x) { &const dyn T<bool> t = x; }", "does not implement");
  err("dyn generic args", "trait T<U> {} struct S {} impl T<int> for S {} main() { var s = S {}; &const dyn T<bool> t = &const s; }", "does not implement");
}

void test_recursive_types() {
  ok("recursive owners", "struct Node { Vec<Node> children; } enum List { Nil, Cons(int, Box<List>) } main() { var n = Box<int>.new(1); int x = n.into_inner(); }");
  ok("finite repeated generic", "struct Wrap<T> { T value; } main() { var w = Wrap { value: Wrap { value: 1 } }; }");
  ok("mutual pointer recursion", "struct A { Box<B>? next; } struct B { Box<A>? next; }");
  err("inline recursion", "struct A { A a; }", "inline layout");
  err("mutual inline recursion", "struct A { B b; } struct B { A a; }", "inline layout");
  err("enum inline recursion", "enum E { End, Next(E) }", "inline layout");
  err("generic inline recursion", "struct Wrap<T> { T value; } struct A { Wrap<A> a; }", "inline layout");
  err("expanding inline recursion", "struct A<T> { A<Vec<T>> a; }", "inline layout");
  ok("sized reference pointer cast", "main() { int x=1; var shared=(&const x) as *const int; var writable=(&x) as *int; }");
  err("shared reference pointer strengthening", "main() { int x=1; var p=(&const x) as *int; }", "cannot cast");
  err("unsized reference pointer cast", "void f(&const int[] x) { var p=x as *const int; }", "cannot cast");
  err("explicit Box destructor", "main() { var b=Box<int>.new(1); b.drop(); }", "cannot be called explicitly");
  err("explicit generic destructor", "void f<T: Drop>(&T value) { value.drop(); }", "cannot be called explicitly");
  err("optional explicit destructor", "main() { var b=Some(Box<int>.new(1)); b?.drop(); }", "cannot be called explicitly");
  err("explicit dynamic destructor", "void f(&dyn Drop value) { value.drop(); }", "cannot be called explicitly");
  ok("ordinary method named drop", "struct S { void drop(&self) {} } main() { var s=S {}; s.drop(); }");
  err("drop intrinsic unsafe", "main() { drop_in_place<int>(0 as *int); }", "requires an unsafe block");
  err("str_from_raw unsafe", "main() { var s = str_from_raw(0 as *const byte, 0); }", "str_from_raw requires an unsafe block");
  ok("str_from_raw in unsafe", "main() { var s = unsafe { str_from_raw(0 as *const byte, 0) }; }");
  err("box unsized", "trait T {} main() { var b = Box<dyn T>.new(1); }", "unsized");
}

void test_closure_modes() {
  ok("shared function type", "int f(&const fn const(int) -> int value) { return value(1); } main() { const value=const |int n| n; int n=f(value); }");
  ok("consuming function type", "int f(fn once(int) -> int value) { return value(1); } main() { int n=f(once |int n| n); }");
  ok("named function modes", "int id(int n) { return n; } main() { fn const(int) -> int a=id; fn once(int) -> int b=id; int x=a(1); int y=b(2); }");
  ok("shared weakens to writable", "main() { fn() -> int f=const || 1; int n=f(); }");
  err("writable cannot become shared", "main() { fn const() -> int f=|| 1; }", "type mismatch");
  err("writable cannot become consuming", "main() { fn once() -> int f=|| 1; }", "type mismatch");
  err("readonly writable function ref", "int call(&const fn() -> int f) { return f(); }", "writable access");
  err("capture unknown", "main() { var f=[&const missing] || 1; }", "unknown captured variable");
  err("capture duplicate", "main() { int n=1; var f=[n,n] || n; }", "duplicate capture");
  err("capture read only mutable", "main() { const int n=1; var f=[&n] || n; }", "read only");
}

void test_async() {
  ok("async signature", "async int answer() { return 42; } main() { Task<int> t=answer(); }");
  ok("async generic", "async T identity<T>(T value) { return value; } main() { Task<int> t=identity(2); }");
  ok("await task", "async int f() { return 1; } async int g() { return await f(); }");
  ok("await through reference", "async int f(&Task<int> t) { return await t; }");
  ok("await future bound", "async T f<T,F:Future<T>>(F future) { return await future; }");
  ok("async named value", "async int f(int n) { return n; } main() { fn(int)->Task<int> call=f; }");
  err("await plain value", "async void f() { await 1; }", "implements Future");
  err("await shared future", "async int f(&const Task<int> task) { return await task; }", "writable access");
  err("async main", "async main() {}", "main must be synchronous");
  err("async trait method", "trait T { async int f(&self); }", "async trait");
  err("closure does not inherit async", "async void f() { var g=|| { await 1; }; }", "requires an async");
}

void test_misc_errors() {
  err("const closure call", "main() { const f = || 1; int x = f(); }", "requires writable access");
  err("duplicate fn", "f() { }\nf() { }", "already defined");
  err("duplicate field", "struct S { int a; int a; }", "duplicate field");
  err("unknown type", "f(Foo x) { }", "unknown type 'Foo'");
  err("type args count", "struct S<T> { T a; }\nf(S x) { }", "expects 1 type argument");
  err("self outside", "f(&self) { }", "only allowed in a method");
  err("type as value", "struct S { int a; }\nmain() { var x = S; }", "is a type, not a value");
  err("unsafe deref", "main() { *int p = 0 as *int; int x = *p; }", "requires an unsafe block");
  ok("empty dyn", "trait T { }\nf(&const dyn T x) { }");
  err("await outside async", "f() { await g(); }\ng() { }", "requires an async");
}

}  // namespace

int main() {
  test_prelude_checks();
  test_functions();
  test_variables();
  test_structs();
  test_generics_and_traits();
  test_not_trait();
  test_enums_and_match();
  test_control_flow();
  test_references();
  test_dyn();
  test_recursive_types();
  test_closure_modes();
  test_async();
  test_misc_errors();
  return synx_test::finish("check_test");
}
