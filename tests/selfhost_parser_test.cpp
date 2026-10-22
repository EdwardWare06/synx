// Differential parser checks. One compiler build mode per CTest entry.
#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>
#include <utility>

#include "selfhost_support.hpp"
#include "synx/ast.hpp"
#include "synx/lexer.hpp"
#include "synx/parser.hpp"
#include "test_support.hpp"

namespace fs = std::filesystem;
using namespace selfhost_test;

struct Snippet {
  char mode;
  std::string label;
  std::string text;
};

struct Input {
  char mode;
  std::string label;
  fs::path path;
};

static void add(std::vector<Snippet>& out, char mode, const std::string& label, std::string text) {
  out.push_back({mode, label, std::move(text)});
}

static void fixed(std::vector<Snippet>& out, char mode, const char* group, std::initializer_list<const char*> texts) {
  size_t index = 0;
  for (const char* text : texts) add(out, mode, std::string(group) + "-" + std::to_string(index++), text);
}

static void precedence(std::vector<Snippet>& out) {
  const std::vector<std::string> binary = {"||", "&&", "==", "!=", "<", "<=", ">", ">=",
                                           "|", "^", "&", "<<", ">>", "+", "-", "*", "/", "%"};
  const std::vector<std::string> unary = {"-", "!", "~", "*", "&", "&const ", "await "};
  for (const auto& left : binary) {
    for (const auto& right : binary) {
      const std::string a = "a " + left + " b";
      const std::string b = "b " + right + " c";
      add(out, 'E', "binary-unparenthesized", a + " " + right + " c");
      add(out, 'E', "binary-left-parenthesized", "(" + a + ") " + right + " c");
      add(out, 'E', "binary-right-parenthesized", "a " + left + " (" + b + ")");
    }
    add(out, 'E', "cast-left", "a as long " + left + " b");
    add(out, 'E', "cast-right", "a " + left + " b as long");
    add(out, 'E', "cast-chain", "(a " + left + " b) as long as int");
    add(out, 'E', "coalesce-left", "a ?? b " + left + " c");
    add(out, 'E', "coalesce-right", "a " + left + " b ?? c");
    for (const auto& prefix : unary) {
      add(out, 'E', "unary-left", prefix + " a " + left + " b");
      add(out, 'E', "unary-parenthesized-left", "(" + prefix + " a) " + left + " b");
      add(out, 'E', "unary-right", "a " + left + " " + prefix + " b");
      add(out, 'E', "unary-parenthesized-expression", prefix + " (a " + left + " b)");
    }
    for (const std::string range : {"..", "..="}) {
      add(out, 'E', "range-left", "a " + left + " b " + range + " c");
      add(out, 'E', "range-right", "a " + range + " b " + left + " c");
      add(out, 'E', "range-parenthesized", "(a " + range + " b) " + left + " c");
    }
  }
  for (const auto& outer : unary) {
    for (const auto& inner : unary) {
      add(out, 'E', "unary-chain", outer + " " + inner + " a");
      add(out, 'E', "unary-chain-parenthesized", outer + " (" + inner + " a)");
    }
  }
  fixed(out, 'E', "range", {"..", "..a", "a..", "a..b", "..=b", "a..=b", "..=", "a..=", "(a..b)", "a..b..c", "a..=b..=c"});
  fixed(out, 'E', "shift-adjacency", {"a >> b", "a > > b", "a >/*gap*/> b", "a >>> b", "a << b + c", "(a << b) + c"});
}

