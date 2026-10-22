#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "synx/source.hpp"

namespace synx {

// X(name, display text). Display text is used in diagnostics and dumps.
#define SYNX_TOKEN_KINDS(X)                      \
  X(Eof, "end of file")                          \
  X(Error, "invalid token")                      \
  X(Ident, "identifier")                         \
  X(IntLit, "integer literal")                   \
  X(LongLit, "long literal")                     \
  X(FloatLit, "float literal")                   \
  X(DoubleLit, "double literal")                 \
  X(CharLit, "char literal")                     \
  X(StringLit, "string literal")                 \
  X(StringStart, "string start")                 \
  X(StringText, "string text")                   \
  X(InterpStart, "interpolation start")          \
  X(InterpEnd, "interpolation end")              \
  X(StringEnd, "string end")                     \
  X(DocComment, "doc comment")                   \
  /* keywords */                                 \
  X(KwAs, "as")                                  \
  X(KwAsync, "async")                            \
  X(KwAwait, "await")                            \
  X(KwBreak, "break")                            \
  X(KwConst, "const")                            \
  X(KwContinue, "continue")                      \
  X(KwDyn, "dyn")                                \
  X(KwElse, "else")                              \
  X(KwEnum, "enum")                              \
  X(KwExtern, "extern")                          \
  X(KwFalse, "false")                            \
  X(KwFn, "fn")                                  \
  X(KwFor, "for")                                \
  X(KwIf, "if")                                  \
  X(KwImpl, "impl")                              \
  X(KwImport, "import")                          \
  X(KwIn, "in")                                  \
  X(KwLoop, "loop")                              \
  X(KwMatch, "match")                            \
  X(KwOnce, "once")                              \
  X(KwPub, "pub")                                \
  X(KwReturn, "return")                          \
  X(KwSelf, "self")                              \
  X(KwSelfType, "Self")                          \
  X(KwStruct, "struct")                          \
  X(KwTrait, "trait")                            \
  X(KwTrue, "true")                              \
  X(KwUnsafe, "unsafe")                          \
  X(KwVar, "var")                                \
  X(KwWhile, "while")                            \
  /* primitive type keywords */                  \
  X(KwBool, "bool")                              \
  X(KwByte, "byte")                              \
  X(KwShort, "short")                            \
  X(KwInt, "int")                                \
  X(KwLong, "long")                              \
  X(KwUint, "uint")                              \
  X(KwUlong, "ulong")                            \
  X(KwUsize, "usize")                            \
  X(KwFloat, "float")                            \
  X(KwDouble, "double")                          \
  X(KwChar, "char")                              \
  X(KwVoid, "void")                              \
  /* punctuation */                              \
  X(LParen, "(")                                 \
  X(RParen, ")")                                 \
  X(LBracket, "[")                               \
  X(RBracket, "]")                               \
  X(LBrace, "{")                                 \
  X(RBrace, "}")                                 \
  X(Comma, ",")                                  \
  X(Semi, ";")                                   \
  X(Colon, ":")                                  \
  X(ColonColon, "::")                            \
  X(Dot, ".")                                    \
  X(DotDot, "..")                                \
  X(DotDotEq, "..=")                             \
  X(Arrow, "->")                                 \
  X(FatArrow, "=>")                              \
  X(Question, "?")                               \
  X(QuestionDot, "?.")                           \
  X(QuestionQuestion, "??")                      \
  X(Plus, "+")                                   \
  X(Minus, "-")                                  \
  X(Star, "*")                                   \
  X(Slash, "/")                                  \
  X(Percent, "%")                                \
  X(Eq, "=")                                     \
  X(EqEq, "==")                                  \
  X(Bang, "!")                                   \
  X(BangEq, "!=")                                \
  X(Lt, "<")                                     \
  X(Gt, ">")                                     \
  X(LtEq, "<=")                                  \
  X(GtEq, ">=")                                  \
  X(AmpAmp, "&&")                                \
  X(PipePipe, "||")                              \
  X(Amp, "&")                                    \
  X(Pipe, "|")                                   \
  X(Caret, "^")                                  \
  X(Tilde, "~")                                  \
  X(Shl, "<<")                                   \
  X(PlusEq, "+=")                                \
  X(MinusEq, "-=")                               \
  X(StarEq, "*=")                                \
  X(SlashEq, "/=")                               \
  X(PercentEq, "%=")                             \
  X(AmpEq, "&=")                                 \
  X(PipeEq, "|=")                                \
  X(CaretEq, "^=")                               \
  X(ShlEq, "<<=")                                \
  X(ShrEq, ">>=")

// Note: ">>" is never produced. The lexer emits two adjacent Gt tokens and
// the parser joins them into a shift when it is parsing an expression. This
// keeps nested generics like Vec<Vec<int>> simple.

enum class TokenKind : uint8_t {
#define X(name, text) name,
  SYNX_TOKEN_KINDS(X)
#undef X
};

const char* token_kind_name(TokenKind kind);
bool is_keyword(TokenKind kind);
bool is_primitive_type_keyword(TokenKind kind);

struct Token {
  TokenKind kind = TokenKind::Eof;
  Span span;
  // Decoded text: identifier name, string contents with escapes resolved,
  // doc comment body, or the literal's source text for numbers.
  std::string text;
  // IntLit, LongLit: the value. CharLit: the Unicode scalar value.
  uint64_t int_value = 0;
  // FloatLit, DoubleLit: the value.
  double float_value = 0.0;

  bool is(TokenKind k) const { return kind == k; }
};

// Human readable one line dump, used by `synx lex` and tests.
std::string token_to_string(const Token& token);

}  // namespace synx
