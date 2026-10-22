#include "synx/parser.hpp"

#include <string>

#include "synx/ast.hpp"
#include "synx/diagnostics.hpp"
#include "synx/lexer.hpp"
#include "synx/source.hpp"
#include "test_support.hpp"

namespace {

struct Parsed {
  synx::DiagnosticSink sink;
  std::string dump;
};

Parsed parse_expr(const std::string& src) {
  Parsed out;
  synx::SourceFile file("t.snx", src);
  synx::Lexer lexer(file, out.sink);
  synx::Parser parser(file, lexer.tokenize(), out.sink);
  auto e = parser.parse_expression();
  if (e) out.dump = synx::ast::dump(*e);
  return out;
}

Parsed parse_type(const std::string& src) {
  Parsed out;
  synx::SourceFile file("t.snx", src);
  synx::Lexer lexer(file, out.sink);
  synx::Parser parser(file, lexer.tokenize(), out.sink);
  auto t = parser.parse_type_only();
  if (t) out.dump = synx::ast::dump(*t);
  return out;
}

Parsed parse_pattern(const std::string& src) {
  Parsed out;
  synx::SourceFile file("t.snx", src);
  synx::Lexer lexer(file, out.sink);
  synx::Parser parser(file, lexer.tokenize(), out.sink);
  auto p = parser.parse_pattern_only();
  if (p) out.dump = synx::ast::dump(*p);
  return out;
}

Parsed parse_stmt(const std::string& src) {
  Parsed out;
  synx::SourceFile file("t.snx", src);
  synx::Lexer lexer(file, out.sink);
  synx::Parser parser(file, lexer.tokenize(), out.sink);
  auto s = parser.parse_statement_only();
  if (s) out.dump = synx::ast::dump(*s);
  return out;
}

Parsed parse_module(const std::string& src) {
  Parsed out;
  synx::SourceFile file("t.snx", src);
  synx::Lexer lexer(file, out.sink);
  synx::Parser parser(file, lexer.tokenize(), out.sink);
  auto m = parser.parse_module();
  out.dump = synx::ast::dump(m);
  return out;
}

#define EXPR_IS(src, want)                              \
  do {                                                  \
    Parsed _p = parse_expr(src);                        \
    CHECK_EQ(_p.dump, std::string(want));               \
    CHECK(!_p.sink.has_errors());                       \
    if (_p.dump != want) std::printf("  got: %s\n", _p.dump.c_str()); \
  } while (0)

#define TYPE_IS(src, want)                              \
  do {                                                  \
    Parsed _p = parse_type(src);                        \
    CHECK_EQ(_p.dump, std::string(want));               \
    CHECK(!_p.sink.has_errors());                       \
    if (_p.dump != want) std::printf("  got: %s\n", _p.dump.c_str()); \
  } while (0)

#define PAT_IS(src, want)                               \
  do {                                                  \
    Parsed _p = parse_pattern(src);                     \
    CHECK_EQ(_p.dump, std::string(want));               \
    CHECK(!_p.sink.has_errors());                       \
    if (_p.dump != want) std::printf("  got: %s\n", _p.dump.c_str()); \
  } while (0)

#define STMT_IS(src, want)                              \
  do {                                                  \
    Parsed _p = parse_stmt(src);                        \
    CHECK_EQ(_p.dump, std::string(want));               \
    CHECK(!_p.sink.has_errors());                       \
    if (_p.dump != want) std::printf("  got: %s\n", _p.dump.c_str()); \
  } while (0)

#define MOD_IS(src, want)                               \
  do {                                                  \
    Parsed _p = parse_module(src);                      \
    CHECK_EQ(_p.dump, std::string(want));               \
    CHECK(!_p.sink.has_errors());                       \
    if (_p.dump != want) std::printf("  got: %s\n", _p.dump.c_str()); \
  } while (0)

void test_precedence() {
  EXPR_IS("1 + 2 * 3", "(+ 1 (* 2 3))");
  EXPR_IS("(1 + 2) * 3", "(* (+ 1 2) 3)");
  EXPR_IS("a - b - c", "(- (- a b) c)");
  EXPR_IS("a == b && c || d", "(|| (&& (== a b) c) d)");
  EXPR_IS("a | b ^ c & d", "(| a (^ b (& c d)))");
  EXPR_IS("a << 2 + 1", "(<< a (+ 2 1))");
  EXPR_IS("a >> 2", "(>> a 2)");
  EXPR_IS("a > b > c", "(> (> a b) c)");
  EXPR_IS("-x * !y", "(* (unary - x) (unary ! y))");
  EXPR_IS("x as long + 1", "(+ (as x long) 1)");
  EXPR_IS("*p + 1", "(+ (unary * p) 1)");
  EXPR_IS("&x", "(& x)");
  EXPR_IS("&const x.y", "(&const (. x y))");
  EXPR_IS("0..10", "(.. 0 10)");
  EXPR_IS("a..=b + 1", "(..= a (+ b 1))");
  EXPR_IS("..3", "(.. _ 3)");
}

void test_postfix() {
  EXPR_IS("f(1, 2)", "(call f 1 2)");
  EXPR_IS("a.b.c", "(. (. a b) c)");
  EXPR_IS("p.length()", "(call (. p length))");
  EXPR_IS("Point.origin()", "(call (. Point origin))");
  EXPR_IS("items[0]", "(index items 0)");
  EXPR_IS("items[1..3]", "(index items (.. 1 3))");
  EXPR_IS("t.0", "(. t 0)");
  EXPR_IS("parse(s)?", "(try (call parse s))");
  EXPR_IS("x?.name", "(?. x name)");
  EXPR_IS("x ?? 0", "(?? x 0)");
  EXPR_IS("f(x)?.y?.z", "(?. (?. (call f x) y) z)");
  EXPR_IS("first<int>(items)", "(call (name first (targs int)) items)");
  EXPR_IS("Vec<int>.new()", "(call (. (name Vec (targs int)) new))");
  // "<" that is not followed by a matching ">(" stays a comparison.
  EXPR_IS("a < b", "(< a b)");
  EXPR_IS("a < b && c > d", "(&& (< a b) (> c d))");
}

void test_literals_and_structures() {
  EXPR_IS("42L", "42L");
  EXPR_IS("1.5f", "1.5f");
  EXPR_IS("'a'", "(char 97)");
  EXPR_IS("\"hi\"", "\"hi\"");
  EXPR_IS("true", "true");
  EXPR_IS("()", "(tuple)");
  EXPR_IS("(1, 2)", "(tuple 1 2)");
  EXPR_IS("[1, 2, 3]", "(array 1 2 3)");
  EXPR_IS("[0; 8]", "(array-repeat 0 8)");
  EXPR_IS("Point { x: 1.0, y }", "(struct Point (x 1.0) (y))");
  EXPR_IS("Shape.Rect { w: 1.0, h: 2.0 }", "(struct Shape.Rect (w 1.0) (h 2.0))");
  EXPR_IS("\"a {x} b\"", "(interp \"a \" x \" b\")");
  EXPR_IS("\"{p.len()}\"", "(interp (call (. p len)))");
  EXPR_IS("|a, b| a + b", "(closure (params (a) (b)) (+ a b))");
  EXPR_IS("|int a| -> int { a }", "(closure (params (int a)) (ret int) (block (tail a)))");
  EXPR_IS("|| 1", "(closure (params) 1)");
}

void test_control_flow() {
  EXPR_IS("if (x < 0) { -1 } else { 1 }", "(if (< x 0) (block (tail (unary - 1))) (block (tail 1)))");
  EXPR_IS("if (a) { 1 } else if (b) { 2 } else { 3 }",
          "(if a (block (tail 1)) (if b (block (tail 2)) (block (tail 3))))");
  EXPR_IS("match t { Some(v) => v, None => 0 }",
          "(match t (arm (Some v) v) (arm None 0))");
  EXPR_IS("match t { Token.Number(n) if n > 0 => { n } _ => 0, }",
          "(match t (arm (Token.Number n) (if (> n 0)) (block (tail n))) (arm _ 0))");
  // Struct literal is not allowed in the scrutinee position.
  EXPR_IS("match x { _ => 1 }", "(match x (arm _ 1))");
  EXPR_IS("unsafe { f() }", "(unsafe (block (tail (call f))))");
  EXPR_IS("return", "(return)");
  EXPR_IS("return x + 1", "(return (+ x 1))");
  EXPR_IS("await f()", "(await (call f))");
}

void test_types() {
  TYPE_IS("int", "int");
  TYPE_IS("Vec<int>", "Vec<int>");
  TYPE_IS("Map<String, Vec<int>>", "Map<String, Vec<int>>");
  TYPE_IS("&const str", "&const str");
  TYPE_IS("&Point", "&Point");
  TYPE_IS("*const byte", "*const byte");
  TYPE_IS("int[4]", "int[4]");
  TYPE_IS("int[]", "int[]");
  TYPE_IS("&const T[]", "&const T[]");
  TYPE_IS("int?", "int?");
  TYPE_IS("&const str?", "&const str?");
  TYPE_IS("(int, double)", "(int, double)");
  TYPE_IS("()", "()");
  TYPE_IS("fn(int, int) -> int", "fn(int, int) -> int");
  TYPE_IS("&const dyn Shape", "&const dyn Shape");
  TYPE_IS("std.io.File", "std.io.File");
  TYPE_IS("Self", "Self");
}

void test_patterns() {
  PAT_IS("_", "_");
  PAT_IS("x", "x");
  PAT_IS("None", "None");
  PAT_IS("Some(x)", "(Some x)");
  PAT_IS("Token.Plus", "Token.Plus");
  PAT_IS("Token.Number(n)", "(Token.Number n)");
  PAT_IS("Shape.Rect { w, h: height }", "(struct Shape.Rect (w) (h height))");
  PAT_IS("(a, b)", "(tuple a b)");
  PAT_IS("1 | 2 | 3", "(or 1 2 3)");
  PAT_IS("0..=9", "(..= 0 9)");
  PAT_IS("-1", "(unary - 1)");
  PAT_IS("\"yes\"", "\"yes\"");
  PAT_IS("Ok(_)", "(Ok _)");
  PAT_IS("Err(ParseError.NotANumber(s))", "(Err (ParseError.NotANumber s))");
}

void test_statements() {
  STMT_IS("int x = 5;", "(var int x 5)");
  STMT_IS("var y = 5;", "(var y 5)");
  STMT_IS("const int z = 5;", "(const int z 5)");
  STMT_IS("const w = 5;", "(const w 5)");
  STMT_IS("int x;", "(var int x)");
  STMT_IS("int[3] a = [1, 2, 3];", "(var int[3] a (array 1 2 3))");
  STMT_IS("Vec<int> v = Vec.new();", "(var Vec<int> v (call (. Vec new)))");
  STMT_IS("&const str s = \"x\";", "(var &const str s \"x\")");
  STMT_IS("Point p = Point { x: 1.0, y: 2.0 };", "(var Point p (struct Point (x 1.0) (y 2.0)))");
  STMT_IS("x = 5;", "(= x 5)");
  STMT_IS("self.x *= k;", "(*= (. self x) k)");
  STMT_IS("*p = 1;", "(= (unary * p) 1)");
  STMT_IS("a < b;", "(< a b)");
  STMT_IS("f(x);", "(call f x)");
  STMT_IS("while (i < 10) { i += 1; }", "(while (< i 10) (block (+= i 1)))");
  STMT_IS("loop { break; }", "(loop (block (break)))");
  STMT_IS("for (i in 0..10) { }", "(for i (.. 0 10) (block))");
  STMT_IS("for ((k, v) in map) { }", "(for (tuple k v) map (block))");
  STMT_IS("for (t in &const tokens) { }", "(for t (&const tokens) (block))");
  STMT_IS("if (a) { b(); }", "(if a (block (call b)))");
  STMT_IS("return 0;", "(return 0)");
}

void test_items() {
  MOD_IS("int add(int a, int b) { return a + b; }",
         "(fn add (ret int) (params (int a) (int b)) (block (return (+ a b))))");
  MOD_IS("add(int a, int b) { return a + b; }",
         "(fn add (params (int a) (int b)) (block (return (+ a b))))");
  MOD_IS("main() { print(\"hi\"); }", "(fn main (params) (block (call print \"hi\")))");
  MOD_IS("T max<T: Ord>(T a, T b) { a }",
         "(fn max (generics (T Ord)) (ret T) (params (T a) (T b)) (block (tail a)))");
  MOD_IS("Result<int, E> f() { }", "(fn f (ret Result<int, E>) (params) (block))");
  MOD_IS("&const T bigger<T: Shape>(&const T a, &const T b) { a }",
         "(fn bigger (generics (T Shape)) (ret &const T) (params (&const T a) (&const T b)) (block (tail a)))");
  MOD_IS("pub async void run() { }", "(fn run pub async (ret void) (params) (block))");
  MOD_IS("import std.io;", "(import std.io)");
  MOD_IS("import std.io.print;", "(import std.io.print)");
  MOD_IS("import std.collections.{Vec, Map};", "(import std.collections {Vec, Map})");
  MOD_IS("const int MAX = 100;", "(const int MAX 100)");
  MOD_IS("struct Point { double x; double y; }",
         "(struct Point (field double x) (field double y))");
  MOD_IS("struct Point { double x; double len(&const self) { 0.0 } void scale(&self, double k) { } Point origin() { Point { x: 0.0 } } }",
         "(struct Point (field double x) (fn len (ret double) (params (&const self)) (block (tail 0.0))) "
         "(fn scale (ret void) (params (&self) (double k)) (block)) "
         "(fn origin (ret Point) (params) (block (tail (struct Point (x 0.0))))))");
  MOD_IS("struct Vec<T> { pub usize len; }", "(struct Vec (generics (T)) (field pub usize len))");
  MOD_IS("enum Option<T> { None, Some(T), }",
         "(enum Option (generics (T)) (variant None) (variant Some T))");
  MOD_IS("enum Shape { Circle(double), Rect { double w; double h; } }",
         "(enum Shape (variant Circle double) (variant Rect (field double w) (field double h)))");
  MOD_IS("enum Token { Plus, End, describe(&const self) { 1 } int value(&const self) { 2 } }",
         "(enum Token (variant Plus) (variant End) (fn describe (params (&const self)) (block (tail 1))) "
         "(fn value (ret int) (params (&const self)) (block (tail 2))))");
  MOD_IS("trait Shape { double area(&const self); &const str name(&const self) { \"shape\" } }",
         "(trait Shape (fn area (ret double) (params (&const self))) "
         "(fn name (ret &const str) (params (&const self)) (block (tail \"shape\"))))");
  MOD_IS("impl Shape for Circle { double area(&const self) { 1.0 } }",
         "(impl Shape for Circle (fn area (ret double) (params (&const self)) (block (tail 1.0))))");
  MOD_IS("impl<T: Copy> Copy for Wrapper<T> { }",
         "(impl (generics (T Copy)) Copy for Wrapper<T>)");
  MOD_IS("extern \"C\" { int puts(*const byte s); }",
         "(extern \"C\" (fn puts (ret int) (params (*const byte s))))");
  MOD_IS("/// Adds.\nint add(int a) { a }", "(fn add (ret int) (params (int a)) (block (tail a)))");
}

void test_doc_comments_attach() {
  synx::DiagnosticSink sink;
  synx::SourceFile file("t.snx", "/// Adds one.\n/// Really.\nint inc(int a) { a + 1 }");
  synx::Lexer lexer(file, sink);
  synx::Parser parser(file, lexer.tokenize(), sink);
  auto m = parser.parse_module();
  CHECK_EQ(m.items.size(), 1u);
  CHECK_EQ(m.items[0]->doc, std::string("Adds one.\nReally."));
  CHECK(!sink.has_errors());
}

void test_errors_and_recovery() {
  // A missing semicolon is reported once and parsing continues.
  Parsed p = parse_module("main() { int x = 5 int y = 6; }");
  CHECK_EQ(p.sink.error_count(), 1u);

  // Two independent errors in two functions are both reported.
  p = parse_module("a() { x = ; }\nb() { y = ; }");
  CHECK_EQ(p.sink.error_count(), 2u);

  // The module still contains both functions.
  CHECK(p.dump.find("(fn a") != std::string::npos);
  CHECK(p.dump.find("(fn b") != std::string::npos);

  // self outside a method.
  p = parse_module("f(&self) { }");
  CHECK_EQ(p.sink.error_count(), 1u);

  // Trait signatures need a semicolon or body.
  p = parse_module("trait T { int f(&const self) }");
  CHECK(p.sink.error_count() >= 1u);

  // A stray token inside a struct variant must not loop forever.
  p = parse_module("enum E { A { int x; ) int y } }");
  CHECK(p.sink.error_count() >= 1u);
  p = parse_module("enum E { A { ) } }");
  CHECK(p.sink.error_count() >= 1u);

  // Unknown tokens from the lexer surface as parse errors without crashing.
  p = parse_module("main() { @ }");
  CHECK(p.sink.error_count() >= 1u);

  // Error messages carry positions.
  synx::SourceFile file("t.snx", "main() { int x = ; }");
  synx::DiagnosticSink sink;
  synx::Lexer lexer(file, sink);
  synx::Parser parser(file, lexer.tokenize(), sink);
  parser.parse_module();
  CHECK_EQ(sink.error_count(), 1u);
  std::string r = sink.render(file);
  CHECK(r.find("t.snx:1:18: error[SNX0201]: expected an expression, found ;") == 0);
}

}  // namespace

int main() {
  test_precedence();
  test_postfix();
  test_literals_and_structures();
  test_control_flow();
  test_types();
  test_patterns();
  test_statements();
  test_items();
  test_doc_comments_attach();
  test_errors_and_recovery();
  return synx_test::finish("parser_test");
}
