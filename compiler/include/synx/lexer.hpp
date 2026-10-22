#pragma once

#include <vector>

#include "synx/diagnostics.hpp"
#include "synx/source.hpp"
#include "synx/token.hpp"

namespace synx {

// Turns source text into tokens. Errors go to the sink and lexing continues,
// so one run reports every lexical problem in the file.
//
// String interpolation: a literal with no `{` becomes one StringLit. A
// literal with `{expr}` parts becomes
//   StringStart StringText* (InterpStart <tokens> InterpEnd StringText*)* StringEnd
// Braces inside an interpolation are tracked so nested blocks work.
class Lexer {
 public:
  Lexer(const SourceFile& file, DiagnosticSink& sink);

  // Lexes the whole file. The last token is always Eof.
  std::vector<Token> tokenize();

 private:
  struct InterpFrame {
    int brace_depth = 0;  // open braces inside the current interpolation
  };

  Token next();
  Token lex_in_string();
  Token lex_number(uint32_t start);
  Token lex_identifier(uint32_t start);
  Token lex_char(uint32_t start);
  Token lex_string_open(uint32_t start);
  bool lex_escape(std::string& out);
  void skip_whitespace_and_comments();
  bool skip_block_comment();
  Token lex_doc_comment(uint32_t start);

  char peek(uint32_t ahead = 0) const;
  bool at_end() const { return pos_ >= text_.size(); }
  Token make(TokenKind kind, uint32_t start);
  Token error(uint32_t start, std::string message, const char* code = "");

  const SourceFile& file_;
  DiagnosticSink& sink_;
  std::string_view text_;
  uint32_t pos_ = 0;

  // True while the next token must come from inside a string literal.
  bool in_string_ = false;
  std::vector<InterpFrame> frames_;
};

}  // namespace synx
