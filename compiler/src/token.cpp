#include "synx/token.hpp"

#include <sstream>

namespace synx {

const char* token_kind_name(TokenKind kind) {
  switch (kind) {
#define X(name, text) \
  case TokenKind::name: return text;
    SYNX_TOKEN_KINDS(X)
#undef X
  }
  return "?";
}

bool is_keyword(TokenKind kind) {
  return kind >= TokenKind::KwAs && kind <= TokenKind::KwVoid;
}

bool is_primitive_type_keyword(TokenKind kind) {
  return kind >= TokenKind::KwBool && kind <= TokenKind::KwVoid;
}

std::string token_to_string(const Token& t) {
  std::ostringstream out;
  out << t.span.start << '-' << t.span.end << ' ';
  switch (t.kind) {
    case TokenKind::Ident:
      out << "Ident " << t.text;
      break;
    case TokenKind::IntLit:
      out << "IntLit " << t.int_value;
      break;
    case TokenKind::LongLit:
      out << "LongLit " << t.int_value;
      break;
    case TokenKind::FloatLit:
      out << "FloatLit " << t.float_value;
      break;
    case TokenKind::DoubleLit:
      out << "DoubleLit " << t.float_value;
      break;
    case TokenKind::CharLit:
      out << "CharLit U+" << std::hex << t.int_value << std::dec;
      break;
    case TokenKind::StringLit:
      out << "StringLit \"" << t.text << '"';
      break;
    case TokenKind::StringText:
      out << "StringText \"" << t.text << '"';
      break;
    case TokenKind::DocComment:
      out << "DocComment \"" << t.text << '"';
      break;
    case TokenKind::Error:
      out << "Error " << t.text;
      break;
    default:
      if (is_keyword(t.kind)) {
        out << "Kw " << token_kind_name(t.kind);
      } else {
        out << token_kind_name(t.kind);
      }
      break;
  }
  return out.str();
}

}  // namespace synx