static void fixed_cases(std::vector<Snippet>& out) {
  fixed(out, 'E', "ambiguity", {"a < b > (c)", "a < b > c", "a < b && c > d", "f<Vec<Vec<int>>>(x)", "Vec<Vec<int>>.new()",
                                "f<(int, double)>(x)", "f<int>.member(x)", "a < (b > c)", "a < b > (c) + d",
                                "(a < b) > c", "f<int>(x)[0].field?", "f(x)?.field ?? fallback", "tuple.0", "x?.0"});
  fixed(out, 'E', "closure", {"|x| x", "|| 1", "|x, y| x + y", "|int x, int y| -> int { x + y }", "once |x| x",
                              "once || 1", "const |x| x", "const || 1", "send once |x| x", "send const || 1",
                              "[&x, y] |z| x + y + z", "[&const x, &y, self] once || x", "[] const || 1",
                              "[x] send once || x", "|| || 1", "|x| { |y| x + y }"});
  fixed(out, 'E', "postfix-prefix", {"await f()", "await await f()", "await f()?", "(await f())?", "await x.field[0]",
                                     "f()?.field", "x ?? await f()", "x ?? y ?? z", "try f()", "?x", "return",
                                     "return 1 + 2", "return { 1 }", "break", "continue", "[1, 2, 3]", "[0; 8]", "()",
                                     "(1)", "(1,)", "(1, 2)", "Point { x: 1, y }", "Point<int> { x: 1 }",
                                     "\"a{x}b{f(2)}c\"", "\"{\"nested {x}\"}\"", "unsafe { *p }"});
  fixed(out, 'E', "heads", {"if (Point { x: 1 }.x) { 1 } else { 2 }", "if (x) { 1 } else if (y) { 2 } else { 3 }",
                            "match x { Some(v) if v > 0 => v, _ => 0 }", "match Point { x: 1 } { _ => 0 }",
                            "match (Point { x: 1 }) { _ => 0 }", "match x { P { x, y: z } => { z } _ => 0 }"});
  fixed(out, 'T', "type", {"int", "Self", "std.io.File", "Vec<Vec<int>>", "Map<String, Vec<Vec<int>>>",
                           "Vec<(int, Vec<int>)>", "()", "(int)", "(int,)", "(int, double)", "((int, double),)",
                           "&const str", "&Point", "*const byte", "*byte", "int[4]", "int[]", "&const int[]",
                           "Vec<int>[1 + 2]", "int?", "&const str?", "(&const str)?", "int[4]?", "&const dyn Shape",
                           "&dyn Shape", "fn()", "fn(int, double) -> int", "fn const(int) -> int", "fn once() -> String",
                           "fn send once(int) -> int", "fn send const() -> int", "fn(int) -> fn() -> int"});
  fixed(out, 'P', "pattern", {"_", "x", "None", "Token.End", "Some(x)", "Err(ParseError.Bad((x, _)))", "()", "(x)", "(x,)",
                              "((x, y), Some(_))", "Point { x, y: height }", "Outer { inner: Some((a, b)), tail }",
                              "0..9", "0..=9", "-9..=-1", "-1", "1 | 2 | 3", "Some(1 | 2)", "(1 | 2, _)", "true", "false",
                              "'x'", "'\\u{10ffff}'", "\"text\"", "\"a\\0b\"", "1.5", "2.5f", "42L", "&x", "&const x", "&&x"});
  fixed(out, 'S', "statement", {"int x = 1;", "int x;", "var x = 1;", "const x = 1;", "const int x = 1;",
                                "Vec<Vec<int>> x = f();", "&const str x = \"x\";",
                                "fn const(int) -> int f = const |x| x;", "while (x) { x -= 1; }", "loop { break; }",
                                "for ((a, b) in items) { continue; }", "for (x in Vec { value: 1 }) { }",
                                "while (Point { x: 1 }.x) { break; }", "if (x) { f(); }", "match x { _ => 1 };",
                                "return 1;", "break;", "continue;", "f();", "a < b;", "*p = 1;", "x"});
  for (const char* op : {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="})
    add(out, 'S', "assignment", std::string("self.value ") + op + " x + 1;");
  fixed(out, 'M', "item", {
    "pub import std.io;",
    "pub import std.collections.{Vec, HashMap};",
    "import std.io.print;",
    "unsafe impl Send for Worker { }",
    "unsafe impl<T: Copy + Owned> Send for Worker<T> { }",
    "pub async send int work(int x) { return await next(x); }",
    "extern \"C\" { int puts(*const byte text); void done(); }",
    "trait Shape { double area(&const self); &const str name(&const self) { \"shape\" } }",
    "enum Shape { End, Circle(double), Rect { double w; double h; } int tag(&const self) { 1 } }",
    "struct Pair<A: Copy, B: Copy + Owned> { pub A first; B second; "
    "Pair<A, B> make(A a, B b) { Pair { first: a, second: b } } }",
    "impl<T: Copy> Eq for Pair<T> { bool eq(&const self, &const Self other) { true } }",
    "const int MAX = 100;",
    "/// Item.\npub struct S {\n/// Field.\npub int value;\n/// Method.\nint get(&const self) { self.value }\n}",
    "/// Enum.\nenum E {\n/// Unit.\nA,\n/// Tuple.\nB(int),\n/// Record.\nC {\n/// Nested field.\nint n;\n}\n"
    "/// Method.\nint get(&const self) { 0 }\n}",
    "/// Trait.\ntrait T {\n/// Signature.\nint f(&const self);\n/// Default.\nint g(&const self) { 1 }\n}",
    "/// Impl.\nimpl T for S {\n/// Method.\nint f(&const self) { 0 }\n}",
    "/// Extern.\nextern \"C\" {\n/// Function.\nint f(int x);\n}",
    "/// Constant.\nconst int K = 1;\n/// Function.\nf() { }",
    "pub /// After visibility.\nf() { }",
    "/// Orphan.\n", "main() { /// Statement.\n f(); }"});

  // Reachable expect/error contexts. Token deletions below cover delimiter
  // failures within the feature-rich module witnesses.
  fixed(out, 'M', "item-error", {
    "import ;", "import std.;", "import std.{,};", "import std.{x;", "import std.io",
    "const int = 1;", "const int x 1;", "const int x = 1",
    "struct { }", "struct S", "struct S { int ; }", "struct S { int x }", "struct S {",
    "enum { }", "enum E", "enum E { , }", "enum E { A(int }", "enum E { A B }",
    "enum E { A { int ; } }", "enum E { A { int x } }", "enum E { A { int x; }", "enum E {",
    "trait { }", "trait T", "trait T { int f(&const self) }", "trait T { int field; }", "trait T {",
    "impl T U { }", "impl T for U", "impl T for U {",
    "unsafe struct S { }", "extern { }", "extern \"C\"", "extern \"C\" { int f() { } }", "extern \"C\" {",
    "int () { }", "f<T:>() { }", "f<>() { }", "f<T,>() { }", "f<T() { }", "f(int) { }",
    "f(int x { }", "f(&self) { }", "struct S { f(int x, &self) { } }",
    "f()", "int f();", "async send",
    "main() { int x = 1 int y = 2; }",
    "a() { x = ; }\nb() { y = ; }",
    "main() { @ { bad } }\ngood() { }",
    "main() { x + ; y = 2; }\ngood() { }",
    "main() { var x; }", "main() { var = 1; }", "main() { int x = 1; ",
    "/// First.\n@;\n/// Second.\ngood() { }"});
  fixed(out, 'T', "type-error", {"", "@", "&", "&const", "*", "*const", "dyn", "Vec<", "Vec<int", "Vec<int,>",
                                 "Vec<,int>", "std.", "int[", "int[1", "int[1 + ]", "(int", "(int, double", "fn",
                                 "fn int", "fn(int", "fn(int,)", "fn(int) ->", "fn send once(", "fn const() -> &"});
  fixed(out, 'E', "expression-error", {
    "", "@", "+x", "(", "(1", "(1,", "[", "[1,", "[0;", "[0; 3", "f(", "f(1,", "f(1 2)",
    "a[", "a[1", "a.", "a?.", "Point {", "Point { : 1 }", "Point { x: }", "Point { x: 1",
    "if x { 1 }", "if (x { 1 }", "if (x) 1", "if (x) { 1", "if (x) {} else",
    "match x", "match x { _ 1 }", "match x { _ => }", "match x { 1 => 1 2 => 2 }", "match x { _ => 0",
    "|int| 1", "|int x", "once", "const", "send once",
    "[&] || 1", "[&x,] once", "[x] once |int| 0",
    "\"{x\"", "\"{x", "\"{x}tail", "\"{x}{", "await", "return +", "a as", "a..="});
  fixed(out, 'P', "pattern-error", {"", "@", "-", "Some(", "Some(x", "(x,", "P {", "P { : x }", "P { x: }", "P { x",
                                    "1 |", "0..", "0..=", "1..x", "\"{x}\""});
  fixed(out, 'S', "statement-error", {"", "@", "var x;", "var = 1;", "int x = ;", "int x = 1", "x = 1", "x + 1 x",
                                      "while x) { }", "while (x { }", "while (x) ", "for x in y) { }", "for (x y) { }",
                                      "for (x in y { }", "for (x in y)", "loop"});

  add(out, 'M', "invalid-identifier", std::string("main() { var ") + "\xff\x80" + " = 1; }");
  add(out, 'M', "invalid-string-bytes", std::string("main() { var x = \"") + "\xc0\xaf" + "\"; }");
  add(out, 'M', "nul-string", std::string("main() { var x = \"a") + std::string(1, '\0') + "b\"; }");
  add(out, 'M', "nul-token", std::string("main() { ") + std::string(1, '\0') + " return 0; }");
  add(out, 'E', "truncated-utf8-char", std::string("'\xe2\x82", 3));
  add(out, 'E', "nul-between-operands", std::string("a") + std::string(1, '\0') + "+ b");

  add(out, 'E', "depth-200-parens", std::string(200, '(') + "1" + std::string(200, ')'));
  add(out, 'E', "depth-200-blocks", std::string(200, '{') + "1" + std::string(200, '}'));
  add(out, 'P', "depth-200-pattern", std::string(200, '(') + "x" + std::string(200, ')'));
  std::string generic;
  for (int i = 0; i < 200; ++i) generic += "Vec<";
  generic += "int";
  generic += std::string(200, '>');
  add(out, 'T', "depth-200-generics", generic);
}

static std::vector<std::string> feature_sources() {
  return {
R"SNX(/// Imports.
pub import std.collections.{Vec, HashMap};
import std.io.print;
/// Constant.
const int LIMIT = 8;
/// Packet.
pub struct Packet<T: Copy + Owned> {
    /// Payload.
    pub T value;
    /// Getter.
    T get(&const self) { self.value }
}
/// Variants.
enum Shape<T> {
    /// Empty.
    End,
    /// Tuple.
    Point(T, T),
    /// Record.
    Rect { int width; int height; }
    int tag(&const self) { 0 }
}
trait Measure {
    int size(&const self);
    int twice(&const self) { self.size() * 2 }
}
impl<T: Copy> Measure for Packet<T> {
    int size(&const self) { 1 }
}
unsafe impl<T: Copy + Owned> Send for Packet<T> { }
extern "C" { int read(*byte data, usize count); void finish(); }
pub async send int work(int x) { return await next(x); }
)SNX",
R"SNX(main() {
    var items = [1, 2, 3];
    int[4] zeros = [0; 4];
    var closure = [&const items, &zeros] const |int x| -> int { x + 1 };
    var owned = [items] send once || 2;
    for ((key, value) in pairs) {
        if (value > 0) { continue; } else { break; }
    }
    while (Point { x: 1 }.x < 3) { zeros[0] += 1; }
    loop { break; }
    var result = match token {
        Some((x, y)) if x > 0 => { x + y }
        Error { code, text: message } => 0,
        0..=9 | 11 => 1,
        _ => -1,
    };
    print("value {closure(result)} and {"nested {result}"}");
    var optional = parse(text)?.value ?? fallback;
    unsafe { *pointer = result; }
    return optional;
}
)SNX",
R"SNX(/// Nested types.
struct Types<T: Copy> {
    Map<String, Vec<Vec<T>>> values;
    fn const(int, int) -> int shared;
    fn send once() -> String owned;
    &const dyn Shape view;
    int[1 + 2] data;
    &const str? label;
    (int, (double, T)) tuple;
    *const byte raw;
    &const T lookup<U: Copy + Owned>(&const T x, U extra) { x }
}
enum Value { Empty, Pair(int, double), Record { pub int count; } }
trait Factory<T: Copy> {
    T make(&const self);
    fn once() -> T deferred(&const self) { once || self.make() }
}
main() {
    var x = first<Vec<Vec<int>>>(items);
    var y = Vec<Vec<int>>.new();
    var z = (x.0 as long + y[0] as long) << 2;
    var range = ..=z;
    var fn_value = |int n| -> int { if (n) { n } else { 0 } };
    match value { Value.Pair(a, b) => a, _ => 0 }
}
)SNX"
  };
}

