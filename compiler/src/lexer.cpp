#include "synx/lexer.hpp"

#include <cerrno>
#include <cstdlib>
#include <string_view>
#include <unordered_map>

namespace synx {

namespace {

const std::unordered_map<std::string_view, TokenKind>& keyword_table() {
  static const std::unordered_map<std::string_view, TokenKind> table = {
      {"as", TokenKind::KwAs},         {"async", TokenKind::KwAsync},
      {"await", TokenKind::KwAwait},   {"break", TokenKind::KwBreak},
      {"const", TokenKind::KwConst},   {"continue", TokenKind::KwContinue},
      {"dyn", TokenKind::KwDyn},       {"else", TokenKind::KwElse},
      {"enum", TokenKind::KwEnum},     {"extern", TokenKind::KwExtern},
      {"false", TokenKind::KwFalse},   {"fn", TokenKind::KwFn},
      {"for", TokenKind::KwFor},       {"if", TokenKind::KwIf},
      {"impl", TokenKind::KwImpl},     {"import", TokenKind::KwImport},
      {"in", TokenKind::KwIn},         {"loop", TokenKind::KwLoop},
      {"match", TokenKind::KwMatch},   {"pub", TokenKind::KwPub},
      {"once", TokenKind::KwOnce},
      {"return", TokenKind::KwReturn}, {"self", TokenKind::KwSelf},
      {"Self", TokenKind::KwSelfType}, {"struct", TokenKind::KwStruct},
      {"trait", TokenKind::KwTrait},   {"true", TokenKind::KwTrue},
      {"unsafe", TokenKind::KwUnsafe}, {"var", TokenKind::KwVar},
      {"while", TokenKind::KwWhile},   {"bool", TokenKind::KwBool},
      {"byte", TokenKind::KwByte},     {"short", TokenKind::KwShort},
      {"int", TokenKind::KwInt},       {"long", TokenKind::KwLong},
      {"uint", TokenKind::KwUint},     {"ulong", TokenKind::KwUlong},
      {"usize", TokenKind::KwUsize},   {"float", TokenKind::KwFloat},
      {"double", TokenKind::KwDouble}, {"char", TokenKind::KwChar},
      {"void", TokenKind::KwVoid},
  };
  return table;
}

bool is_ident_start(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
         static_cast<unsigned char>(c) >= 0x80;
}

bool is_ident_char(char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool is_hex_digit(char c) {
  return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int hex_value(char c) {
  if (is_digit(c)) return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return c - 'A' + 10;
}

void append_utf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// Decodes one UTF-8 code point from s. Returns the byte length, 0 on error.
size_t decode_utf8(std::string_view s, uint32_t& cp) {
  if (s.empty()) return 0;
  unsigned char c = static_cast<unsigned char>(s[0]);
  size_t len;
  if (c < 0x80) { cp = c; return 1; }
  if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
  else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
  else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
  else return 0;
  if (s.size() < len) return 0;
  for (size_t i = 1; i < len; ++i) {
    unsigned char cc = static_cast<unsigned char>(s[i]);
    if ((cc & 0xC0) != 0x80) return 0;
    cp = (cp << 6) | (cc & 0x3F);
  }
  return len;
}

}  // namespace

Lexer::Lexer(const SourceFile& file, DiagnosticSink& sink)
    : file_(file), sink_(sink), text_(file.text()) {}

char Lexer::peek(uint32_t ahead) const {
  size_t i = static_cast<size_t>(pos_) + ahead;
  return i < text_.size() ? text_[i] : '\0';
}

Token Lexer::make(TokenKind kind, uint32_t start) {
  Token t;
  t.kind = kind;
  t.span = Span{start, pos_, file_.id()};
  return t;
}

Token Lexer::error(uint32_t start, std::string message, const char* code) {
  sink_.error(Span{start, pos_ > start ? pos_ : start + 1, file_.id()}, message, code);
  Token t = make(TokenKind::Error, start);
  t.text = std::move(message);
  return t;
}

std::vector<Token> Lexer::tokenize() {
  std::vector<Token> out;
  for (;;) {
    Token t = next();
    bool done = t.kind == TokenKind::Eof;
    out.push_back(std::move(t));
    if (done) break;
  }
  return out;
}

// ---------- whitespace and comments ----------

bool Lexer::skip_block_comment() {
  // pos_ is at the first '/' of "/*".
  uint32_t start = pos_;
  pos_ += 2;
  int depth = 1;
  while (!at_end() && depth > 0) {
    if (peek() == '/' && peek(1) == '*') {
      pos_ += 2;
      ++depth;
    } else if (peek() == '*' && peek(1) == '/') {
      pos_ += 2;
      --depth;
    } else {
      ++pos_;
    }
  }
  if (depth > 0) {
    sink_.error(Span{start, start + 2, file_.id()}, "unterminated block comment");
    return false;
  }
  return true;
}

void Lexer::skip_whitespace_and_comments() {
  for (;;) {
    char c = peek();
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      ++pos_;
    } else if (c == '/' && peek(1) == '/' && peek(2) != '/') {
      while (!at_end() && peek() != '\n') ++pos_;
    } else if (c == '/' && peek(1) == '/' && peek(2) == '/' && peek(3) == '/') {
      // "////" is a plain comment, not a doc comment.
      while (!at_end() && peek() != '\n') ++pos_;
    } else if (c == '/' && peek(1) == '*') {
      skip_block_comment();
    } else {
      return;
    }
  }
}

Token Lexer::lex_doc_comment(uint32_t start) {
  // Collects consecutive "///" lines into one token.
  std::string body;
  for (;;) {
    pos_ += 3;
    if (peek() == ' ') ++pos_;
    uint32_t line_start = pos_;
    while (!at_end() && peek() != '\n') ++pos_;
    body.append(text_.substr(line_start, pos_ - line_start));
    // Look ahead past whitespace for another doc line.
    uint32_t save = pos_;
    while (peek() == '\n' || peek() == '\r' || peek() == ' ' || peek() == '\t') ++pos_;
    if (peek() == '/' && peek(1) == '/' && peek(2) == '/' && peek(3) != '/') {
      body += '\n';
      continue;
    }
    pos_ = save;
    break;
  }
  Token t = make(TokenKind::DocComment, start);
  t.text = std::move(body);
  return t;
}

// ---------- main dispatch ----------

Token Lexer::next() {
  if (in_string_) return lex_in_string();

  skip_whitespace_and_comments();
  uint32_t start = pos_;
  if (at_end()) return make(TokenKind::Eof, start);

  char c = peek();

  if (c == '/' && peek(1) == '/' && peek(2) == '/') return lex_doc_comment(start);
  if (is_ident_start(c)) return lex_identifier(start);
  if (is_digit(c)) return lex_number(start);
  if (c == '\'') return lex_char(start);
  if (c == '"') return lex_string_open(start);

  auto two = [&](char second) { return peek(1) == second; };
  auto emit = [&](TokenKind kind, uint32_t len) {
    pos_ += len;
    return make(kind, start);
  };

  switch (c) {
    case '(': return emit(TokenKind::LParen, 1);
    case ')': return emit(TokenKind::RParen, 1);
    case '[': return emit(TokenKind::LBracket, 1);
    case ']': return emit(TokenKind::RBracket, 1);
    case '{':
      if (!frames_.empty()) ++frames_.back().brace_depth;
      return emit(TokenKind::LBrace, 1);
    case '}':
      if (!frames_.empty()) {
        if (frames_.back().brace_depth == 0) {
          frames_.pop_back();
          in_string_ = true;
          return emit(TokenKind::InterpEnd, 1);
        }
        --frames_.back().brace_depth;
      }
      return emit(TokenKind::RBrace, 1);
    case ',': return emit(TokenKind::Comma, 1);
    case ';': return emit(TokenKind::Semi, 1);
    case ':':
      return two(':') ? emit(TokenKind::ColonColon, 2) : emit(TokenKind::Colon, 1);
    case '.':
      if (two('.')) {
        return peek(2) == '=' ? emit(TokenKind::DotDotEq, 3) : emit(TokenKind::DotDot, 2);
      }
      return emit(TokenKind::Dot, 1);
    case '?':
      if (two('.')) return emit(TokenKind::QuestionDot, 2);
      if (two('?')) return emit(TokenKind::QuestionQuestion, 2);
      return emit(TokenKind::Question, 1);
    case '+': return two('=') ? emit(TokenKind::PlusEq, 2) : emit(TokenKind::Plus, 1);
    case '-':
      if (two('>')) return emit(TokenKind::Arrow, 2);
      return two('=') ? emit(TokenKind::MinusEq, 2) : emit(TokenKind::Minus, 1);
    case '*': return two('=') ? emit(TokenKind::StarEq, 2) : emit(TokenKind::Star, 1);
    case '/': return two('=') ? emit(TokenKind::SlashEq, 2) : emit(TokenKind::Slash, 1);
    case '%': return two('=') ? emit(TokenKind::PercentEq, 2) : emit(TokenKind::Percent, 1);
    case '=':
      if (two('=')) return emit(TokenKind::EqEq, 2);
      if (two('>')) return emit(TokenKind::FatArrow, 2);
      return emit(TokenKind::Eq, 1);
    case '!': return two('=') ? emit(TokenKind::BangEq, 2) : emit(TokenKind::Bang, 1);
    case '<':
      if (two('<')) return peek(2) == '=' ? emit(TokenKind::ShlEq, 3) : emit(TokenKind::Shl, 2);
      return two('=') ? emit(TokenKind::LtEq, 2) : emit(TokenKind::Lt, 1);
    case '>':
      if (two('>') && peek(2) == '=') return emit(TokenKind::ShrEq, 3);
      if (two('=')) return emit(TokenKind::GtEq, 2);
      return emit(TokenKind::Gt, 1);  // ">>" becomes two Gt tokens on purpose
    case '&':
      if (two('&')) return emit(TokenKind::AmpAmp, 2);
      return two('=') ? emit(TokenKind::AmpEq, 2) : emit(TokenKind::Amp, 1);
    case '|':
      if (two('|')) return emit(TokenKind::PipePipe, 2);
      return two('=') ? emit(TokenKind::PipeEq, 2) : emit(TokenKind::Pipe, 1);
    case '^': return two('=') ? emit(TokenKind::CaretEq, 2) : emit(TokenKind::Caret, 1);
    case '~': return emit(TokenKind::Tilde, 1);
    default: break;
  }

  // Unknown byte. Skip one whole UTF-8 character so the error is not repeated.
  uint32_t cp = 0;
  size_t len = decode_utf8(text_.substr(pos_), cp);
  pos_ += static_cast<uint32_t>(len == 0 ? 1 : len);
  return error(start, "unexpected character '" + std::string(text_.substr(start, pos_ - start)) + "'", "SNX0101");
}

// ---------- identifiers ----------

Token Lexer::lex_identifier(uint32_t start) {
  while (is_ident_char(peek())) ++pos_;
  std::string_view word = text_.substr(start, pos_ - start);
  auto it = keyword_table().find(word);
  if (it != keyword_table().end()) return make(it->second, start);
  Token t = make(TokenKind::Ident, start);
  t.text = std::string(word);
  return t;
}

// ---------- numbers ----------

Token Lexer::lex_number(uint32_t start) {
  std::string digits;
  int base = 10;
  bool is_float = false;

  if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
    base = 16;
    pos_ += 2;
    while (is_hex_digit(peek()) || peek() == '_') {
      if (peek() != '_') digits += peek();
      ++pos_;
    }
  } else if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
    base = 2;
    pos_ += 2;
    while (peek() == '0' || peek() == '1' || peek() == '_') {
      if (peek() != '_') digits += peek();
      ++pos_;
    }
  } else {
    while (is_digit(peek()) || peek() == '_') {
      if (peek() != '_') digits += peek();
      ++pos_;
    }
    if (peek() == '.' && is_digit(peek(1))) {
      is_float = true;
      digits += '.';
      ++pos_;
      while (is_digit(peek()) || peek() == '_') {
        if (peek() != '_') digits += peek();
        ++pos_;
      }
    }
    if (peek() == 'e' || peek() == 'E') {
      char sign = peek(1);
      uint32_t after = (sign == '+' || sign == '-') ? 2 : 1;
      if (is_digit(peek(after))) {
        is_float = true;
        digits += 'e';
        if (after == 2) digits += sign;
        pos_ += after;
        while (is_digit(peek())) {
          digits += peek();
          ++pos_;
        }
      }
    }
  }

