#include <string>
#include "synx/compile.hpp"
#include "test_support.hpp"

static void check(const char* label, const std::string& source, const std::string& expected) {
  auto c = synx::check_source("stdlib.snx", "import std.fs.File; " + source);
  if (!c->sink.has_errors()) c->run_lowering();
  std::string errors = c->sink.render(c->user_file);
  ++synx_test::g_checks;
  if ((expected.empty() && c->sink.has_errors()) ||
      (!expected.empty() && errors.find(expected) == std::string::npos)) {
    ++synx_test::g_failures;
    std::printf("FAIL %s: expected '%s'\n%s", label, expected.c_str(), errors.c_str());
  }
}
int main() {
  check("close consumes", "main() { match File.create(\"x\") { Ok(f) => { f.close(); f.flush(); } Err(_) => {} } }", "use of moved value");
  check("file is move only", "main() { match File.open(\"x\") { Ok(f) => { var other=f; f.position(); } Err(_) => {} } }", "use of moved value");
  check("private handle", "main() { match File.open(\"x\") { Ok(f) => { var p=f.handle; } Err(_) => {} } }", "private to module");
  check("private constructor", "main() { File f=File { handle:0 as *byte, mode:0, direction:0 }; }", "private to module");
  check("file needs exclusive access", "void f(&const File file) { file.flush(); }", "read only");
  check("raw pointer needs unsafe", "main() { var v=Vec<int>.new(); var p=v.as_mut_ptr(); *p=1; }", "unsafe");
  check("path storage is private", "import std.path.Path; main() { var p=Path.from(\"a\"); var text=p.text; }", "private to module");
  check("path keeps source independent", "import std.path.Path; main() { Path p; { var text=String.from(\"a\"); p=Path.from(text); } p.is_absolute(); }", "");
  check("int literal too large", "main() { int x=2147483648; }", "SNX0413");
  check("inferred literal too large", "main() { var x=4294967295; }", "SNX0413");
  check("byte literal too large", "main() { byte x=256; }", "SNX0413");
  check("long literal too large", "main() { long x=9223372036854775808; }", "SNX0413");
  check("negative literal too small", "main() { int x=-2147483649; }", "SNX0413");
  check("literal minima and explicit casts", "main() { byte b=256 as byte; int i=-2147483648; short s=-32768; long l=-9223372036854775808; ulong u=18446744073709551615; }", "");
  check("mem swap alias", "import std.mem; main() { int x=1; mem.swap(&x, &x); }", "borrowed");
  check("mem replace stores loan", "import std.mem; main() { int outer=1; var r=&const outer; { int inner=2; var old=mem.replace(&r, &const inner); } int n=*r; }", "scope");
  check("option take keeps payload loan", "main() { Option<&const int> r; { int inner=2; var option=Some(&const inner); r=option.take(); } int n=*r.unwrap(); }", "scope");
  check("mem swap exchanges loans", "import std.mem; main() { int outer=1; var a=&const outer; { int inner=2; var b=&const inner; mem.swap(&a, &b); } int n=*a; }", "scope");
  check("option replace stores loan", "main() { int outer=1; var option=Some(&const outer); { int inner=2; var old=option.replace(&const inner); } int n=*option.unwrap(); }", "scope");
  check("option mutable loan", "main() { int? x=Some(1); var r=x.as_mut(); var old=x.take(); int n=*r.unwrap(); }", "borrowed");
  check("explicit drop moves owner", "import std.mem; main() { var b=Box<int>.new(1); mem.drop(b); int n=*b.as_ref(); }", "moved");
  return synx_test::finish("stdlib_test");
}