static std::vector<synx::Token> source_tokens(const std::string& text) {
  synx::SourceFile file("mutation.snx", text);
  synx::DiagnosticSink sink;
  auto tokens = synx::Lexer(file, sink).tokenize();
  if (sink.has_errors()) throw std::runtime_error("mutation witness has lexical errors");
  tokens.erase(std::remove_if(tokens.begin(), tokens.end(),
                              [](const synx::Token& token) { return token.kind == synx::TokenKind::Eof; }),
               tokens.end());
  for (const auto& token : tokens) {
    if (token.span.start > token.span.end || token.span.end > text.size())
      throw std::runtime_error("invalid mutation token span");
  }
  return tokens;
}

static void mutations(std::vector<Snippet>& out) {
  std::mt19937 random(0x534e5801);
  auto seeds = feature_sources();
  for (size_t seed_index = 0; seed_index < seeds.size(); ++seed_index) {
    const auto& text = seeds[seed_index];
    const auto tokens = source_tokens(text);
    if (tokens.empty()) throw std::runtime_error("empty mutation witness");
    const std::string prefix = "seed-" + std::to_string(seed_index);

    add(out, 'M', prefix + "-complete", text);
    std::set<size_t> boundaries{0, text.size()};
    for (const auto& token : tokens) {
      boundaries.insert(token.span.start);
      boundaries.insert(token.span.end);
    }
    for (size_t boundary : boundaries)
      add(out, 'M', prefix + "-truncate-" + std::to_string(boundary), text.substr(0, boundary));

    // Delete each token once. Spaces prevent accidental token concatenation.
    for (size_t i = 0; i < tokens.size(); ++i) {
      const auto& token = tokens[i];
      add(out, 'M', prefix + "-delete-" + std::to_string(i),
          text.substr(0, token.span.start) + " " + text.substr(token.span.end));
    }

    // Insert documentation at every token position of the item witness.
    if (seed_index == 0) {
      for (size_t i = 0; i < tokens.size(); ++i) {
        const size_t position = tokens[i].span.start;
        add(out, 'M', prefix + "-doc-" + std::to_string(i),
            text.substr(0, position) + "\n/// Inserted.\n" + text.substr(position));
      }
    }

    // 192 deterministic mutations per witness: deletion, duplication, swap.
    for (size_t i = 0; i < 192; ++i) {
      size_t a = static_cast<size_t>(random()) % tokens.size();
      size_t b = static_cast<size_t>(random()) % tokens.size();
      if (a == b) b = (b + 1) % tokens.size();
      if (a > b) std::swap(a, b);
      const auto& first = tokens[a];
      const auto& second = tokens[b];
      std::string changed;
      switch (i % 3) {
        case 0:
          changed = text.substr(0, first.span.start) + " " + text.substr(first.span.end);
          break;
        case 1: {
          auto spelling = text.substr(first.span.start, first.span.end - first.span.start);
          changed = text.substr(0, first.span.start) + " " + spelling + " " + spelling + " " +
                    text.substr(first.span.end);
          break;
        }
        default:
          changed = text.substr(0, first.span.start) +
                    text.substr(second.span.start, second.span.end - second.span.start) +
                    text.substr(first.span.end, second.span.start - first.span.end) +
                    text.substr(first.span.start, first.span.end - first.span.start) +
                    text.substr(second.span.end);
          break;
      }
      add(out, 'M', prefix + "-mutate-" + std::to_string(i), std::move(changed));
    }
  }
}