  if (digits.empty()) {
    return error(start, "number has no digits");
  }

  TokenKind kind;
  if (peek() == 'f' || peek() == 'F') {
    ++pos_;
    kind = TokenKind::FloatLit;
    is_float = true;
  } else if (peek() == 'L') {
    ++pos_;
    if (is_float) return error(start, "L suffix is only valid on an integer literal");
    kind = TokenKind::LongLit;
  } else {
    kind = is_float ? TokenKind::DoubleLit : TokenKind::IntLit;
  }

  if (is_ident_char(peek())) {
    while (is_ident_char(peek())) ++pos_;
    return error(start, "invalid suffix on number literal");
  }

  Token t = make(kind, start);
  t.text = std::string(text_.substr(start, pos_ - start));

  if (is_float) {
    if (base != 10) return error(start, "float literal must be decimal");
    errno = 0;
    t.float_value = std::strtod(digits.c_str(), nullptr);
    if (errno == ERANGE) return error(start, "float literal out of range");
    return t;
  }

  uint64_t value = 0;
  for (char d : digits) {
    uint64_t v = static_cast<uint64_t>(hex_value(d));
    if (value > (UINT64_MAX - v) / static_cast<uint64_t>(base)) {
      return error(start, "integer literal too large");
    }
    value = value * static_cast<uint64_t>(base) + v;
  }
  t.int_value = value;
  return t;
}

