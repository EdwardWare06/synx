#include <string>
#include "synx/compile.hpp"
#include "test_support.hpp"

namespace {
const std::string base = std::string(SYNX_SOURCE_DIR) + "/tests/run/modules/";
// Checks that the text produces exactly `count` errors.
void error_count(const char* label, const std::string& text, size_t count) {
  auto c = synx::check_source(base + "check.snx", text);
  size_t found = 0;
  for (auto& d : c->sink.all()) if (d.severity == synx::Severity::Error) ++found;
  ++synx_test::g_checks;
  if (found != count) {
    ++synx_test::g_failures;
    std::printf("FAIL %s: expected %zu errors, got %zu\n%s", label, count, found, c->sink.render(c->user_file).c_str());
  }
}
void check(const char* label, const std::string& text, const std::string& expected = "") {
  auto c = synx::check_source(base + "check.snx", text);
  if (!c->sink.has_errors()) c->run_lowering();
  std::string errors = c->sink.render(c->user_file);
  ++synx_test::g_checks;
  if ((expected.empty() && c->sink.has_errors()) || (!expected.empty() && errors.find(expected) == std::string::npos)) {
    ++synx_test::g_failures;
    std::printf("FAIL %s: expected '%s'\n%s", label, expected.c_str(), errors.c_str());
  }
}
}
int main() {
  check("module namespace", "import left; import right; main() { var a = left.make(); var b = right.make(); int n = a.read() + b.read(); }");
  check("inferred context", "import infer_a; import infer_b; main() { int n = infer_a.first() + infer_b.second(); }");
  check("pub re-export", "import facade; main() { var a = facade.make(); var t = facade.Token.End; }");
  check("no root leak", "import left; int secret() { return 1; } main() {}", "");
  check("private through path", "import private; main() { int n = private.secret(); }", "private to module");
  error_count("private through path reports once", "import private; main() { int n = private.secret(); }", 1);
  check("private through import", "import private.secret; main() {}", "private to module");
  check("private field", "import private; main() { var s = private.make(); int n = s.n; }", "private to module");
  check("private method", "import private; main() { var s = private.make(); int n = s.read(); }", "private to module");
  check("private literal", "import private; main() { var s = private.Hidden { n: 1 }; }", "private to module");
  check("missing module", "import absent; main() {}", "cannot find imported module");
  check("name collision", "import left.make; int make() { return 0; } main() {}", "conflicts");
  check("module source diagnostic", "import broken; main() {}", "broken.snx:2:12");
  check("missing imported item", "import left.absent; main() {}", "cannot resolve import");
  check("private import not reexported", "import facade; main() { var x = facade.left.make(); }", "no public item");
  check("constant import cycle", "import const_a; main() {}", "cyclic constant");
  check("private callback", "import private_callback; main() { var s = private_callback.make(); int n = s.callback(); }", "private to module");
  check("private destructuring", "import private; main() { var s = private.make(); match s { private.Hidden { n } => { int value = n; } } }", "private to module");
  check("root declarations stay local", "import foreign_root; int only_root() { return 1; } main() {}", "unknown name 'only_root'");
  check("nested source declaration", "import nested_user; main() { var p = nested_user.make(); int x = p.x; }", "");
  check("namespace isolation", "import isolation; import bundle.child; main() {}", "no public item 'child'");
  check("parent namespace stays private", "import export_child; main() { int n = export_child.bundle.base(); }", "private to module");
  check("explicit module re-export", "import export_child; main() { int n = export_child.child.value(); }");
  check("seeded re-export cycle", "import reexport_b; main() { int n = reexport_b.seed(); }");
  check("unresolved re-export cycle", "import reexport_bad_a; main() {}", "cyclic re-export");
  check("parent source stays unimported", "import parent_only; import bundle.child; main() { int n = bundle.base(); }", "no public item 'base'");
  check("constant block locals", R"(import constants; main() { int n = 99; int value = constants.BLOCK; print("{n} {value}"); })");
  check("constant cannot return", "const VALUE = { return; }; main() {}", "constant initializers cannot");
  check("constant cannot propagate", "const VALUE = { int? n = None; n? }; main() {}", "constant initializers cannot");
  check("canonical file aliases", "import utils; import utils.mod; int use(utils.Tag t) { return t.n; } main() { var t = mod.Tag { n: 2 }; int n = use(t); }");
  return synx_test::finish("module_test");
}