static void write_file(const fs::path& path, std::string_view value) {
  std::ofstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("cannot create " + utf8(path));
  file.write(value.data(), static_cast<std::streamsize>(value.size()));
  file.close();
  if (!file) throw std::runtime_error("cannot write " + utf8(path));
}

static void reference(std::string& out, const Input& input, uint32_t id) {
  auto loaded = synx::SourceFile::load_utf8(utf8(input.path));
  CHECK(loaded.has_value());
  if (!loaded) throw std::runtime_error("cannot load " + utf8(input.path));
  auto& file = *loaded;
  file.set_id(id);

  synx::DiagnosticSink sink;
  auto tokens = synx::Lexer(file, sink).tokenize();
  synx::Parser parser(file, std::move(tokens), sink);
  std::string dump;
  switch (input.mode) {
    case 'M':
      dump = synx::ast::dump_strict(parser.parse_module());
      break;
    case 'E': {
      auto node = parser.parse_expression();
      if (node) dump = synx::ast::dump_strict(*node);
      break;
    }
    case 'T': {
      auto node = parser.parse_type_only();
      if (node) dump = synx::ast::dump_strict(*node);
      break;
    }
    case 'P': {
      auto node = parser.parse_pattern_only();
      if (node) dump = synx::ast::dump_strict(*node);
      break;
    }
    case 'S': {
      auto node = parser.parse_statement_only();
      if (node) dump = synx::ast::dump_strict(*node);
      break;
    }
    default:
      throw std::runtime_error("invalid parser entry mode");
  }

  integer(out, static_cast<unsigned char>(input.mode), 1);
  blob(out, dump);
  integer(out, parser.position(), 8);
  integer(out, sink.error_count(), 8);
  integer(out, sink.all().size(), 8);
  for (const auto& diagnostic : sink.all()) {
    integer(out, static_cast<unsigned>(diagnostic.severity), 4);
    span(out, diagnostic.span);
    blob(out, diagnostic.message);
    blob(out, diagnostic.code);
  }
  blob(out, sink.render(file));
}

