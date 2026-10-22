// The strict AST dump is the oracle for the Synx parser port. These checks
// confirm that it names every node kind, prints spans, bit-exact floats and
// escaped bytes, and fails to hide recovery results.
#include <string>
#include <vector>

#include "synx/ast.hpp"
#include "synx/diagnostics.hpp"
#include "synx/lexer.hpp"
#include "synx/parser.hpp"
#include "synx/source.hpp"
#include "test_support.hpp"

namespace {

std::string strict_module(const std::string& src, size_t* errors = nullptr) {
  synx::SourceFile file("t.snx", src);
  synx::DiagnosticSink sink;
  synx::Lexer lexer(file, sink);
  synx::Parser parser(file, lexer.tokenize(), sink);
  auto module = parser.parse_module();
  if (errors) *errors = sink.error_count();
  if (sink.has_errors() && !errors) std::fputs(sink.render(file).c_str(), stderr);
  return synx::ast::dump_strict(module);
}

std::string strict_expr(const std::string& src) {
  synx::SourceFile file("t.snx", src);
  synx::DiagnosticSink sink;
  synx::Lexer lexer(file, sink);
  synx::Parser parser(file, lexer.tokenize(), sink);
  auto e = parser.parse_expression();
  CHECK(!sink.has_errors());
  return e ? synx::ast::dump_strict(*e) : std::string();
}

std::string strict_type(const std::string& src) {
  synx::SourceFile file("t.snx", src);
  synx::DiagnosticSink sink;
  synx::Lexer lexer(file, sink);
  synx::Parser parser(file, lexer.tokenize(), sink);
  auto t = parser.parse_type_only();
  CHECK(!sink.has_errors());
  return t ? synx::ast::dump_strict(*t) : std::string();
}

const char* kRich = R"SNX(/// Rich module.
import std.io;
import std.collections.{HashMap, Vec};
pub import std.mem.drop;
const int LIMIT = 10;
extern "C" { void synx_rt_exit(int code); }
/// A point.
struct Point<T: Eq> { /// The x field.
    pub T x; int y; int sum(&const self) { return 1; } }
enum Shape { Unit, Pair(int, bool), /// Named.
    Named { int w; int h; }, int id(self) { 1 } }
trait Area<T> { int area(&const self); int twice(&const self) { 2 } }
impl Area<int> for Point<int> { int area(&const self) { 1 } }
unsafe impl Send for Point<int> {}
pub async send int work(int n) { return await other(n); }
void types(&const int a, *const byte b, int[4] c, int[] d, int? e, (int, bool) f,
           fn once send(int) -> int g, &const dyn Area<int> h, Self i, &int j, *int k) { }
int body(int a, int[] items) {
    int x = 5; var y = 1; const int z = 3; int w;
    x += 1; items[0] = 2; x <<= 1;
    while (x < 10) { x += 1; continue; }
    loop { break; }
    for (i in 0..=3) { y = i; }
    for (i in 0..3) { }
    var f = |int n| -> int { n };
    var g = [&x, y] const |int n| n + x;
    var h = once || 1;
    var s = "a{x + 1}b"; var plain = "text";
    var c = 'c';
    var fl = 1.5f + 2.25;
    var big = 12L;
    var t = (1, 2);
    var unit = ();
    var arr = [1, 2, 3];
    var rep = [0; 4];
    var cast = x as long;
    var m = f()?;
    var o = a?.b;
    var d = a ?? 1;
    var r = &x;
    var cr = &const x;
    var p = Point { x: 1, y };
    var u = unsafe { *r };
    var neg = -x + a * 2 - ~a;
    var inv = !true;
    var gen = make<int>(1);
    var fld = p.x.y;
    var idx = items[1];
    match x { 1 => 2, 2..=3 => 4, Some(v) if v > 1 => v, Shape.Named { w, h: hh } => w,
              (a, b) => a, 7 | 8 => 0, Shape.Unit => 1, name => 2, _ => 3 }
    if (a == 1) { 1 } else if (a != 2) { 2 } else { 3 }
    return x;
}
)SNX";