// ---------- escapes, chars, strings ----------

// pos_ is at the backslash. Appends the decoded bytes to out.
bool Lexer::lex_escape(std::string& out) {
  uint32_t start = pos_;
  ++pos_;  // backslash
  char c = peek();
  ++pos_;
  switch (c) {
    case 'n': out += '\n'; return true;
    case 't': out += '\t'; return true;
    case 'r': out += '\r'; return true;
    case '0': out += '\0'; return true;
    case '\\': out += '\\'; return true;
    case '\'': out += '\''; return true;
    case '"': out += '"'; return true;
    case '{': out += '{'; return true;
    case '}': out += '}'; return true;
    case 'u': {
      if (peek() != '{') {
        sink_.error(Span{start, pos_, file_.id()}, "expected '{' after \\u");
        return false;
      }
      ++pos_;
      uint32_t cp = 0;
      int count = 0;
      while (is_hex_digit(peek())) {
        cp = (cp << 4) | static_cast<uint32_t>(hex_value(peek()));
        ++pos_;
        if (++count > 6) break;
      }
      if (peek() != '}' || count == 0 || count > 6 || cp > 0x10FFFF ||
          (cp >= 0xD800 && cp <= 0xDFFF)) {
        while (!at_end() && peek() != '}' && peek() != '"' && peek() != '\n') ++pos_;
        if (peek() == '}') ++pos_;
        sink_.error(Span{start, pos_, file_.id()}, "invalid unicode escape");
        return false;
      }
      ++pos_;
      append_utf8(out, cp);
      return true;
    }
    default:
      sink_.error(Span{start, pos_, file_.id()}, "unknown escape sequence");
      return false;
  }
}