int main(int argc, char** argv) {
  if (argc != 4 || std::string(argv[3]).size() != 1 || argv[3][0] < '0' || argv[3][0] > '3') {
    std::fputs("usage: test_selfhost_parser <synx> <source-root> <mode 0..3>\n", stderr);
    return 1;
  }

  const unsigned mode = static_cast<unsigned>(argv[3][0] - '0');
  fs::path root;
  try {
    const fs::path compiler = fs::absolute(argv[1]);
    const fs::path source = fs::absolute(argv[2]);
    root = fs::temp_directory_path() /
           ("synx-parser-" + std::to_string(mode) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);

    std::vector<fs::path> repository;
    for (const char* folder : {"selfhost", "std", "tests", "examples"}) {
      for (const auto& entry : fs::recursive_directory_iterator(source / folder))
        if (entry.is_regular_file() && entry.path().extension() == ".snx") repository.push_back(entry.path());
    }
    std::sort(repository.begin(), repository.end(),
              [](const fs::path& a, const fs::path& b) { return utf8(a) < utf8(b); });

    std::vector<Input> inputs;
    for (const auto& path : repository) inputs.push_back({'M', "repository", path});

    std::vector<Snippet> snippets;
    precedence(snippets);
    fixed_cases(snippets);
    mutations(snippets);
    for (size_t i = 0; i < snippets.size(); ++i) {
      const auto& snippet = snippets[i];
      auto path = root / ("case-" + std::to_string(i) + "-" + snippet.mode + ".snx");
      write_file(path, snippet.text);
      inputs.push_back({snippet.mode, snippet.label, path});
    }

    auto unicode_path = root / fs::path(u8"parser café λ.snx");
    write_file(unicode_path, "/// Unicode path.\nmain() { var x = ; }\n");
    inputs.push_back({'M', "unicode-path", unicode_path});

    if (inputs.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max()) - 7)
      throw std::runtime_error("too many parser inputs");

    const auto manifest = root / "manifest.txt";
    const auto report = root / "actual.bin";
    const auto log = root / "run.log";
    std::ofstream list(manifest, std::ios::binary);
    std::ofstream index(root / "records.tsv", std::ios::binary);
    if (!list || !index) throw std::runtime_error("cannot create manifest or record index");

    std::string expected;
    std::vector<size_t> ends;
    for (size_t i = 0; i < inputs.size(); ++i) {
      const auto& input = inputs[i];
      const auto path = utf8(input.path);
      if (path.find_first_of("\r\n\t") != std::string::npos)
        throw std::runtime_error("manifest path contains CR, LF, or tab");
      list << input.mode << '\t' << path << '\n';
      const size_t begin = expected.size();
      if (std::getenv("SYNX_PARITY_TRACE")) {
        std::fprintf(stderr, "%zu %c %s %s\n", i, input.mode, input.label.c_str(), path.c_str());
        std::fflush(stderr);
      }
      reference(expected, input, static_cast<uint32_t>(i + 7));
      ends.push_back(expected.size());
      index << i + 7 << '\t' << input.mode << '\t' << begin << '\t' << expected.size() << '\t' << input.label << '\t'
            << path << '\n';
    }
    list.close();
    index.close();
    if (!list || !index) throw std::runtime_error("cannot finish manifest or record index");
    write_file(root / "expected.bin", expected);

    const std::vector<std::vector<std::string>> flags = {
        {}, {"--no-regalloc"}, {"--release"}, {"--release", "--no-regalloc"}};
    const auto executable = root / "parser-probe.exe";
    std::vector<std::string> arguments = {"build", utf8(source / "selfhost/parser_probe.snx"), "-o", utf8(executable)};
    arguments.insert(arguments.end(), flags[mode].begin(), flags[mode].end());

    int rc = run(compiler, arguments, log);
    CHECK(rc == 0);
    if (rc) {
      std::fputs(read(log).c_str(), stderr);
      std::fprintf(stderr, "artifacts: %s\n", utf8(root).c_str());
      return 1;
    }

    rc = run(executable, {utf8(manifest), utf8(report)}, log);
    CHECK(rc == 0);
    if (rc) {
      std::fputs(read(log).c_str(), stderr);
      std::fprintf(stderr, "artifacts: %s\n", utf8(root).c_str());
      return 1;
    }

    CHECK(fs::is_regular_file(report));
    const std::string actual = read(report);
    CHECK(actual == expected);
    if (actual != expected) {
      size_t offset = 0;
      while (offset < expected.size() && offset < actual.size() && expected[offset] == actual[offset]) ++offset;
      const size_t record = static_cast<size_t>(std::upper_bound(ends.begin(), ends.end(), offset) - ends.begin());
      std::fprintf(stderr, "mode %u: mismatch at byte %zu; expected %zu bytes, got %zu\n", mode, offset,
                   expected.size(), actual.size());
      if (record < inputs.size()) {
        std::fprintf(stderr, "file ID %zu, entry %c, %s: %s\n", record + 7, inputs[record].mode,
                     inputs[record].label.c_str(), utf8(inputs[record].path).c_str());
      } else {
        std::fputs("mismatch after final expected record\n", stderr);
      }
      std::fprintf(stderr, "artifacts: %s; record offsets: records.tsv\n", utf8(root).c_str());
      return 1;
    }

    std::printf("mode %u: %zu repository sources, %zu generated snippets, 1 Unicode-path case, %zu report bytes\n",
                mode, repository.size(), snippets.size(), expected.size());
    fs::remove_all(root);
    return synx_test::finish("selfhost_parser");
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\nartifacts: %s\n", error.what(), utf8(root).c_str());
    return 1;
  }
}