void test_every_kind_is_named() {
  size_t errors = 0;
  std::string out = strict_module(kRich, &errors);
  if (errors) std::printf("rich module has %zu parse errors\n", errors);
  CHECK_EQ(errors, 0u);
  const char* names[] = {
      // expressions
      "IntLit", "FloatLit", "CharLit", "StringLit", "BoolLit", "Interp", "Name", "Field", "Call", "Index",
      "Unary", "Binary", "Cast", "Try", "OptChain", "Coalesce", "Range", "Ref", "StructLit", "Tuple",
      "Array", "ArrayRepeat", "BlockExpr", "If", "Match", "Closure", "Unsafe", "Await", "Return", "Break",
      "Continue",
      // patterns
      "Wildcard", "LiteralPattern", "Binding", "PathPattern", "TupleStructPattern", "StructPattern",
      "TuplePattern", "OrPattern", "RangePattern",
      // statements
      "VarDecl", "ExprStmt", "Assign", "While", "Loop", "For",
      // types
      "PrimitiveType", "PathType", "RefType", "PtrType", "ArrayType", "SliceType", "OptionalType",
      "TupleType", "FnType", "DynType", "SelfType",
      // items and parts
      "Import", "Const", "FunctionItem", "Struct", "Enum", "Trait", "Impl", "Extern", "Function", "Param",
      "Bound", "Generic", "Variant", "FieldDecl", "Capture", "ClosureParam", "Arm", "Init", "FieldPattern",
      "Part", "Segment", "Path", "Block", "Module"};
  for (const char* name : names) {
    std::string marker = std::string("(") + name + " ";
    bool found = out.find(marker) != std::string::npos;
    if (!found) std::printf("strict dump never prints %s\n", name);
    CHECK(found);
  }
  CHECK(out.find("(Module [") == 0);
}

void test_exact_forms() {
  CHECK_EQ(strict_expr("1.5"), std::string("(FloatLit @0-3:0 #3ff8000000000000 f)"));
  CHECK_EQ(strict_expr("0.1f"), std::string("(FloatLit @0-4:0 #3fb999999999999a t)"));
  CHECK_EQ(strict_expr("12L"), std::string("(IntLit @0-3:0 12 t)"));
  CHECK_EQ(strict_expr("\"a\\n\\\"b\\\\\""), std::string("(StringLit @0-10:0 \"a\\x0a\\x22b\\x5c\")"));
  CHECK_EQ(strict_expr("a + b"),
           std::string("(Binary @0-5:0 + (Name @0-1:0 \"a\" []) (Name @4-5:0 \"b\" []))"));
  CHECK_EQ(strict_type("int?"), std::string("(OptionalType @0-4:0 (PrimitiveType @0-3:0 int))"));
  std::string fn_type = strict_type("fn once send(int) -> int");
  CHECK(fn_type.find("(FnType @0-") == 0);
  CHECK(fn_type.find(" once t [(PrimitiveType ") != std::string::npos);
  // Non-ASCII bytes print as escapes so reports stay ASCII.
  CHECK_EQ(strict_expr("\"\xc3\xa9\""), std::string("(StringLit @0-4:0 \"\\xc3\\xa9\")"));
}

void test_recovery_is_visible() {
  size_t errors = 0;
  std::string a = strict_module("main() { int x = ; }", &errors);
  CHECK(errors >= 1u);
  // Dropping a token must change the dump, so recovery differences show up.
  std::string b = strict_module("main() { int x = 1; }", &errors);
  CHECK(a != b);
  CHECK(strict_module("", &errors) == "(Module [])");
}

void test_flags_are_visible() {
  size_t errors = 0;
  std::string plain = strict_module("void f() { }", &errors);
  std::string pub = strict_module("pub void f() { }", &errors);
  std::string async = strict_module("async void f() { }", &errors);
  std::string send = strict_module("async send void f() { }", &errors);
  CHECK(plain != pub);
  CHECK(plain != async);
  CHECK(async != send);
  // Variants of unit and empty tuple kind differ even with no payload types.
  CHECK(strict_module("enum E { A, B() }", &errors).find("unit") != std::string::npos);
  CHECK(strict_module("enum E { A, B() }", &errors).find("tuple") != std::string::npos);
  // has_semi and doc text are printed.
  CHECK(strict_module("void f() { 1; 2 }", &errors).find("ExprStmt") != std::string::npos);
  CHECK(strict_module("/// note\nvoid f() { }", &errors).find("\"note\"") != std::string::npos);
}

}  // namespace

int main() {
  test_every_kind_is_named();
  test_exact_forms();
  test_recovery_is_visible();
  test_flags_are_visible();
  return synx_test::finish("ast_strict_test");
}
