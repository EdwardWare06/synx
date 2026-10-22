#include "synx/lexer.hpp"

#include <string>
#include <vector>

#include "synx/diagnostics.hpp"
#include "synx/source.hpp"
#include "test_support.hpp"

using synx::Token;
using synx::TokenKind;
using K = synx::TokenKind;

namespace {

struct Lexed {
  std::vector<Token> tokens;
  synx::DiagnosticSink sink;
};

Lexed lex(const std::string& src) {
  Lexed out;
  synx::SourceFile file("test.snx", src);
  synx::Lexer lexer(file, out.sink);
  out.tokens = lexer.tokenize();
  return out;
}

std::vector<K> kinds(const std::string& src) {
  auto l = lex(src);
  std::vector<K> ks;
  for (auto& t : l.tokens) ks.push_back(t.kind);
  return ks;
}

void test_punctuation() {
  auto ks = kinds("( ) [ ] { } , ; : :: . .. ..= -> => ? ?. ??");
  std::vector<K> want = {K::LParen, K::RParen, K::LBracket, K::RBracket,
                         K::LBrace, K::RBrace, K::Comma, K::Semi, K::Colon,
                         K::ColonColon, K::Dot, K::DotDot, K::DotDotEq,
                         K::Arrow, K::FatArrow, K::Question, K::QuestionDot,
                         K::QuestionQuestion, K::Eof};
  CHECK(ks == want);

  ks = kinds("+ - * / % = == ! != < > <= >= && || & | ^ ~ <<");
  want = {K::Plus, K::Minus, K::Star, K::Slash, K::Percent, K::Eq, K::EqEq,
          K::Bang, K::BangEq, K::Lt, K::Gt, K::LtEq, K::GtEq, K::AmpAmp,
          K::PipePipe, K::Amp, K::Pipe, K::Caret, K::Tilde, K::Shl, K::Eof};
  CHECK(ks == want);

  ks = kinds("+= -= *= /= %= &= |= ^= <<= >>=");
  want = {K::PlusEq, K::MinusEq, K::StarEq, K::SlashEq, K::PercentEq,
          K::AmpEq, K::PipeEq, K::CaretEq, K::ShlEq, K::ShrEq, K::Eof};
  CHECK(ks == want);

  // ">>" is two Gt tokens so nested generics close cleanly.
  ks = kinds("Vec<Vec<int>>");
  want = {K::Ident, K::Lt, K::Ident, K::Lt, K::KwInt, K::Gt, K::Gt, K::Eof};
  CHECK(ks == want);
}

void test_keywords_and_identifiers() {
  auto l = lex("int main struct Self self _x x1 async");
  CHECK_EQ(l.tokens[0].kind, K::KwInt);
  CHECK_EQ(l.tokens[1].kind, K::Ident);
  CHECK_EQ(l.tokens[1].text, std::string("main"));
  CHECK_EQ(l.tokens[2].kind, K::KwStruct);
  CHECK_EQ(l.tokens[3].kind, K::KwSelfType);
  CHECK_EQ(l.tokens[4].kind, K::KwSelf);
  CHECK_EQ(l.tokens[5].text, std::string("_x"));
  CHECK_EQ(l.tokens[6].text, std::string("x1"));
  CHECK_EQ(l.tokens[7].kind, K::KwAsync);
  CHECK(synx::is_primitive_type_keyword(K::KwInt));
  CHECK(!synx::is_primitive_type_keyword(K::KwIf));
}

void test_numbers() {
  auto l = lex("42 0xFF 0b1010 1_000 42L 1.5f 1.5 2e10 3f 7E-2");
  CHECK_EQ(l.tokens[0].kind, K::IntLit);
  CHECK_EQ(l.tokens[0].int_value, 42u);
  CHECK_EQ(l.tokens[1].int_value, 255u);
  CHECK_EQ(l.tokens[2].int_value, 10u);
  CHECK_EQ(l.tokens[3].int_value, 1000u);
  CHECK_EQ(l.tokens[4].kind, K::LongLit);
  CHECK_EQ(l.tokens[4].int_value, 42u);
  CHECK_EQ(l.tokens[5].kind, K::FloatLit);
  CHECK(l.tokens[5].float_value == 1.5);
  CHECK_EQ(l.tokens[6].kind, K::DoubleLit);
  CHECK(l.tokens[6].float_value == 1.5);
  CHECK_EQ(l.tokens[7].kind, K::DoubleLit);
  CHECK(l.tokens[7].float_value == 2e10);
  CHECK_EQ(l.tokens[8].kind, K::FloatLit);
  CHECK(l.tokens[8].float_value == 3.0);
  CHECK_EQ(l.tokens[9].kind, K::DoubleLit);
  CHECK(l.tokens[9].float_value == 7e-2);
  CHECK(!l.sink.has_errors());

  // A range after an integer must not become a float.
  auto ks = kinds("0..10");
  std::vector<K> want = {K::IntLit, K::DotDot, K::IntLit, K::Eof};
  CHECK(ks == want);

  // Tuple field access stays an int literal after a dot.
  ks = kinds("t.0");
  want = {K::Ident, K::Dot, K::IntLit, K::Eof};
  CHECK(ks == want);

  auto bad = lex("123abc 0x 99999999999999999999 1.5L");
  CHECK_EQ(bad.sink.error_count(), 4u);
}

void test_chars() {
  auto l = lex("'a' '\\n' '\\u{1F600}' 'é'");
  CHECK_EQ(l.tokens[0].kind, K::CharLit);
  CHECK_EQ(l.tokens[0].int_value, 97u);
  CHECK_EQ(l.tokens[1].int_value, 10u);
  CHECK_EQ(l.tokens[2].int_value, 0x1F600u);
  CHECK_EQ(l.tokens[3].int_value, 0xE9u);
  CHECK(!l.sink.has_errors());

  auto bad = lex("'' 'ab' '\\q'");
  CHECK(bad.sink.error_count() >= 3);
}

void test_plain_strings() {
  auto l = lex("\"hello\" \"a\\tb\\n\" \"{{x}}\" \"\\u{48}i\" \"\"");
  CHECK_EQ(l.tokens[0].kind, K::StringLit);
  CHECK_EQ(l.tokens[0].text, std::string("hello"));
  CHECK_EQ(l.tokens[1].text, std::string("a\tb\n"));
  CHECK_EQ(l.tokens[2].text, std::string("{x}"));
  CHECK_EQ(l.tokens[3].text, std::string("Hi"));
  CHECK_EQ(l.tokens[4].text, std::string(""));
  CHECK(!l.sink.has_errors());

  auto bad = lex("\"open\nint x;");
  CHECK_EQ(bad.sink.error_count(), 1u);
  // Lexing continues after the error.
  CHECK_EQ(bad.tokens[1].kind, K::KwInt);
}

void test_interpolation() {
  auto l = lex("\"a {x} b {p.len()} c\"");
  std::vector<K> want = {K::StringStart, K::StringText, K::InterpStart,
                         K::Ident, K::InterpEnd, K::StringText, K::InterpStart,
                         K::Ident, K::Dot, K::Ident, K::LParen, K::RParen,
                         K::InterpEnd, K::StringText, K::StringEnd, K::Eof};
  std::vector<K> got;
  for (auto& t : l.tokens) got.push_back(t.kind);
  CHECK(got == want);
  CHECK_EQ(l.tokens[1].text, std::string("a "));
  CHECK_EQ(l.tokens[5].text, std::string(" b "));
  CHECK_EQ(l.tokens[13].text, std::string(" c"));
  CHECK(!l.sink.has_errors());

  // Braces inside an interpolation, and a nested string inside it.
  l = lex("\"{ if (c) { \"in\" } else { 2 } }\"");
  got.clear();
  for (auto& t : l.tokens) got.push_back(t.kind);
  want = {K::StringStart, K::InterpStart, K::KwIf, K::LParen, K::Ident,
          K::RParen, K::LBrace, K::StringLit, K::RBrace, K::KwElse,
          K::LBrace, K::IntLit, K::RBrace, K::InterpEnd, K::StringEnd, K::Eof};
  CHECK(got == want);
  CHECK(!l.sink.has_errors());

  // Interpolation at the very start and end, no text parts.
  got = kinds("\"{x}\"");
  want = {K::StringStart, K::InterpStart, K::Ident, K::InterpEnd,
          K::StringEnd, K::Eof};
  CHECK(got == want);

  auto bad = lex("\"a } b\"");
  CHECK_EQ(bad.sink.error_count(), 1u);
}

void test_comments() {
  auto ks = kinds("a // line\nb /* block /* nested */ still */ c //// four\nd");
  std::vector<K> want = {K::Ident, K::Ident, K::Ident, K::Ident, K::Eof};
  CHECK(ks == want);

  auto l = lex("/// First line\n///  second\nint x;");
  CHECK_EQ(l.tokens[0].kind, K::DocComment);
  CHECK_EQ(l.tokens[0].text, std::string("First line\n second"));
  CHECK_EQ(l.tokens[1].kind, K::KwInt);

  auto bad = lex("/* never closed");
  CHECK_EQ(bad.sink.error_count(), 1u);
}

void test_spans_and_errors() {
  auto l = lex("int  x = @;");
  CHECK_EQ(l.tokens[0].span.start, 0u);
  CHECK_EQ(l.tokens[0].span.end, 3u);
  CHECK_EQ(l.tokens[1].span.start, 5u);
  CHECK_EQ(l.tokens[3].kind, K::Error);
  CHECK_EQ(l.tokens[4].kind, K::Semi);
  CHECK_EQ(l.sink.error_count(), 1u);

  synx::SourceFile file("t.snx", "int  x = @;\n");
  std::string rendered = synx::DiagnosticSink::render_one(l.sink.all()[0], file);
  CHECK(rendered.find("t.snx:1:10: error[SNX0101]: unexpected character '@'") == 0);
  CHECK(rendered.find("         ^") != std::string::npos);
}

void test_source_lines() {
  synx::SourceFile file("t.snx", "ab\ncd\r\nef");
  CHECK_EQ(file.line_count(), 3u);
  CHECK_EQ(file.line_col(0).line, 1u);
  CHECK_EQ(file.line_col(4).line, 2u);
  CHECK_EQ(file.line_col(4).col, 2u);
  CHECK_EQ(file.line_col(7).line, 3u);
  CHECK(file.line_text(2) == "cd");
  CHECK(file.line_text(3) == "ef");
}

}  // namespace

int main() {
  test_punctuation();
  test_keywords_and_identifiers();
  test_numbers();
  test_chars();
  test_plain_strings();
  test_interpolation();
  test_comments();
  test_spans_and_errors();
  test_source_lines();
  return synx_test::finish("lexer_test");
}