Token Lexer::lex_char(uint32_t start) {
  ++pos_;  // opening quote
  std::string decoded;
  if (peek() == '\\') {
    if (!lex_escape(decoded)) {
      while (!at_end() && peek() != '\'' && peek() != '\n') ++pos_;
      if (peek() == '\'') ++pos_;
      return make(TokenKind::Error, start);
    }
  } else if (peek() == '\'' || peek() == '\n' || at_end()) {
    return error(start, "empty char literal");
  } else {
    uint32_t cp = 0;
    size_t len = decode_utf8(text_.substr(pos_), cp);
    if (len == 0) {
      ++pos_;
      return error(start, "invalid UTF-8 in char literal");
    }
    decoded.append(text_.substr(pos_, len));
    pos_ += static_cast<uint32_t>(len);
  }
  if (peek() != '\'') {
    while (!at_end() && peek() != '\'' && peek() != '\n') ++pos_;
    if (peek() == '\'') ++pos_;
    return error(start, "char literal must contain exactly one character");
  }
  ++pos_;
  uint32_t cp = 0;
  decode_utf8(decoded, cp);
  Token t = make(TokenKind::CharLit, start);
  t.int_value = cp;
  t.text = std::move(decoded);
  return t;
}

// Called at the opening quote. Decides between a plain StringLit and the
// interpolated token sequence.
Token Lexer::lex_string_open(uint32_t start) {
  // Scan ahead for an unescaped, undoubled '{'.
  bool interpolated = false;
  for (uint32_t i = pos_ + 1; i < text_.size(); ++i) {
    char c = text_[i];
    if (c == '\\') {
      // Skip the escape. A \u{...} escape contains braces that are not
      // interpolation.
      if (i + 2 < text_.size() && text_[i + 1] == 'u' && text_[i + 2] == '{') {
        i += 2;
        while (i < text_.size() && text_[i] != '}' && text_[i] != '"' && text_[i] != '\n') ++i;
      } else {
        ++i;
      }
      continue;
    }
    if (c == '"' || c == '\n') break;
    if (c == '{') {
      if (i + 1 < text_.size() && text_[i + 1] == '{') { ++i; continue; }
      interpolated = true;
      break;
    }
  }

  if (interpolated) {
    ++pos_;
    in_string_ = true;
    return make(TokenKind::StringStart, start);
  }

  ++pos_;
  std::string decoded;
  bool ok = true;
  for (;;) {
    if (at_end() || peek() == '\n') {
      return error(start, "unterminated string literal", "SNX0102");
    }
    char c = peek();
    if (c == '"') { ++pos_; break; }
    if (c == '\\') { if (!lex_escape(decoded)) ok = false; continue; }
    if (c == '{' && peek(1) == '{') { decoded += '{'; pos_ += 2; continue; }
    if (c == '}' && peek(1) == '}') { decoded += '}'; pos_ += 2; continue; }
    if (c == '}') {
      sink_.error(Span{pos_, pos_ + 1, file_.id()}, "unmatched '}' in string, write '}}' for a literal brace");
      ++pos_;
      ok = false;
      continue;
    }
    decoded += c;
    ++pos_;
  }
  if (!ok) return make(TokenKind::Error, start);
  Token t = make(TokenKind::StringLit, start);
  t.text = std::move(decoded);
  return t;
}

