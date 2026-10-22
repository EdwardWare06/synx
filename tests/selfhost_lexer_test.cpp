// Differential source, diagnostic, and lexer checks for the Synx compiler port.
#include <algorithm>
#include <bit>
#include <chrono>
#include <random>

#include "selfhost_support.hpp"
#include "synx/lexer.hpp"
#include "test_support.hpp"

namespace fs = std::filesystem;
using namespace selfhost_test;

static void reference(std::string& out, const fs::path& path, uint32_t id) {
  auto loaded = synx::SourceFile::load_utf8(utf8(path)); CHECK(loaded.has_value());
  if (!loaded) return;
  auto& file = *loaded; file.set_id(id);
  synx::DiagnosticSink sink;
  auto tokens = synx::Lexer(file, sink).tokenize();
  integer(out, tokens.size(), 8);
  for (const auto& item : tokens) {
    integer(out, static_cast<unsigned>(item.kind), 4); span(out, item.span);
    blob(out, item.text); integer(out, item.int_value, 8);
    integer(out, std::bit_cast<uint64_t>(item.float_value), 8);
  }
  integer(out, sink.error_count(), 8); integer(out, sink.all().size(), 8);
  for (const auto& item : sink.all()) {
    integer(out, static_cast<unsigned>(item.severity), 4); span(out, item.span);
    blob(out, item.message); blob(out, item.code);
  }
  blob(out, sink.render(file)); integer(out, file.line_count(), 4);
  for (size_t i = 0; i <= file.text().size() + 1; ++i) {
    auto lc = file.line_col(static_cast<uint32_t>(i)); integer(out, lc.line, 4); integer(out, lc.col, 4);
  }
  for (uint32_t i = 0; i <= file.line_count() + 1; ++i) blob(out, file.line_text(i));
  span(out, synx::Span::merge({5,9,id}, {2,7,id+1}));
  synx::DiagnosticSink mixed;
  mixed.error({0, static_cast<uint32_t>(file.text().size()), id}, "whole source", "SNXTEST");
  mixed.warning({static_cast<uint32_t>(file.text().size()), static_cast<uint32_t>(file.text().size()), id}, "warning");
  mixed.note({0,0,id+100000}, "fallback");
  synx::SourceFile replaced("replaced.snx", ""); replaced.set_id(id); mixed.add_source(replaced);
  synx::SourceFile alternate("registered.snx", ""); alternate.set_id(id); mixed.add_source(alternate);
  blob(out, mixed.render(file));
}
static std::vector<std::string> cases() {
  std::vector<std::string> out = {
    "", "\n", "\r\n\t\r", std::string("\0",1), std::string("a\0b",3), "\xef\xbb\xbf", "\xff\x80\xc0\xaf",
    "/// one\r\n\t/// two\n\n  /// three\n//// plain\n///last",
    "/* a /* b */ c */ x /* unfinished", "// end", "//// end", "///",
    "a _ A z Z x0 \xc3\xa9 \xce\xbb \xf0\x9f\x92\xa1",
    "0 0x 0b 0x_ 0b_ 0xff 0xFFL 0b101L 1__2_ 1..2 1..=2",
    "2147483647 2147483648 9223372036854775807L 9223372036854775808L 18446744073709551615 18446744073709551616 0x10000000000000000",
    "0.1 0.3 3.14159 1e5 1E-5 1e+5 1_000.5_0 0.0 0e999999 1f 1.5f 1e5f 1.00000001f",
    "1.7976931348623157e308 1.7976931348623158e308 1.7976931348623159e308 1e309 1e-310 5e-324 2.5e-324 1e-400",
    "0x1.8 0b1.0 0b1f 1.f 1.e5 1e 1e+ 1__0.5 1.5L 1e0999999999 1e0_9 123z 0x12u",
    "'a' '\\n' '\\0' '\\u{1f4a1}' '\\u{d800}' '\\u{}' '' 'ab' '\\q' '\\uZ' '\xff' '\xc0\xaf'",
    "\"plain\" \"{{double}}\" \"escaped\\{x\\}\" \"\\n\\t\\r\\0\\\\\\\"\\'\"",
    "\"{x}\" \"a{x}b{y}c\" \"{ Point { x: 1 } }\" \"{\"nested {value}\"}\"",
    "\"\\u{1f4a1}\" \"\\u{}\" \"\\u{1234567}\" \"\\u{110000}\" \"\\u{d800}\" \"\\uX\"",
    "\"}\" \"{x} }\" \"{x}\\q\" \"unterminated\nnext", "\"{x}", "\"{", "\"\\", "'\\", "\"{x}\\",
    "()[]{} ,;: :: . .. ..= -> => ? ?. ?? + - * / % = == ! != < > <= >= && || & | ^ ~ << += -= *= /= %= &= |= ^= <<= >>= >> >>>=",
    "as async await break const continue dyn else enum extern false fn for if impl import in loop match once pub return self Self struct trait true unsafe var while bool byte short int long uint ulong usize float double char void"
  };
  out.insert(out.end(), {
    "\t\t@", "x\ty @", "\xc3\xa9 @", "\"abc\r\nx", "a\rb @",
    "'\xe2\x82", "\xe2\x82", "\"\xe2\x82", "0XFF 0B101 0xFFFFFFFFFFFFFFFF 1__2",
    "\"\\u{10ffff}\\u{0}\\u{ffffff}\\u{1_}\" '\\u{10ffff}' '\\u{0}' '\\u{ffffff}' '\\u{1_}'",
    "\"a{x}b{y}c{z}\" \"{ { { { \"inner {v}\" } } } }\""
  });
  for (int lines : {9, 99, 999}) out.push_back(std::string(lines, '\n') + "x\t@");
  out.push_back("0b" + std::string(64, '1') + " 0b" + std::string(65, '1'));
  out.push_back("0." + std::string(100000, '1'));
  // Rounding ties, long mantissas, exponent saturation, and exact subnormals.
  out.push_back("1.00000000000000011102230246251565404236316680908203125 1.00000000000000011102230246251565404236316680908203126");
  out.push_back("0." + std::string(800, '9') + " 1" + std::string(800, '0') + "e-800");
  out.push_back("1e" + std::string(800, '9') + " 0e-" + std::string(800, '9'));
  out.push_back(std::string(512, '/') + "\n" + std::string(256, '*'));
  out.push_back(std::string(256, '{') + "\n");
  std::string nested;
  for (int i=0;i<128;++i) nested += "/*";
  for (int i=0;i<128;++i) nested += "*/";
  out.push_back(nested);
  std::string alphabet;
  for (int i=0;i<256;++i) alphabet += static_cast<char>(i);
  out.push_back(alphabet);
  const std::string seed = "/// doc\nmain() { var x = 1.25e-9; print(\"a{x + 2}z\"); /* comment */ }\n";
  for (size_t i=0;i<=seed.size();++i) out.push_back(seed.substr(0,i));
  std::mt19937 random(0x534e5801);
  for (int i=0;i<256;++i) {
    auto value=seed;
    for (int j=0;j<1+i%6;++j) value[random()%value.size()]=static_cast<char>(random()%256);
    out.push_back(value);
  }
  return out;
}
int main(int argc, char** argv) {
  if (argc != 3) return 1;
  fs::path compiler = fs::absolute(argv[1]), source = fs::absolute(argv[2]);
  auto root = fs::temp_directory_path() / ("synx-lexer-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(root);
  std::vector<fs::path> files;
  for (const char* folder : {"selfhost", "std", "tests", "examples"}) {
    for (const auto& entry : fs::recursive_directory_iterator(source / folder))
      if (entry.is_regular_file() && entry.path().extension()==".snx") files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());
  auto malformed = cases();
  for (size_t i=0;i<malformed.size();++i) {
    auto path = root / ("case-"+std::to_string(i)+".snx");
    std::ofstream file(path, std::ios::binary); file.write(malformed[i].data(), malformed[i].size()); file.close(); files.push_back(path);
  }
  auto unicode_path = root / fs::path(u8"source café λ.snx");
  std::ofstream(unicode_path, std::ios::binary) << "\t\"unterminated\r\n";
  files.push_back(unicode_path);
  auto manifest = root/"manifest.txt", report=root/"actual.bin", log=root/"run.log";
  std::ofstream list(manifest, std::ios::binary); std::string expected;
  unsigned kind_count = static_cast<unsigned>(synx::TokenKind::ShrEq) + 1;
  integer(expected, kind_count, 4);
  for (unsigned i = 0; i < kind_count; ++i) {
    auto kind = static_cast<synx::TokenKind>(i);
    blob(expected, synx::token_kind_name(kind));
    integer(expected, synx::is_keyword(kind), 1);
    integer(expected, synx::is_primitive_type_keyword(kind), 1);
  }
  for (size_t i=0;i<files.size();++i) { list << utf8(files[i]) << '\n'; reference(expected,files[i],static_cast<uint32_t>(i+7)); }
  list.close();
  std::ofstream(root/"expected.bin",std::ios::binary).write(expected.data(),expected.size());
  unsigned mode = 0;
  for (auto flags : {std::vector<std::string>{}, std::vector<std::string>{"--no-regalloc"}, std::vector<std::string>{"--release"}, std::vector<std::string>{"--release","--no-regalloc"}}) {
    auto executable=root/("probe-"+std::to_string(mode++)+".exe");
    std::vector<std::string> args={"build",utf8(source/"selfhost/lexer_probe.snx"),"-o",utf8(executable)};
    args.insert(args.end(),flags.begin(),flags.end());
    int rc=run(compiler,args,log); CHECK(rc==0);
    if (rc) { std::fputs(read(log).c_str(),stderr); return 1; }
    rc=run(executable,{utf8(manifest),utf8(report)},log); CHECK(rc==0);
    if (rc) { std::fputs(read(log).c_str(),stderr); return 1; }
    auto actual=read(report); CHECK(actual==expected);
    if (actual!=expected) {
      size_t position=0; while (position<actual.size() && position<expected.size() && actual[position]==expected[position]) ++position;
      std::fprintf(stderr,"mode %u: mismatch at byte %zu; expected %zu bytes, got %zu; artifacts: %s\n",mode-1,position,expected.size(),actual.size(),utf8(root).c_str());
      return 1;
    }
  }
  std::printf("%zu sources (%zu generated), 4 modes, %zu bytes per report\n",files.size(),malformed.size(),expected.size());
  fs::remove_all(root);
  return synx_test::finish("selfhost_lexer");
}
