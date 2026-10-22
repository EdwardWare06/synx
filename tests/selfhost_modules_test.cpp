// Declaration/module parity before signature resolution. Retain failed corpora.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <random>
#include <stdexcept>
#include <thread>

#include "selfhost_support.hpp"
#include "synx/compile.hpp"
#include "synx/lexer.hpp"
#include "test_support.hpp"

using namespace selfhost_test;

static void write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!out) throw std::runtime_error("cannot write " + utf8(path));
}
struct EnvScope {
  std::string saved;
#ifdef _WIN32
  std::wstring saved_w;
#endif
  bool present;
  explicit EnvScope(const std::string& value, bool unset = false) {
#ifdef _WIN32
    const wchar_t* old = _wgetenv(L"SYNX_STD_PATH"); present = old != nullptr; if (old) saved_w = old;
#else
    const char* old = std::getenv("SYNX_STD_PATH"); present = old != nullptr; if (old) saved = old;
#endif
    if (unset) clear(); else set(value);
  }
  static void set(const std::string& value) {
#ifdef _WIN32
    _wputenv_s(L"SYNX_STD_PATH", wide(value).c_str());
#else
    setenv("SYNX_STD_PATH", value.c_str(), 1);
#endif
  }
  static void clear() {
#ifdef _WIN32
    _wputenv_s(L"SYNX_STD_PATH", L"");
#else
    unsetenv("SYNX_STD_PATH");
#endif
  }
  ~EnvScope() {
    if (present) {
#ifdef _WIN32
      _wputenv_s(L"SYNX_STD_PATH", saved_w.c_str());
#else
      set(saved);
#endif
    }
    else clear();
  }
};
using Files = std::vector<std::pair<std::string, std::string>>;
static void project(std::vector<fs::path>& inputs, const fs::path& root, const char* label, const Files& files) {
  auto folder = root / label;
  for (auto& [name, text] : files) write(folder / name, text);
  inputs.push_back(folder / "main.snx");
}
static void corpus(std::vector<fs::path>& inputs, const fs::path& root) {
  project(inputs, root, "declarations", {
    {"main.snx", "pub struct S<T, T> { int x; f() {} f() {} }\n"
                  "enum E<T> { A, A, B(int), C { int x; } f() {} f() {} }\n"
                  "trait Tr<T> { f(&const self); f(&const self); }\n"
                  "impl<T> Tr<T> for S<T> { f(&const self) {} }\n"
                  "const int C = 1; const int C = 2; int C() { 0 }\n"
                  "struct C {} enum C { A } trait C {} extern \"C\" { int C(); }\n"
                  "pub main() {} main() {}"}});
  project(inputs, root, "cycle", {{"main.snx", "import a; import b; main() {}"},
      {"a.snx", "import b; pub struct A {}"}, {"b.snx", "import a; pub struct B {}"}});
  project(inputs, root, "diamond", {{"main.snx", "import a; import b; import leaf.Item;"},
      {"a.snx", "pub import leaf.Item;"}, {"b.snx", "pub import leaf.Item;"}, {"leaf.snx", "pub struct Item {}"}});
  project(inputs, root, "reexport", {{"main.snx", "import a.{Thing, run}; import d;"},
      {"a.snx", "pub import b.{Thing, run};"}, {"b.snx", "pub import c.{Thing, run};"},
      {"c.snx", "pub struct Thing {} pub run() {}"}, {"d.snx", "import a.Thing;"}});
  project(inputs, root, "bad-cycle", {{"main.snx", "import a.X;"}, {"a.snx", "pub import b.X;"}, {"b.snx", "pub import a.X;"}});
  project(inputs, root, "privacy", {{"main.snx", "import a.{Public, Secret}; import a.hidden; import b.Secret;"},
      {"a.snx", "pub struct Public {} struct Secret {} hidden() {}"}, {"b.snx", "pub import a.Secret;"}});
  project(inputs, root, "groups", {{"main.snx", "import a.{A, B}; import a.A; pub import a.A; import a.{A, Missing};"},
      {"a.snx", "pub struct A {} pub enum B { X, Y }"}});
  project(inputs, root, "variants", {{"main.snx", "import a.E.{X, Y}; import a.E.X; import std.prelude.Option.Some;"},
      {"a.snx", "pub enum E { X, Y(int) }"}});
  project(inputs, root, "collision", {{"main.snx", "struct A {} import a.A; import a.A;"}, {"a.snx", "pub struct A {}"}});
  project(inputs, root, "namespace", {{"main.snx", "struct pack {} import pack.leaf.Item; import pack;"},
      {"pack/leaf.snx", "pub struct Item {}"}, {"pack/mod.snx", "pub struct leaf {}"}});
  project(inputs, root, "overlay-conflict", {{"main.snx", "import pack.leaf; import pack;"},
      {"pack/leaf.snx", "pub main() {}"}, {"pack/mod.snx", "pub struct leaf {}"}});
  project(inputs, root, "isolation", {{"main.snx", "import a; import pack.leaf; import b;"},
      {"a.snx", "import pack;"}, {"b.snx", "import a.leaf;"},
      {"pack/mod.snx", "pub struct Item {}"}, {"pack/leaf.snx", "pub struct Leaf {}"}});
  project(inputs, root, "directory", {{"main.snx", "import a; import a.b;"},
      {"a/mod.snx", "pub struct A {}"}, {"a/b/mod.snx", "pub struct B {}"}});
  project(inputs, root, "file-priority", {{"main.snx", "import a.X;"}, {"a.snx", "pub struct X {}"}, {"a/mod.snx", "pub struct Y {}"}});
  project(inputs, root, "file-as-directory", {{"main.snx", "import a.b; import a.X;"}, {"a", "ordinary file"}, {"a.snx", "pub struct X {}"}});
  project(inputs, root, "missing", {{"main.snx", "import missing; import std.no_such_module.Item; import std;"}});
  project(inputs, root, "prelude", {{"main.snx", "import std.prelude.{Vec, String, Option, Some, print}; import std.prelude;"}});
  project(inputs, root, "broken-import", {{"main.snx", "import a; import b;"}, {"a.snx", "pub struct A { int x; ) }"}, {"b.snx", "pub struct B {}"}});
  project(inputs, root, "broken-entry", {{"main.snx", "import a; struct { )"}, {"a.snx", "pub struct A {}"}});
  auto unicode = root / fs::path(u8"module caf\u00e9 \u03bb");
  write(unicode / "main.snx", "import leaf.Item; import leaf.Item;");
  write(unicode / "leaf.snx", "pub struct Item {}"); inputs.push_back(unicode / "main.snx");
  auto alias = root / "hardlinks";
  write(alias / "main.snx", "import a; import z; import alias; import main; import a.A; import z.A;");
  write(alias / "a.snx", "pub struct A {}");
  fs::create_hard_link(alias / "a.snx", alias / "z.snx");
  fs::create_hard_link(alias / "main.snx", alias / "alias.snx");
  inputs.push_back(alias / "main.snx");
  // Symlinks can require privileges on Windows. Test each supported operation.
  auto links = root / "symlinks";
  write(links / "main.snx", "import pack; import alias; import pack.Item;");
  write(links / "real/mod.snx", "pub struct Item {}");
  std::error_code symlink_error;
  fs::create_directory_symlink(links / "real", links / "pack", symlink_error);
  if (!symlink_error) {
    fs::create_symlink(links / "real/mod.snx", links / "alias.snx", symlink_error);
    if (!symlink_error) inputs.push_back(links / "main.snx");
  }
  auto loop = root / "symlink-loop";
  write(loop / "main.snx", "import looping;");
  symlink_error.clear(); fs::create_symlink(loop / "looping.snx", loop / "looping.snx", symlink_error);
  if (!symlink_error) inputs.push_back(loop / "main.snx");
  auto denied = root / "unreadable";
  write(denied / "main.snx", "import denied; import leaf.Item;");
  write(denied / "denied.snx", "pub struct Secret {}");
  write(denied / "leaf.snx", "pub struct Item {}");
  std::error_code permission_error;
  fs::permissions(denied / "denied.snx", fs::perms::none, permission_error);
  if (!permission_error && !synx::SourceFile::load_utf8(utf8(denied / "denied.snx"))) inputs.push_back(denied / "main.snx");
  else fs::permissions(denied / "denied.snx", fs::perms::owner_all, permission_error);
  // Several file identities, reached under names in a different lexical order.
  for (int seed = 0; seed < 16; ++seed) {
    std::mt19937 random(0x534e5804u + static_cast<unsigned>(seed));
    auto folder = root / ("graph-" + std::to_string(seed));
    write(folder / "main.snx", "import n5; import n2; import n0; import n7.Item;"); inputs.push_back(folder / "main.snx");
    for (int n = 0; n < 8; ++n) {
      std::string text = "pub struct Item {}\n";
      for (int edge = 0; edge < 3; ++edge) text += "import n" + std::to_string(random() % 8) + ";\n";
      write(folder / ("n" + std::to_string(n) + ".snx"), text);
    }
  }
  // Mutate complete tokens in module-heavy entries. Keep sibling files valid.
  const std::string original = "import a.{A, E}; pub import b.run; import a.E.X; struct Root {} main() {}";
  synx::SourceFile source("mutation.snx", original); synx::DiagnosticSink sink;
  auto tokens = synx::Lexer(source, sink).tokenize();
  size_t number = 0;
  for (auto& token : tokens) {
    if (token.span.start == token.span.end) continue;
    for (const std::string replacement : {"", ".", "pub", "Missing", "import", "{"}) {
      auto text = original.substr(0, token.span.start) + replacement + original.substr(token.span.end);
      const std::string label = "mutation-" + std::to_string(number++);
      project(inputs, root, label.c_str(), {{"main.snx", text}, {"a.snx", "pub struct A {} pub enum E { X, Y(int) }"}, {"b.snx", "pub run() {}"}});
    }
  }
}
// Stage 4b fixtures. Declarations succeed, so signature resolution runs and
// reports type, bound, parameter, return, and trait method-set diagnostics.
static void signature_corpus(std::vector<fs::path>& inputs, const fs::path& root) {
  const std::string shapes = "pub struct Shape { pub int sides; } pub struct Pair<A, B> { pub A first; pub B second; }\n"
                             "struct Hidden {} pub trait Area { double area(&const self); }";
  project(inputs, root, "sig-types", {{"main.snx",
    "import shapes.{Shape, Pair, Area}; import shapes; import std.prelude;\n"
    "trait Show<T> { T show(&const self); int count(&const self) { 0 } }\n"
    "struct Holder<T: Show<int> + Eq, U> { T value; U[4] items; T? maybe; (T, U) pair; () unit; fn(T) -> bool test;\n"
    "  fn const() shared; fn once send(int) -> U done; &const dyn Show<int> shown; *const T raw; *byte bytes;\n"
    "  Pair<T, U>? link; shapes.Shape shape; &const str text; &const int[] view; Vec<String> names; Result<int, String> result;\n"
    "  prelude.Vec<T> listed; std.prelude.Option<U> chosen; &dyn Area area;\n"
    "  Self? copy(&const self) { None } U first<V: Show<U>>(&self, V other, &const fn(&const V) -> U f) { f(&other) }\n"
    "  Holder<T, U> make(T value, U unit) { panic(\"x\") } }\n"
    "enum Tree<T> { Leaf, Node(Box<Tree<T>>, T, Self?), Named { T value; Tree<T>[2] kids; }, Bare(str), Wide { str s; } int size(self) { 0 } }\n"
    "impl<T: Eq> Show<T> for Tree<T> { T show(&const self) { panic(\"x\") } }\n"
    "impl<A: Eq + Ord> Eq for Pair<A, A> { bool eq(&const self, &const Self other) { true } }\n"
    "impl Area for Shape { double area(&const self) { 1.0 } }\n"
    "pub const int LIMIT = 4; const long BIG = 1; const NAME = \"n\"; const int[2] PAIR = [1, 2];\n"
    "extern \"C\" { int c_call(*const byte data, usize n); }\n"
    "pub int run<A, B: Show<A>>(A a, &const B b, (A, B)? both, fn once() -> A later) { 0 }\n"
    "inferred(int x) { x } main() {}"}, {"shapes.snx", shapes}});
  project(inputs, root, "sig-errors", {{"main.snx",
    "import shapes;\n"
    "struct S { str name; int[] items; dyn Tr d; int x; int x; int x; Self me; }\n"
    "enum E { A(str), B { str s; Missing m; }, C(Self, Nope<int>) }\n"
    "trait Tr { int need(&const self); int extra(&const self) { 1 } nobody(&const self); with_body(&const self) { 0 } }\n"
    "trait Two<A, B> {} struct G<T> { T value; }\n"
    "impl Tr for S { int extra(&const self) { 2 } int unknown(&const self) { 3 } }\n"
    "impl Missing for S { int any(&const self) { 0 } } impl Two<int> for G<int> {} impl<T> Tr for Self {}\n"
    "impl shapes.Area for G<Missing> {} impl S for G<int> {}\n"
    "pub f1() {} int f2(Self s) { 0 } int f4(int a, int a, int a) { 0 } int f5<T, T>(T x) { 0 }\n"
    "int f6(str s, int[] xs, dyn Tr t) { 0 } int f7(T<int> x) { 0 } int f8<T>(T<int> x) { 0 } int f9(str<int> s) { 0 }\n"
    "int f10(Missing m, f2 g, G g2, G<int, int> g3, Tr t, E<int> e, shapes g4) { 0 } int f11(int[N] a, int[1 + 1] b) { 0 }\n"
    "int f12<T: Missing + Two<int> + f2>(T x) { 0 } int f13(&const dyn Missing d, &const dyn Two<int> e, &const dyn G<int> g) { 0 }\n"
    "int f14(shapes.Hidden h, shapes.Nope n, shapes.Shape.x y, Missing.Type z) { 0 } Self f15() { 0 }\n"
    "extern \"C\" { nothing(); pub also(); }\n"
    "const Missing BAD = 1; const Self WORSE = 2; pub const str TEXT = \"t\";\n"
    "main() {}"}, {"shapes.snx", shapes}});
  project(inputs, root, "sig-modules", {{"main.snx",
    "import pack.leaf; import pack.leaf.Item; import alias.Thing; import std.prelude;\n"
    "int f(pack.leaf.Item a, leaf.Item b, Item c, Thing d, alias.Thing e, std.prelude.Option<int> o, pack.Secret s, leaf.Private p) { 0 }\n"
    "main() {}"},
    {"pack/mod.snx", "pub struct Secret {}"},
    {"pack/leaf.snx", "pub struct Item {} struct Private {} int h(Private p, Item i) { 0 } pub trait Use<T> { T used(&const self); }"},
    {"alias.snx", "import pack.leaf.{Item, Use}; pub import pack.leaf.Item; pub struct Thing { Item item; }\n"
                  "pub Item g(Item i, &const dyn Use<Item> u) { i } impl Use<Item> for Thing { Item used(&const self) { panic(\"x\") } }"}});
  project(inputs, root, "sig-generics", {{"main.snx",
    "trait Into2<T> { T into2(self); }\n"
    "struct W<T> { T v; U get<U: Into2<T>>(&const self, U u) { u } Self make(T v) { W { v: v } } T shadow<T>(T t) { t } }\n"
    "enum M<T> { Some2(T), None2, M<T> map<U: Into2<T>>(self, fn(T) -> U f) { self } }\n"
    "impl<T> Into2<W<T>> for W<T> { W<T> into2(self) { self } }\n"
    "impl<A: Eq + Ord> Eq for W<A> { bool eq(&const self, &const Self other) { true } }\n"
    "trait Bounded<T: Eq + Into2<Self>> { Self made(T t); }\n"
    "main() {}"}});
  // Replace each complete token in a signature-heavy entry. Siblings stay valid.
  const std::string original = "import a.{A, E}; struct S<T: Tr> { T v; A a; E? e; int[2] n; }\n"
                               "trait Tr { int f(&const self); } impl Tr for S<int> { int f(&const self) { 0 } }\n"
                               "int g<U>(&const S<U> s, fn(U) -> int k) { 0 } main() {}";
  synx::SourceFile source("signature.snx", original); synx::DiagnosticSink sink;
  auto tokens = synx::Lexer(source, sink).tokenize();
  size_t number = 0;
  for (auto& token : tokens) {
    if (token.span.start == token.span.end) continue;
    for (const std::string replacement : {"", "Missing", "Self", "str", "T", "int[]"}) {
      auto text = original.substr(0, token.span.start) + replacement + original.substr(token.span.end);
      const std::string label = "signature-mutation-" + std::to_string(number++);
      project(inputs, root, label.c_str(), {{"main.snx", text}, {"a.snx", "pub struct A {} pub enum E { X, Y(int) }"}});
    }
  }
}
// Stage 4c fixtures. Signatures resolve cleanly, so every function body is
// checked. Each file groups one checker area; one parse slip blocks one file.
static void check_corpus(std::vector<fs::path>& inputs, const fs::path& root) {
  project(inputs, root, "check-statements", {{"main.snx",
    "struct P { int x; pub int y; }\n"
    "int s1() { return true; }\n"
    "int s2() { }\n"
    "s3() { var v = print(\"x\"); }\n"
    "s4() { int x = 1; x = \"s\"; x += true; bool b = true; b += 1; b <<= 1; double d = 1.0; d %= 2.0; d &= 1.0; }\n"
    "s5() { const c = 1; c = 2; 5 = 3; P p = P { x: 1, y: 2 }; p.x = 3; const q = P { x: 1, y: 2 }; q.y = 4; }\n"
    "s6() { break; continue; loop { break; } }\n"
    "s7() { if (1) {} while (\"s\") {} }\n"
    "s8() { if (true) { 1 } }\n"
    "s9() { var x = if (true) { 1 } else { \"s\" }; var y = if (true) { return; } else { 2 }; }\n"
    "s10() { 1 + 2 }\n"
    "s11() { for (i in 1.5..2.5) {} for (x in 5) {} for (x in [1, 2]) { x; } var a = [1, 2]; for (r in &a) {} for (w in &a) { *w; } }\n"
    "int s12(bool b) { if (b) { return 1; } }\n"
    "s13() { var a = [1, 2]; a[true]; var n = 3; n[0]; a[0..1]; }\n"
    "s14() { long l = 1; int i = l; ulong u = 5; var neg = -1; usize z = neg; }\n"
    "s15() { var never = return; }\n"
    "main() {}\n"}});
  project(inputs, root, "check-expressions", {{"main.snx",
    "struct P { int x; pub int y; f(&const self) {} } enum E { A, B(int), C { int z; } } trait T { int g(&const self); }\n"
    "e1() { 1 + true; \"a\" - \"b\"; 1 & 1.5; 2 << 1.0; !\"s\"; ~1.5; -true; uint u = 1; -u; true < false; 'a' < 'b'; 1.0 % 2.0; }\n"
    "e2() { var x = 1; *x; var p = 0 as *const int; *p; unsafe { *p; } }\n"
    "e3() { 3 as bool; 300 as char; (65 as byte) as char; \"x\" as int; var q = 7; &q as *const int; &const q as *int; 1.5 as int; true as int; }\n"
    "e4() { P { x: 1 }; P { x: 1, x: 2, y: 3 }; P { q: 1, x: 1, y: 2 }; Q { a: 1 }; int x = 1; P { x, y: 2 }; P { x: 1, y }; E.C { z: 1 }; E.B { z: 1 }; }\n"
    "e5() { undefined_name; P; T; E; Self; }\n"
    "e6() { var t = (1, 2); t.5; t.1; var p = P { x: 1, y: 2 }; p.nope; p.f; var e = E.A; e.z; var r = &p; r.x; }\n"
    "e7() { var x = 1; x(); s(1, 2); E.B(1, 2); E.Q; P.nope(); E.B(\"s\"); var f = E.B; f(3); }\n"
    "e8() { var s = String.new(); s[0]; var o = Some(P { x: 1, y: 2 }); o?.x; o?.nope; var n = 1; n?.x; n ?? 2; o ?? 3; var k = Some(4); k ?? 5; }\n"
    "Result<int, String> e9() { var r = Ok(1)?; var o = Some(2)?; Ok(r) }\n"
    "int e10() { var r = Ok(1)?; 0 }\n"
    "Option<int> e11() { var x = Some(1)?; var y = Err(2)?; Some(x) }\n"
    "e12() { var a = [1, \"s\"]; var b = [0; 3]; var n = 2; var c = [0; n]; var r = 1..; var t = (1, true, \"s\"); var u = (); }\n"
    "e13() { return 1; }\n"
    "int e14() { return; }\n"
    "e15() { int x = 99999999999; byte b = 256; short s = -32769; int m = -2147483648; long l = 9223372036854775807; uint u = 4294967296; byte k = 300 as byte; int z = -0; }\n"
    "e16() { 1 == \"a\"; P { x: 1, y: 1 } == P { x: 1, y: 1 }; \"a\" == \"b\"; var s = String.new(); s == s; 1.5 == 2; }\n"
    "e17() { var p = P { x: 1, y: 2 }; p.f(1); P.f(); var v = Vec.new(); v.push(1); v.nope(); p.x(); }\n"
    "e18() { var w = &mut_target(); } int mut_target() { 1 } e19() { const c = 1; var r = &c; var q = &5; var ok = &const 5; }\n"
    "main() {}\n"}});
  project(inputs, root, "check-patterns", {{"main.snx",
    "enum E { A, B(int), C { int z; }, D(int, bool) } struct P { int x; int y; }\n"
    "p1(E e) { match e { A => {}, B(x) => {} } }\n"
    "p2(E e) { match e { E.B(1) => {}, _ => {} } match e { E.A(x) => {}, E.B => {}, E.C(1) => {}, E.Q => {}, E.B(1, 2) => {}, _ => {} } }\n"
    "p3(bool b) { match b { true => {} } match b { false => {}, true => {} } match b { true if b => {}, false => {} } }\n"
    "p4((int, bool) t) { match t { (1, true) => {} } match t { (_, x) => {} } match t { (a, b, c) => {} } match t { (x, true) => {}, (y, false) => {} } }\n"
    "p5(P p) { match p { P { x, y: 1 } => {}, P { q } => {} } match p { P { x, y } => {} } match p { E.C { z } => {} } }\n"
    "p6(Option<int> o) { match o { Some(1) | None => {}, Some(x) | Some(x) => {} } match o { None => {}, Some(_) => {} } }\n"
    "p7(&const E e) { match e { E.A => {}, E.B(n) => { var m = n; }, E.C { z } => {}, E.D(a, b) => {} } }\n"
    "p8(int n) { match n { 1..5 => {}, 7 => {} } match n { \"s\" => {} } match n { k => {} } }\n"
    "p9(E e) { match e { E.D(1, true) => {}, E.D(_, false) => {}, _ => {} } match e { E.C { z: 1 } => {}, E.A => {}, E.B(_) => {}, E.D(_, _) => {} } }\n"
    "p10(&const str s) { match s { \"a\" => {}, _ => {} } }\n"
    "p11(Option<E> o) { match o { Some(E.A) => {}, Some(E.B(_)) => {}, None => {} } match o { Some(E.A) => {}, None => {}, Some(_) => {} } }\n"
    "p12(E e) { match e {} var v = match e { E.A => 1, _ => \"s\" }; var w = match e { E.A => return, _ => 2 }; }\n"
    "p13(&E e) { match e { E.B(n) => { *n = 1; }, _ => {} } for ((a, b) in [(1, 2)]) { a + b; } }\n"
    "main() {}\n"}});
  project(inputs, root, "check-traits", {{"main.snx",
    "trait Shape { double area(&const self); double twice(&const self) { self.area() * 2.0 } }\n"
    "trait Gen { Self make(); } trait Mark {}\n"
    "struct Sq { double s; } impl Shape for Sq { double area(&const self) { self.s * self.s } }\n"
    "struct V { int x; } impl Add for V { V add(self, V o) { V { x: self.x + o.x } } } impl Eq for V { bool eq(&const self, &const V o) { self.x == o.x } }\n"
    "struct W { int x; consume(self) {} change(&self) { self.x = 1; } make() {} }\n"
    "int area_of<T: Shape>(&const T t) { t.area() as int }\n"
    "t1() { var q = Sq { s: 1.0 }; q.twice(); area_of(&q); area_of(&1); var v = V { x: 1 } + V { x: 2 }; var w = W { x: 1 } - W { x: 2 }; v == v; v < v; -v; }\n"
    "t2() { var q = Sq { s: 2.0 }; &const dyn Shape d = &q; d.area(); d.twice(); &const dyn Gen g = &q; int n = 1; &const dyn Shape bad = &n; &const dyn Shape auto = q; }\n"
    "t3(&const W w) { w.change(); w.consume(); W { x: 1 }.change(); const k = W { x: 2 }; k.change(); var m = W { x: 3 }; m.make(); W.make(); W.change(); }\n"
    "t4() { var s = \"abc\"; s.len(); s.parse_int(); s.nope(); var st = String.from(\"x\"); takes_str(st); takes_str(&st); takes_slice(&[1, 2]); var v = Vec.new(); v.push(1); takes_slice(&v); takes_slice(v); }\n"
    "takes_str(&const str s) {} takes_slice(&const int[] s) {}\n"
    "t5() { print(\"{Sq { s: 1.0 }}\"); print(\"{1} {true} {\"x\"} {1.5}\"); var r = &1; print(\"{r}\"); }\n"
    "struct Pair<A: Eq, B> { A a; B b; } t6() { var p = Pair { a: Sq { s: 1.0 }, b: 1 }; var ok = Pair { a: 1, b: Sq { s: 1.0 } }; }\n"
    "impl Copy for W {} unsafe impl Send for V {} impl Sync for V {} unsafe impl Mark for Sq {}\n"
    "t7<T>(T x) { x.area(); var y = x; } t8<T: Copy>(T x) { var y = x; var z = x; } t9<T: Shape>(T x) { x.twice(); T.make(); }\n"
    "struct D { int x; } impl Drop for D { void drop(&self) {} } t10() { var d = D { x: 1 }; d.drop(); }\n"
    "struct Wk {} impl Wake for Wk { void wake(&self) {} }\n"
    "t11() { size_of<int>(); size_of(); var p = 0 as *int; drop_in_place(p); unsafe { drop_in_place(p); } }\n"
    "t11b() { var s = str_from_raw(0 as *const byte, 0); } int t11c() { loop { while (true) { break; } return 1; } } t11d() { panic(\"x\"); }\n"
    "int t11e() { loop { break; } } struct NC { String s; } impl Copy for NC {} struct GC<T> { T v; } impl<T: Copy> Copy for GC<T> {} impl<T> Copy for Pair<T, T> {}\n"
    "extern \"C\" { int c_call(int x); } t12() { c_call(1); unsafe { c_call(2); } }\n"
    "struct Gn<T> { T v; T get(&const self) { self.v } } t13() { var g = Gn { v: 1 }; Gn<int, int>.get; var b = Gn { v: true }; b.get(); Gn<bool>.get(); }\n"
    "main() {}\n"}});
  project(inputs, root, "check-closures", {{"main.snx",
    "struct T { int id; }\n"
    "c1() { var n = 1; var f = |int a| a + n; f(2); const g = || n; g(); var h = once || n; h(); var s = const || { n += 1; }; var bad = |a| a; }\n"
    "c2() { var n = 1; var f = [&n] || { n += 1; }; var g = [&const n] || { n += 1; }; var k = [n, n] || n; var u = [missing] || 1; var z = [] || n; const ro = 2; var w = [&ro] || 1; }\n"
    "c3() { var c = || 1; &const fn() -> int r = &c; r(); fn once() -> int o = once || 1; &fn once() -> int ro = &o; ro(); const w = || 2; w(); }\n"
    "c4() { fn send() -> int s = || 1; var t = T { id: 1 }; fn send() -> int s2 = || t.id; var r = &t; fn send() -> int s3 = || r.id; }\n"
    "c5(fn const(int) -> int f) { f(1); } c6() { c5(|int x| x); fn(int) -> int w = |int x| x; c5(w); c5(c6); var n = 0; c5(|int x| { n += x; x }); }\n"
    "struct Holder { fn() -> int call; fn once() -> int once_call; } c7(&const Holder h, Holder own) { h.call(); h.once_call(); own.once_call(); }\n"
    "c8() { var x = 1; var f = || x + 1; var g = |int y| -> long { y }; var h = || -> int { return 1; }; }\n"
    "main() {}\n"}});
  project(inputs, root, "check-async", {{"main.snx",
    "struct Tk { int id; } trait Run { async int go(&const self); }\n"
    "async int a1() { 1 } a2() { await a1(); } async int a3() { var t = a1(); await t; await 5; var r = &const t; await r; var w = &t; await w; 1 }\n"
    "async send int a4(Tk k) { k.id } async send int a5(&const Tk k) { 1 } async void a6() { var x = a1(); }\n"
    "extern \"C\" { async int nope(); }\n"
    "async main() {}\n"}});
  project(inputs, root, "check-inference", {{"main.snx",
    "f() { g() } g() { f() } int h() { var v = Vec.new(); 0 } k() { var x = Vec.new(); x.push(1); var y = Vec.new(); y.push(true); }\n"
    "const A = B; const B = A; const C = 1 + 2; const int D = \"s\"; const E = return; const F = Some(1)?;\n"
    "self_return() { self_return() } int typed() { typed() } m() { var o = None; var e = Err(1); var n = Some(Vec.new()); }\n"
    "lit() { var a = 1; var b = 2.5; var c = a + 1; long l = c; var d = b * 2.0; float fl = d; }\n"
    "main() {}\n"}});
  project(inputs, root, "check-layout", {{"main.snx",
    "struct R { R next; } struct S { int x; } main() {}\n"}});
  project(inputs, root, "check-modules", {{"main.snx",
    "import lib; import lib.{Item, make};\n"
    "c1() { var i = lib.Item { v: 1 }; i.v; i.secret; lib.make(); lib.hidden(); lib.Item.new(); lib.nope; lib; lib.Kind.A; lib.Kind.B(1); }\n"
    "c2() { var i = make(); i.bump(); i.peek(); Item.new(); Item { v: 1, secret: 2 }; lib.LIMIT; lib.PRIVATE; lib.Hidden {}; }\n"
    "main() {}\n"},
    {"lib.snx", "pub struct Item { pub int v; int secret; pub Item new() { Item { v: 0, secret: 0 } } pub void bump(&self) { self.v += 1; } peek(&const self) {} }\n"
                "pub Item make() { Item.new() } hidden() {} struct Hidden {} pub enum Kind { A, B(int) } pub const int LIMIT = 3; const int PRIVATE = 4;\n"}});
  // Replace each complete token in a body-heavy entry. The entry stays one file.
  const std::string original =
    "struct P { int x; int y; sum(&const self) -> int { self.x + self.y } } enum E { A, B(int) }\n"
    "int f(&P p, Option<int> o) { var t = 0; for (i in 0..3) { t += i; } match o { Some(v) => t += v, None => {} }\n"
    "  if (p.x > 1) { p.y = t; } var c = |int a| a * 2; var e = E.B(c(t)); match e { E.A => 0, E.B(n) => n + p.sum() } }\n"
    "main() { var p = P { x: 1, y: 2 }; f(&p, Some(3)); }";
  synx::SourceFile source("check.snx", original); synx::DiagnosticSink sink;
  auto tokens = synx::Lexer(source, sink).tokenize();
  size_t number = 0;
  for (auto& token : tokens) {
    if (token.span.start == token.span.end) continue;
    for (const std::string replacement : {"", "x", "1", "&", "true", "Self"}) {
      auto text = original.substr(0, token.span.start) + replacement + original.substr(token.span.end);
      const std::string label = "check-mutation-" + std::to_string(number++);
      project(inputs, root, label.c_str(), {{"main.snx", text}});
    }
  }
}
// Stage 5a fixtures. Bodies check cleanly, so lowering runs; the error file
// reaches every SNX0601 path the checker admits, in worklist order.
static void lower_corpus(std::vector<fs::path>& inputs, const fs::path& root) {
  project(inputs, root, "lower-errors", {{"main.snx",
    "struct V { int x; } impl Ord for V { bool lt(&const self, &const V o) { self.x < o.x } } impl Eq for V { bool eq(&const self, &const V o) { self.x == o.x } }\n"
    "struct Holder { String s; }\n"
    "enum E { A, B(int) }\n"
    "void l1() { var a = V { x: 1 }; var b = V { x: 2 }; bool g = a > b; bool le = a <= b; bool ok = a < b; bool ne = a != b; bool ge = a >= b; }\n"
    "void l2() { var v = Vec.new(); v.push(1); for (x in v) { } }\n"
    "void l3() { String[2] a = [String.new(), String.new()]; for (s in a) { } }\n"
    "void l4() { var f = print; var g = E.B; }\n"
    "void l5(Option<Holder> h) { var s = h?.s; }\n"
    "main() { }\n"}});
  project(inputs, root, "lower-main", {{"main.snx", "int main(int argc) { 0 }\n"}});
  project(inputs, root, "lower-main-type", {{"main.snx", "String main() { String.new() }\n"}});
  project(inputs, root, "lower-features", {{"main.snx",
    "trait Shape { double area(&const self); double twice(&const self) { self.area() * 2.0 } }\n"
    "struct Sq { double s; } impl Shape for Sq { double area(&const self) { self.s * self.s } }\n"
    "struct Pt { int x; int y; } impl Add for Pt { Pt add(self, Pt o) { Pt { x: self.x + o.x, y: self.y + o.y } } }\n"
    "impl Eq for Pt { bool eq(&const self, &const Pt o) { self.x == o.x && self.y == o.y } }\n"
    "impl Display for Pt { String to_string(&const self) { \"({self.x}, {self.y})\" } }\n"
    "struct Noisy { int id; } impl Drop for Noisy { void drop(&self) { print(\"drop {self.id}\"); } }\n"
    "enum Tree { Leaf, Node(Box<Tree>, int, Box<Tree>) }\n"
    "struct Pair<A, B> { A a; B b; &const A first(&const self) { &const self.a } }\n"
    "const int LIMIT = 3 + 4;\n"
    "int sum(&const int[] xs) { int t = 0; for (x in xs) { t += *x; } t }\n"
    "int depth(&const Tree t) { match t { Tree.Leaf => 0, Tree.Node(l, _, r) => { int a = depth(l.as_ref()); int b = depth(r.as_ref()); 1 + if (a > b) { a } else { b } } } }\n"
    "Result<int, String> parse(&const str s) { if (s == \"x\") { return Err(String.from(\"bad\")); } Ok(1) }\n"
    "Result<int, String> twice_parse(&const str s) { var a = parse(s)?; var b = parse(s)?; Ok(a + b) }\n"
    "int area_of<T: Shape>(&const T t) { t.twice() as int }\n"
    "async int later(int x) { x + 1 }\n"
    "async int chain() { var a = await later(1); var b = await later(a); a + b }\n"
    "void closures() {\n"
    "  int n = 1; var add = |int k| k + n; var shared = const || n * 2; var consume = once || Noisy { id: 9 };\n"
    "  int total = 0; { var bump = [&total] |int k| { total += k; }; bump(2); bump(3); }\n"
    "  var peek = [&const total] || total; var moved = [n] || n + 1;\n"
    "  var nested = |int a| { var inner = |int b| a + b; inner(1) }; var keep = consume(); print(\"{add(1)} {shared()} {peek()} {moved()} {nested(2)} {keep.id}\");\n"
    "}\n"
    "void shapes() { var q = Sq { s: 2.0 }; &const dyn Shape d = &q; var a = d.area(); var b = area_of(&q); print(\"{a} {b}\"); }\n"
    "void loops() {\n"
    "  int t = 0; for (i in 0..LIMIT) { if (i == 1) { continue; } t += i; } for (i in 1..=3) { t += i; }\n"
    "  var v = Vec.new(); v.push(1); v.push(2); for (x in &v) { t += *x; } for (x in &v) { if (*x > 1) { break; } }\n"
    "  int[4] arr = [7; 4]; t += sum(&arr); t += sum(&v); int k = 0; while (k < 3) { k += 1; } loop { if (k > 5) { break; } k += 1; }\n"
    "  print(\"{t} {k}\");\n"
    "}\n"
    "void values() {\n"
    "  var p = Pt { x: 1, y: 2 } + Pt { x: 3, y: 4 }; bool same = p == p; bool differ = p != Pt { x: 0, y: 0 };\n"
    "  print(\"{p} {same} {differ}\"); var pair = Pair { a: String.from(\"a\"), b: 2 }; var f = pair.first();\n"
    "  var t = Tree.Node(Box.new(Tree.Leaf), 1, Box.new(Tree.Node(Box.new(Tree.Leaf), 2, Box.new(Tree.Leaf))));\n"
    "  int d = depth(&t); Option<int> o = Some(4); int got = o ?? 0; var tup = (1, true, 'c');\n"
    "  match tup { (1, true, c) => print(\"{c}\"), (_, _, _) => {} } match o { Some(4) | Some(5) => {}, Some(n) if n > 9 => {}, _ => {} }\n"
    "  var r = twice_parse(\"y\"); var s = String.from(\"abc\"); bool less = s.as_str() < \"abd\"; var n = Noisy { id: 1 }; n = Noisy { id: 2 };\n"
    "  long big = -9223372036854775808; byte b = 255 as byte; char ch = b as char; usize u = ~(0 as usize); print(\"{d} {got} {less} {big} {ch} {u}\");\n"
    "}\n"
    "main() { closures(); shapes(); loops(); values(); }\n"}});
}
// Stage after declarations: "" (declarations only), "signatures", or "check".
static std::string stage;
static bool signatures = false;
static bool run_stage(synx::Compilation& c, const std::string& which) {
  if (which == "check") return c.run_front_end();
  if (which == "lower") return c.run_front_end() && c.lower_ir(false);
  if (which == "borrow") return c.run_front_end() && c.lower_ir(false) && c.check_ownership();
  if (which == "async") return c.run_front_end() && c.lower_ir(false) && c.check_ownership() && c.convert_async();
  if (which == "signatures") return c.run_signatures();
  return c.run_declarations();
}
static std::string oracle(const synx::Compilation& c, const std::string& which) {
  auto text = synx::dump_declarations(c);
  if (which == "check") text += synx::dump_check(c);
  if (which == "lower" || which == "borrow" || which == "async") text += synx::dump_ir_strict(c);
  return text;
}
// "all": every pass in order on one compilation, stopping at the first that
// fails. The record holds the final declaration dump with every diagnostic,
// the checker tables, the IR after lowering, after ownership checks, and
// after async frame conversion, and the layouts and register allocation when
// every pass succeeds.
static unsigned probe_mode = 0;
static std::string staged_oracle(synx::Compilation& c) {
  bool ok = c.run_declarations();
  if (ok) { c.resolver->resolve_signatures(); ok = !c.sink.has_errors(); }
  if (ok) { c.checker->check_program(); ok = !c.sink.has_errors(); }
  if (ok) ok = c.lower_ir(false);
  auto lowered = synx::dump_ir_strict(c);
  if (ok) ok = c.check_ownership();
  auto borrowed = synx::dump_ir_strict(c);
  if (ok) ok = c.convert_async();
  auto converted = synx::dump_ir_strict(c);
  // Axon runs after the machine dump; its errors join the declaration dump.
  // Build mode bit 0 disables register allocation, bit 1 checked arithmetic.
  std::string machine = "machine skipped\n";
  if (ok) machine = synx::dump_machine(c) + synx::dump_objects(c, !(probe_mode & 1), !(probe_mode & 2));
  auto declarations = synx::dump_declarations(c);
  return declarations + synx::dump_check(c) + lowered + borrowed + converted + machine;
}
static void compare(const fs::path& source, const fs::path& root, const fs::path& exe,
                    const std::vector<fs::path>& inputs, const fs::path& standard, bool unset = false) {
  EnvScope environment(utf8(standard), unset); PhaseTimer timer;
  const auto manifest = root / "manifest.txt", report = root / "actual.bin", log = root / "run.log";
  std::string list, expected; std::vector<size_t> ends;
  for (const auto& path : inputs) list += utf8(path) + '\n';
  write(manifest, list);
  Environment overrides;
  if (!unset) overrides.push_back({"SYNX_STD_PATH", utf8(standard)});
  std::vector<std::string> args = {utf8(source / "compiler/prelude.snx"), utf8(source / "std"), utf8(manifest), utf8(report)};
  if (signatures) args.push_back(stage);
  // The "all" probe generates objects with the build mode's target flags.
  if (stage == "all") args.push_back(std::to_string(probe_mode));
  // The probe needs only the manifest, so it runs while this process computes
  // the C++ oracle. The thread is joined before any exception leaves.
  int rc = -1;
  std::thread probe([&] { rc = run(exe, args, log, overrides); });
  try {
    std::ofstream index(root / "records.tsv", std::ios::binary);
    for (size_t i = 0; i < inputs.size(); ++i) {
      const auto& path = inputs[i];
      auto file = synx::SourceFile::load_utf8(utf8(path));
      if (!file) throw std::runtime_error("cannot read entry");
      synx::Compilation c(synx::SourceFile("<prelude>", synx::sema::prelude_source()), std::move(*file));
      std::string dump;
      if (stage == "all") dump = staged_oracle(c);
      else { run_stage(c, stage); dump = oracle(c, stage); }
      const auto begin = expected.size(); blob(expected, dump); blob(expected, c.sink.render(c.user_file)); ends.push_back(expected.size());
      index << i << '\t' << begin << '\t' << expected.size() << '\t' << utf8(path) << '\n';
    }
  } catch (...) { probe.join(); throw; }
  write(root / "expected.bin", expected); timer.mark("oracle");
  probe.join(); timer.mark("probe wait");
  CHECK(rc == 0); if (rc) throw std::runtime_error(read(log));
  auto actual = read(report); CHECK(actual == expected);
  if (actual != expected) {
    size_t offset = 0;
    while (offset < actual.size() && offset < expected.size() && actual[offset] == expected[offset]) ++offset;
    auto record = static_cast<size_t>(std::upper_bound(ends.begin(), ends.end(), offset) - ends.begin());
    throw std::runtime_error("module mismatch at byte " + std::to_string(offset) + ", record " + std::to_string(record) +
                             (record < inputs.size() ? ": " + utf8(inputs[record]) : " after final record"));
  }
  std::printf("%zu entry files, %zu bytes; std override %s\n", inputs.size(), actual.size(), utf8(standard).c_str());
}
int main(int argc, char** argv) {
  // Optional fifth argument: "signatures" compares after resolve_signatures;
  // "check" also checks bodies and compares every checker table.
  // "borrow" takes the test_borrow executable as a sixth argument; its cases
  // are exported through SYNX_CASE_DIR and compared too.
  if (argc >= 5) stage = argv[4];
  // "all" also takes test_borrow and test_async_pool and compares every stage
  // in one probe run.
  bool known = stage == "signatures" || stage == "check" || stage == "lower";
  if (argc != 4 && !(argc == 5 && known) && !(argc == 6 && stage == "borrow") && !(argc == 7 && stage == "all")) return 1;
  signatures = argc >= 5;
  const auto mode = static_cast<unsigned>(std::stoi(argv[3]));
  probe_mode = mode;
  const auto root = fs::temp_directory_path() / (std::string(signatures ? "synx-" + stage + "-" : "synx-modules-") + std::to_string(mode) + '-' +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    PhaseTimer timer;
    const auto source = fs::absolute(argv[2]), compiler = fs::absolute(argv[1]);
    fs::create_directories(root);
    std::vector<fs::path> inputs;
    for (const char* folder : {"tests/run", "examples"}) for (auto& entry : fs::recursive_directory_iterator(source / folder))
      if (entry.is_regular_file() && entry.path().extension() == ".snx") inputs.push_back(entry.path());
    std::sort(inputs.begin(), inputs.end(), [](const fs::path& a, const fs::path& b) { return utf8(a) < utf8(b); });
    inputs.push_back(fs::relative(source / "tests/run/modules/reexport_bad_a.snx", fs::current_path()));
    inputs.push_back(source / "tests/run/modules/../modules/reexport_bad_a.snx");
#ifdef _WIN32
    const auto relative = fs::relative(source / "tests/run/modules/reexport_bad_a.snx", fs::current_path());
    inputs.push_back(fs::path(fs::current_path().root_name().wstring() + relative.wstring()));
    const auto full = source / "tests/run/modules/reexport_bad_a.snx";
    inputs.push_back(full.root_directory() / full.relative_path());
#endif
    corpus(inputs, root); signature_corpus(inputs, root);
    if (stage == "check" || stage == "lower" || stage == "borrow" || stage == "all") {
      check_corpus(inputs, root);
      if (stage != "check") lower_corpus(inputs, root);
      // Test executables export their case sources through SYNX_CASE_DIR.
      auto exported = [&](const char* exe, const std::string& name) {
        auto cases = root / name;
        int rc = run(fs::absolute(exe), {}, root / (name + ".log"), {{"SYNX_CASE_DIR", utf8(cases)}});
        CHECK(rc == 0); if (rc) throw std::runtime_error(read(root / (name + ".log")));
        std::vector<std::pair<int, fs::path>> found;
        for (auto& entry : fs::directory_iterator(cases))
          found.push_back({std::stoi(entry.path().filename().string().substr(5)), entry.path() / "main.snx"});
        std::sort(found.begin(), found.end());
        for (auto& [number, path] : found) { (void)number; inputs.push_back(path); }
      };
      if (stage == "borrow" || stage == "all") exported(argv[5], "borrow-cases");
      if (stage == "all") exported(argv[6], "async-cases");
      for (auto& entry : fs::directory_iterator(source / "std")) if (entry.path().extension() == ".snx") inputs.push_back(entry.path());
    }
    timer.mark("corpus");
    // Mode 0 compares every token mutation. The other build modes change the
    // probe binary, not the compared passes; they keep every program and fixture
    // and every fourth mutation to bound the run time.
    if (mode != 0) {
      std::vector<fs::path> kept;
      size_t mutation = 0;
      for (auto& input : inputs) {
        bool mutated = utf8(input).find("mutation-") != std::string::npos;
        if (!mutated || mutation++ % 4 == 0) kept.push_back(input);
      }
      inputs = std::move(kept);
    }
    const auto exe = root / "modules-probe.exe", log = root / "build.log";
    std::vector<std::string> args = {"build", utf8(source / "selfhost/modules_probe.snx"), "-o", utf8(exe)};
    if (mode & 1) args.push_back("--no-regalloc");
    if (mode & 2) args.push_back("--release");
    int rc = run(compiler, args, log, {{"SYNX_STD_PATH", utf8(source / "std")}}); CHECK(rc == 0);
    if (rc) throw std::runtime_error(read(log));
    timer.mark("probe build");
    // Exercise the driver argument boundary as well as the library oracle.
    auto cli = root / fs::path(u8"cli caf\u00e9 \u03bb") / "main.snx";
    write(cli, "import leaf.Value; main() {}"); write(cli.parent_path() / "leaf.snx", "pub struct Value {}");
    auto file = synx::SourceFile::load_utf8(utf8(cli));
    if (!file) throw std::runtime_error("cannot read CLI fixture");
    synx::Compilation c(synx::SourceFile("<prelude>", synx::sema::prelude_source()), std::move(*file));
    // The driver prints one stage; "all" checks the last one.
    const std::string cli_stage = stage == "all" ? "async" : stage;
    CHECK(run_stage(c, cli_stage));
    std::vector<std::string> cli_args = {"modules", utf8(cli)};
    if (signatures) cli_args.push_back("--" + cli_stage);
    rc = run(compiler, cli_args, root / "cli.log"); CHECK(rc == 0);
    auto raw = read(root / "cli.log"); std::string normalized;
    for (size_t i = 0; i < raw.size(); ++i) {
      if (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n') continue;
      normalized += raw[i];
    }
    CHECK(normalized == oracle(c, cli_stage));
    if (rc || normalized != oracle(c, cli_stage)) throw std::runtime_error("Unicode modules CLI mismatch");
    // Native build paths are driver behavior; one mode covers them.
    if (!signatures || stage == "all") {
    auto unicode_input = cli.parent_path() / fs::path(u8"entry caf\u00e9 \u03bb.snx");
    write(unicode_input, "main() { 0 }");
    const auto unicode_output = cli.parent_path() / fs::path(u8"output caf\u00e9 \u03bb.exe");
    rc = run(compiler, {"run", utf8(unicode_input), "-o", utf8(unicode_output)}, root / "unicode-run.log");
    CHECK(rc == 0); CHECK(fs::is_regular_file(unicode_output));
    if (rc) throw std::runtime_error(read(root / "unicode-run.log"));
    rc = run(compiler, {"build", utf8(unicode_input)}, root / "unicode-build.log"); CHECK(rc == 0);
    fs::path default_output = unicode_input.parent_path() / unicode_input.stem();
#ifdef _WIN32
    default_output += ".exe";
#endif
    CHECK(fs::is_regular_file(default_output));
    if (rc) throw std::runtime_error(read(root / "unicode-build.log"));
    }
    timer.mark("driver checks");
    fs::create_directories(root / "repository"); compare(source, root / "repository", exe, inputs, source / "std");
    std::vector<fs::path> overridden;
    auto standard = root / fs::path(u8"standard caf\u00e9 \u03bb");
    write(standard / "leaf.snx", "pub import std.peer.Hidden; pub struct Value {} pub enum E { A, B(int) }");
    write(standard / "peer.snx", "struct Hidden {}");
    project(overridden, root, "override", {{"main.snx", "import std.leaf.{Value, E}; import std.leaf.E.A; import std.prelude.Vec;"}});
    fs::create_directories(root / "overridden"); compare(source, root / "overridden", exe, overridden, standard);
#ifdef _WIN32
    auto backslashes = standard; backslashes.make_preferred();
    fs::create_directories(root / "backslashes"); compare(source, root / "backslashes", exe, overridden, backslashes);
#endif
    std::vector<fs::path> missing;
    project(missing, root, "no-standard", {{"main.snx", "import std.leaf; import std.prelude.Vec;"}});
    fs::create_directories(root / "missing"); compare(source, root / "missing", exe, missing, root / "missing-std");
    std::vector<fs::path> fallback;
    project(fallback, root, "std-fallback", {{"main.snx", "import std.io.print; import std.prelude.Vec;"}});
    fs::create_directories(root / "fallback"); compare(source, root / "fallback", exe, fallback, {}, true);
    fs::create_directories(root / "empty"); compare(source, root / "empty", exe, fallback, {});
    fs::remove_all(root); return synx_test::finish(signatures ? ("selfhost_" + stage).c_str() : "selfhost_modules");
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s\nartifacts: %s\n", e.what(), utf8(root).c_str()); return 1;
  }
}