// Lexes the next piece of an interpolated string: text, an interpolation
// start, or the closing quote.
Token Lexer::lex_in_string() {
  uint32_t start = pos_;
  if (at_end() || peek() == '\n') {
    in_string_ = false;
    frames_.clear();
    return error(start, "unterminated string literal", "SNX0102");
  }
  if (peek() == '"') {
    ++pos_;
    in_string_ = false;
    return make(TokenKind::StringEnd, start);
  }
  if (peek() == '{' && peek(1) != '{') {
    ++pos_;
    in_string_ = false;
    frames_.push_back(InterpFrame{});
    return make(TokenKind::InterpStart, start);
  }

  std::string decoded;
  bool ok = true;
  for (;;) {
    if (at_end() || peek() == '\n' || peek() == '"') break;
    char c = peek();
    if (c == '{') {
      if (peek(1) == '{') { decoded += '{'; pos_ += 2; continue; }
      break;
    }
    if (c == '}' && peek(1) == '}') { decoded += '}'; pos_ += 2; continue; }
    if (c == '}') {
      sink_.error(Span{pos_, pos_ + 1, file_.id()}, "unmatched '}' in string, write '}}' for a literal brace");
      ++pos_;
      ok = false;
      continue;
    }
    if (c == '\\') { if (!lex_escape(decoded)) ok = false; continue; }
    decoded += c;
    ++pos_;
  }
  if (!ok) return make(TokenKind::Error, start);
  Token t = make(TokenKind::StringText, start);
  t.text = std::move(decoded);
  return t;
}

}  // namespace synx
