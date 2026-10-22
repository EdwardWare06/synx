#include <string>

#include "synx/compile.hpp"
#include "synx/ir.hpp"
#include "test_support.hpp"

namespace {

// Lowers a program and returns the IR dump. Reports front end or lowering
// errors as test failures.
std::string lower(const char* label, const std::string& src, bool expect_ok = true) {
  auto c = synx::check_source("t.snx", src);
  ++synx_test::g_checks;
  if (c->sink.has_errors()) {
    ++synx_test::g_failures;
    std::printf("FAIL %s: front end errors:\n%s", label, c->sink.render(c->user_file).c_str());
    return "";
  }
  bool ok = c->run_lowering();
  if (ok != expect_ok) {
    ++synx_test::g_failures;
    std::printf("FAIL %s: lowering %s\n%s", label, ok ? "unexpectedly succeeded" : "failed",
                c->sink.render(c->user_file).c_str());
  }
  return synx::ir::dump(c->ir_module);
}

void has(const char* label, const std::string& dump, const std::string& needle) {
  ++synx_test::g_checks;
  if (dump.find(needle) == std::string::npos) {
    ++synx_test::g_failures;
    std::printf("FAIL %s: IR does not contain '%s'\n%s\n", label, needle.c_str(), dump.c_str());
  }
}

void test_basics() {
  std::string d = lower("hello", "main() { print(\"hi\"); }");
  has("hello call", d, "call @print(const \"hi\")");
  has("hello ret", d, "return");

  d = lower("arith", "int add(int a, int b) { return a + b; }\nmain() { int x = add(1, 2); }");
  has("add body", d, "+(copy _1, copy _2)");
  has("call add", d, "call add(const 1_int, const 2_int)");
  has("main local", d, "// x");

  d = lower("if", "int f(bool b) { if (b) { return 1; } else { return 2; } }\nmain() { f(true); }");
  has("switch", d, "switch copy _1 [1: bb");

  d = lower("while", "main() { int i = 0; while (i < 3) { i += 1; } }");
  has("loop cmp", d, "<(copy _");
  has("loop inc", d, "+(copy _");

  d = lower("for range", "main() { int s = 0; for (i in 0..10) { s += i; } }");
  has("range agg", d, "Range<int> {const 0_int, const 10_int}");

  d = lower("if value", "main() { int x = 5; var sign = if (x < 0) { -1 } else { 1 }; print(\"{sign}\"); }");
  has("neg", d, "-(const 1_int)");
  has("fmt", d, "call @fmt_int(");
}

void test_structs_enums() {
  const char* src =
      "struct P { double x; double y; double len(&const self) { return self.x * self.x; } void bump(&self) { self.x += 1.0; } }\n"
      "enum T { A(int), B }\n"
      "int g(T t) { match t { T.A(n) => return n, T.B => return 0 } }\n"
      "main() { var p = P { x: 1.0, y: 2.0 }; var l = p.len(); p.bump(); int v = g(T.A(3)); int w = g(T.B); }";
  std::string d = lower("structs", src);
  has("struct agg", d, "P {const 1_double, const 2_double}");
  has("const method", d, "call P.len(copy _");
  has("mut method", d, "call P.bump(copy _");
  has("variant agg", d, "T::0(const 3_int)");
  has("unit variant", d, "T::1()");
  has("discriminant", d, "discriminant(_1)");
  has("downcast", d, "(_1 as variant 0).0");
  has("field through ref", d, "(*_1).0");
}

void test_generics_and_traits() {
  const char* src =
      "trait Shape { double area(&const self); }\n"
      "struct Sq { double s; }\n"
      "impl Shape for Sq { double area(&const self) { return self.s * self.s; } }\n"
      "double twice<T: Shape>(&const T a) { return a.area() * 2.0; }\n"
      "T id<T>(T x) { return x; }\n"
      "main() { var q = Sq { s: 2.0 }; double t = twice(&const q); int i = id(1); bool b = id(true); }";
  std::string d = lower("generics", src);
  has("mono fn", d, "fn twice<Sq>(");
  has("mono id int", d, "fn id<int>(");
  has("mono id bool", d, "fn id<bool>(");
  has("impl method", d, "fn Sq.Shape.area(");
  has("trait call resolved", d, "call Sq.Shape.area(");
}

void test_option_result() {
  const char* src =
      "enum E { Bad }\n"
      "Result<int, E> p(int n) { if (n < 0) { return Err(E.Bad); } return Ok(n); }\n"
      "Result<int, E> q() { int a = p(1)?; return Ok(a + 1); }\n"
      "main() { int? x = Some(5); int y = x ?? 0; var r = q(); }";
  std::string d = lower("option result", src);
  has("try switch", d, "switch copy _");
  has("err return", d, "Result<int, E>::1(");
  has("coalesce some", d, "Option<int>");
}

void test_strings() {
  const char* src =
      "main() { String s = String.from(\"ab\"); print(s); print(\"{s} has {s.len()} bytes\"); bool e = \"a\" == \"b\"; }";
  std::string d = lower("strings", src);
  has("string from", d, "call String.from(const \"ab\")");
  has("str coercion", d, "str_from_string(");
  has("push str", d, "call @string_push_str(");
  has("fmt uint", d, "call @fmt_uint(");
  has("str cmp", d, "call @str_cmp(");
  has("drop string", d, "drop _");
}

void test_examples() {
  const char* paths[] = {"examples/hello.snx", "examples/shapes.snx", "examples/ownership.snx"};
  for (const char* p : paths) {
    std::string full = std::string(SYNX_SOURCE_DIR) + "/" + p;
    auto file = synx::SourceFile::load(full);
    ++synx_test::g_checks;
    if (!file) { ++synx_test::g_failures; std::printf("FAIL cannot read %s\n", full.c_str()); continue; }
    synx::Compilation c(synx::SourceFile("<prelude>", synx::sema::prelude_source()), *file);
    bool ok = c.run_front_end() && c.run_lowering();
    if (!ok) {
      ++synx_test::g_failures;
      std::printf("FAIL lowering %s:\n%s", p, c.sink.render(c.user_file).c_str());
    }
  }
}

void test_nested_cleanup() {
  auto c = synx::check_source("t.snx", "struct Guard {} impl Drop for Guard { void drop(&self) {} } "
      "struct Pair { String first; Guard second; } void unused(Pair p) {} main() {}");
  CHECK(!c->sink.has_errors());
  CHECK(c->run_lowering());
  for (auto& def : c->program.structs) {
    if (def->name != "Guard") continue;
    CHECK_EQ(c->ir_module.drop_impls.count(def->self_type), size_t(1));
    CHECK_EQ(c->ir_module.drop_types.count(def->self_type), size_t(1));
  }
}

void test_closures() {
  lower("closure", "main() { var f = |int a| a + 1; }", true);
}

}  // namespace

int main() {
  test_basics();
  test_structs_enums();
  test_generics_and_traits();
  test_option_result();
  test_strings();
  test_examples();
  test_nested_cleanup();
  test_closures();
  return synx_test::finish("ir_test");
}
