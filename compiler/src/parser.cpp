#include "synx/parser.hpp"

#include <utility>

namespace synx {

using namespace ast;
using K = TokenKind;

Parser::Parser(const SourceFile& file, std::vector<Token> tokens, DiagnosticSink& sink)
    : file_(file), tokens_(std::move(tokens)), sink_(sink) {
  if (tokens_.empty() || tokens_.back().kind != K::Eof) {
    Token eof;
    eof.kind = K::Eof;
    eof.span = Span{static_cast<uint32_t>(file.text().size()), static_cast<uint32_t>(file.text().size()), file.id()};
    tokens_.push_back(eof);
  }
}

// ---------- token access ----------

const Token& Parser::peek(size_t ahead) const {
  size_t i = pos_ + ahead;
  return i < tokens_.size() ? tokens_[i] : tokens_.back();
}

const Token& Parser::advance() {
  const Token& t = tokens_[pos_];
  if (pos_ + 1 < tokens_.size()) ++pos_;
  return t;
}

bool Parser::accept(K k) {
  if (!at(k)) return false;
  advance();
  return true;
}

void Parser::error(Span span, std::string message, const char* code) {
  if (speculating_ > 0) {
    ++spec_errors_;
    return;
  }
  sink_.error(span, std::move(message), code);
}

void Parser::error_here(std::string message, const char* code) { error(peek().span, std::move(message), code); }

bool Parser::expect(K k, const char* what) {
  if (accept(k)) return true;
  std::string msg = "expected ";
  msg += what;
  msg += ", found ";
  msg += token_kind_name(peek().kind);
  error_here(msg, "SNX0201");
  return false;
}

std::string Parser::expect_ident(const char* what, Span* span_out) {
  if (at(K::Ident)) {
    const Token& t = advance();
    if (span_out) *span_out = t.span;
    return t.text;
  }
  std::string msg = "expected ";
  msg += what;
  msg += ", found ";
  msg += token_kind_name(peek().kind);
  error_here(msg, "SNX0201");
  if (span_out) *span_out = peek().span;
  return "";
}

Span Parser::span_from(Span start) const {
  uint32_t end = pos_ > 0 ? tokens_[pos_ - 1].span.end : start.end;
  if (end < start.start) end = start.end;
  return Span{start.start, end, start.file};
}

// ---------- lookahead ----------

size_t Parser::skip_generic_list(size_t i) const {
  if (i >= tokens_.size() || tokens_[i].kind != K::Lt) return 0;
  int depth = 0;
  for (; i < tokens_.size(); ++i) {
    K k = tokens_[i].kind;
    if (k == K::Lt) {
      ++depth;
    } else if (k == K::Gt) {
      if (--depth == 0) return i + 1;
    } else if (k == K::LParen) {
      // Function types, tuple types, and grouped reference types can occur
      // inside explicit arguments. The speculative type parser validates
      // their contents after this balanced lookahead.
      size_t after = skip_paren_group(i);
      if (after == 0) return 0;
      i = after - 1;
    } else if (k == K::Eof || k == K::LBrace || k == K::RBrace || k == K::Semi ||
               k == K::RParen || k == K::Eq) {
      return 0;
    }
  }
  return 0;
}

size_t Parser::skip_paren_group(size_t i) const {
  if (i >= tokens_.size() || tokens_[i].kind != K::LParen) return 0;
  int depth = 0;
  for (; i < tokens_.size(); ++i) {
    K k = tokens_[i].kind;
    if (k == K::LParen) ++depth;
    else if (k == K::RParen && --depth == 0) return i + 1;
    else if (k == K::Eof) return 0;
  }
  return 0;
}

bool Parser::looks_like_function_name() const {
  if (!at(K::Ident)) return false;
  size_t i = pos_ + 1;
  if (i < tokens_.size() && tokens_[i].kind == K::Lt) {
    i = skip_generic_list(i);
    if (i == 0) return false;
  }
  return i < tokens_.size() && tokens_[i].kind == K::LParen;
}

bool Parser::looks_like_var_decl() {
  K first = peek().kind;
  if (first == K::KwVar || first == K::KwConst) return true;
  if (!(first == K::Ident || first == K::Amp || first == K::Star || first == K::LParen ||
        first == K::KwFn || first == K::KwDyn || first == K::KwSelfType ||
        is_primitive_type_keyword(first))) {
    return false;
  }
  size_t save = pos_;
  int save_errors = spec_errors_;
  ++speculating_;
  spec_errors_ = 0;
  TypePtr t = parse_type();
  bool ok = spec_errors_ == 0 && t && at(K::Ident) &&
            (peek(1).kind == K::Eq || peek(1).kind == K::Semi);
  --speculating_;
  spec_errors_ = save_errors;
  pos_ = save;
  return ok;
}

// ---------- module and items ----------

Module Parser::parse_module() {
  Module m;
  while (!at_end()) {
    size_t before = pos_;
    size_t errors_before = sink_.error_count();
    ItemPtr item = parse_item();
    if (item) m.items.push_back(std::move(item));
    if (sink_.error_count() > errors_before) {
      // Statement level recovery usually leaves us at the next item. Only
      // skip ahead when the next token cannot start one.
      K k = peek().kind;
      bool item_start = k == K::Ident || k == K::KwPub || k == K::DocComment ||
                        k == K::KwImport || k == K::KwConst || k == K::KwStruct ||
                        k == K::KwEnum || k == K::KwTrait || k == K::KwImpl ||
                        k == K::KwExtern || k == K::KwAsync || k == K::Amp ||
                        k == K::Star || k == K::LParen || k == K::Eof ||
                        is_primitive_type_keyword(k);
      if (!item_start) sync_item();
    }
    if (pos_ == before) advance();  // always make progress
  }
  return m;
}

void Parser::sync_item() {
  // Skip to a token that can start an item at nesting depth zero.
  int depth = 0;
  while (!at_end()) {
    K k = peek().kind;
    if (k == K::LBrace) ++depth;
    if (k == K::RBrace) {
      if (depth > 0) --depth;
      advance();
      if (depth == 0) return;
      continue;
    }
    if (depth == 0 && (k == K::KwImport || k == K::KwStruct || k == K::KwEnum ||
                       k == K::KwTrait || k == K::KwImpl || k == K::KwExtern ||
                       k == K::KwPub || k == K::DocComment)) {
      return;
    }
    advance();
  }
}

ItemPtr Parser::parse_item() {
  std::string doc;
  if (at(K::DocComment)) doc = advance().text;
  Span start = peek().span;
  bool is_pub = accept(K::KwPub);
  ItemPtr item;
  switch (peek().kind) {
    case K::KwImport: item = parse_import(start); break;
    case K::KwConst: item = parse_const_item(start); break;
    case K::KwStruct: item = parse_struct(start); break;
    case K::KwEnum: item = parse_enum(start); break;
    case K::KwTrait: item = parse_trait(start); break;
    case K::KwImpl: item = parse_impl(start); break;
    case K::KwUnsafe: {
      advance();
      if (!at(K::KwImpl)) { error_here("expected impl after unsafe"); return nullptr; }
      item = parse_impl(start);
      static_cast<ImplItem&>(*item).is_unsafe = true;
      break;
    }
    case K::KwExtern: item = parse_extern(start); break;
    default: {
      FunctionPtr fn = parse_function(FnContext::Item, doc, is_pub);
      if (!fn) return nullptr;
      Span s = fn->span;
      item = std::make_unique<FunctionItem>(s, std::move(fn));
      break;
    }
  }
  if (item) {
    item->doc = std::move(doc);
    item->is_pub = is_pub;
  }
  return item;
}

ItemPtr Parser::parse_import(Span start) {
  advance();  // import
  auto item = std::make_unique<ImportItem>(start);
  Span seg_span;
  std::string first = expect_ident("module name", &seg_span);
  item->path.segments.push_back({first, seg_span});
  while (accept(K::Dot)) {
    if (accept(K::LBrace)) {
      do {
        if (at(K::RBrace)) break;
        Span s;
        std::string n = expect_ident("imported name", &s);
        item->names.push_back({n, s});
      } while (accept(K::Comma));
      expect(K::RBrace, "'}'");
      break;
    }
    std::string n = expect_ident("module or item name", &seg_span);
    item->path.segments.push_back({n, seg_span});
  }
  item->path.span = span_from(start);
  expect(K::Semi, "';'");
  item->span = span_from(start);
  return item;
}

ItemPtr Parser::parse_const_item(Span start) {
  advance();  // const
  auto item = std::make_unique<ConstItem>(start);
  if (!(at(K::Ident) && peek(1).kind == K::Eq)) item->type = parse_type();
  item->name = expect_ident("constant name");
  expect(K::Eq, "'='");
  item->value = parse_expr();
  expect(K::Semi, "';'");
  item->span = span_from(start);
  return item;
}

std::vector<GenericParam> Parser::parse_generic_params() {
  std::vector<GenericParam> out;
  expect(K::Lt, "'<'");
  while (!at(K::Gt) && !at_end()) {
    GenericParam g;
    g.name = expect_ident("type parameter name", &g.span);
    if (accept(K::Colon)) {
      do {
        g.bounds.push_back(parse_bound());
      } while (accept(K::Plus));
    }
    out.push_back(std::move(g));
    if (!accept(K::Comma)) break;
  }
  expect(K::Gt, "'>'");
  return out;
}

Bound Parser::parse_bound() {
  Bound b;
  Span start = peek().span;
  b.path = parse_path();
  if (at(K::Lt)) b.args = parse_type_args();
  b.span = span_from(start);
  return b;
}

std::vector<Param> Parser::parse_params(FnContext ctx) {
  std::vector<Param> out;
  expect(K::LParen, "'('");
  while (!at(K::RParen) && !at_end()) {
    Param p;
    Span start = peek().span;
    if (at(K::KwSelf)) {
      advance();
      p.self_kind = SelfKind::Value;
      p.name = "self";
    } else if (at(K::Amp) && (peek(1).kind == K::KwSelf ||
                              (peek(1).kind == K::KwConst && peek(2).kind == K::KwSelf))) {
      advance();
      p.self_kind = accept(K::KwConst) ? SelfKind::ConstRef : SelfKind::Ref;
      advance();  // self
      p.name = "self";
    } else {
      p.type = parse_type();
      p.name = expect_ident("parameter name");
    }
    p.span = span_from(start);
    if (p.self_kind != SelfKind::None) {
      if (ctx == FnContext::Item || ctx == FnContext::Extern) {
        error(p.span, "self parameter is only allowed in a method");
      } else if (!out.empty()) {
        error(p.span, "self must be the first parameter");
      }
    }
    out.push_back(std::move(p));
    if (!accept(K::Comma)) break;
  }
  expect(K::RParen, "')'");
  return out;
}

FunctionPtr Parser::parse_function(FnContext ctx, std::string doc, bool is_pub) {
  Span start = peek().span;
  bool is_async = accept(K::KwAsync);
  bool async_send = false;
  if (is_async && at(K::Ident) && peek().text == "send" && !looks_like_function_name()) { advance(); async_send = true; }
  TypePtr ret;
  if (!looks_like_function_name()) {
    if (!(at(K::Ident) || at(K::Amp) || at(K::Star) || at(K::LParen) || at(K::KwFn) ||
          at(K::KwDyn) || at(K::KwSelfType) || is_primitive_type_keyword(peek().kind))) {
      error_here(std::string("expected an item, found ") + token_kind_name(peek().kind), "SNX0201");
      return nullptr;
    }
    ret = parse_type();
  }
  Span name_span;
  std::string name = expect_ident("function name", &name_span);
  auto fn = parse_function_after_head(ctx, std::move(doc), is_pub, is_async, start,
                                   std::move(ret), std::move(name), name_span);
  fn->async_send = async_send;
  return fn;
}

FunctionPtr Parser::parse_function_after_head(FnContext ctx, std::string doc, bool is_pub,
                                              bool is_async, Span start, TypePtr ret,
                                              std::string name, Span name_span) {
  auto fn = std::make_unique<Function>();
  fn->doc = std::move(doc);
  fn->is_pub = is_pub;
  fn->is_async = is_async;
  fn->ret = std::move(ret);
  fn->name = std::move(name);
  fn->name_span = name_span;
  if (at(K::Lt)) fn->generics = parse_generic_params();
  fn->params = parse_params(ctx);
  if (at(K::LBrace)) {
    if (ctx == FnContext::Extern) error_here("extern functions have no body");
    fn->body = parse_block();
  } else {
    if (ctx == FnContext::TraitMethod || ctx == FnContext::Extern) {
      expect(K::Semi, "';' or a function body");
    } else {
      expect(K::LBrace, "function body");
    }
  }
  fn->span = span_from(start);
  return fn;
}

bool Parser::parse_member(FnContext ctx, std::vector<Field>* fields,
                          std::vector<FunctionPtr>& methods) {
  std::string doc;
  if (at(K::DocComment)) doc = advance().text;
  Span start = peek().span;
  bool is_pub = accept(K::KwPub);
  bool is_async = accept(K::KwAsync);
  bool async_send = false;
  if (is_async && at(K::Ident) && peek().text == "send" && !looks_like_function_name()) { advance(); async_send = true; }

  if (looks_like_function_name()) {
    Span name_span;
    std::string name = expect_ident("name", &name_span);
    FunctionPtr fn = parse_function_after_head(ctx, doc, is_pub, is_async, start, nullptr,
                                               std::move(name), name_span);
    if (fn) { fn->async_send = async_send; methods.push_back(std::move(fn)); }
    return true;
  }

  if (!(at(K::Ident) || at(K::Amp) || at(K::Star) || at(K::LParen) || at(K::KwFn) ||
        at(K::KwDyn) || at(K::KwSelfType) || is_primitive_type_keyword(peek().kind))) {
    error_here(std::string("expected a field or method, found ") + token_kind_name(peek().kind), "SNX0201");
    return false;
  }

  TypePtr type = parse_type();
  Span name_span;
  std::string name = expect_ident("name", &name_span);
  if (at(K::LParen) || at(K::Lt)) {
    FunctionPtr fn = parse_function_after_head(ctx, doc, is_pub, is_async, start,
                                               std::move(type), std::move(name), name_span);
    if (fn) { fn->async_send = async_send; methods.push_back(std::move(fn)); }
    return true;
  }
  if (!fields) {
    error(span_from(start), "fields are not allowed here");
    accept(K::Semi);
    return true;
  }
  Field f;
  f.doc = std::move(doc);
  f.is_pub = is_pub;
  f.type = std::move(type);
  f.name = std::move(name);
  expect(K::Semi, "';' after field");
  f.span = span_from(start);
  fields->push_back(std::move(f));
  return true;
}

ItemPtr Parser::parse_struct(Span start) {
  advance();  // struct
  auto item = std::make_unique<StructItem>(start);
  item->name = expect_ident("struct name");
  if (at(K::Lt)) item->generics = parse_generic_params();
  expect(K::LBrace, "'{'");
  while (!at(K::RBrace) && !at_end()) {
    size_t before = pos_;
    if (!parse_member(FnContext::Method, &item->fields, item->methods) || pos_ == before) advance();
  }
  expect(K::RBrace, "'}'");
  item->span = span_from(start);
  return item;
}

ItemPtr Parser::parse_enum(Span start) {
  advance();  // enum
  auto item = std::make_unique<EnumItem>(start);
  item->name = expect_ident("enum name");
  if (at(K::Lt)) item->generics = parse_generic_params();
  expect(K::LBrace, "'{'");
  while (!at(K::RBrace) && !at_end()) {
    size_t before = pos_;
    // Decide between a variant and a method.
    bool is_variant = false;
    size_t i = pos_;
    if (tokens_[i].kind == K::DocComment) ++i;
    if (tokens_[i].kind == K::Ident) {
      K next = tokens_[i + 1].kind;
      if (next == K::Comma || next == K::RBrace || next == K::LBrace) {
        is_variant = true;
      } else if (next == K::LParen) {
        size_t after = skip_paren_group(i + 1);
        is_variant = after != 0 && tokens_[after].kind != K::LBrace;
      }
    }
    if (is_variant) {
      Variant v;
      Span vstart = peek().span;
      if (at(K::DocComment)) v.doc = advance().text;
      v.name = expect_ident("variant name");
      if (accept(K::LParen)) {
        v.kind = VariantKind::Tuple;
        while (!at(K::RParen) && !at_end()) {
          v.tuple_types.push_back(parse_type());
          if (!accept(K::Comma)) break;
        }
        expect(K::RParen, "')'");
      } else if (accept(K::LBrace)) {
        v.kind = VariantKind::Struct;
        while (!at(K::RBrace) && !at_end()) {
          size_t field_before = pos_;
          Field f;
          Span fstart = peek().span;
          f.type = parse_type();
          f.name = expect_ident("field name");
          expect(K::Semi, "';' after field");
          f.span = span_from(fstart);
          v.fields.push_back(std::move(f));
          if (pos_ == field_before) advance();  // always make progress
        }
        expect(K::RBrace, "'}'");
      }
      v.span = span_from(vstart);
      item->variants.push_back(std::move(v));
      if (!accept(K::Comma) && !at(K::RBrace)) {
        // A method may follow a variant without a comma only after a block.
        if (item->variants.back().kind != VariantKind::Struct) expect(K::Comma, "',' after variant");
      }
    } else {
      parse_member(FnContext::Method, nullptr, item->methods);
    }
    if (pos_ == before) advance();
  }
  expect(K::RBrace, "'}'");
  item->span = span_from(start);
  return item;
}

ItemPtr Parser::parse_trait(Span start) {
  advance();  // trait
  auto item = std::make_unique<TraitItem>(start);
  item->name = expect_ident("trait name");
  if (at(K::Lt)) item->generics = parse_generic_params();
  expect(K::LBrace, "'{'");
  while (!at(K::RBrace) && !at_end()) {
    size_t before = pos_;
    if (!parse_member(FnContext::TraitMethod, nullptr, item->methods) || pos_ == before) advance();
  }
  expect(K::RBrace, "'}'");
  item->span = span_from(start);
  return item;
}

ItemPtr Parser::parse_impl(Span start) {
  advance();  // impl
  auto item = std::make_unique<ImplItem>(start);
  if (at(K::Lt)) item->generics = parse_generic_params();
  item->trait = parse_bound();
  if (!accept(K::KwFor)) {
    // Allow "for" to be an identifier mistake; report clearly.
    error_here("expected 'for' after trait name in impl");
  }
  item->target = parse_type();
  expect(K::LBrace, "'{'");
  while (!at(K::RBrace) && !at_end()) {
    size_t before = pos_;
    if (!parse_member(FnContext::Method, nullptr, item->methods) || pos_ == before) advance();
  }
  expect(K::RBrace, "'}'");
  item->span = span_from(start);
  return item;
}

ItemPtr Parser::parse_extern(Span start) {
  advance();  // extern
  auto item = std::make_unique<ExternItem>(start);
  if (at(K::StringLit)) {
    item->abi = advance().text;
  } else {
    error_here("expected ABI string after extern, for example \"C\"");
  }
  expect(K::LBrace, "'{'");
  while (!at(K::RBrace) && !at_end()) {
    size_t before = pos_;
    if (!parse_member(FnContext::Extern, nullptr, item->functions) || pos_ == before) advance();
  }
  expect(K::RBrace, "'}'");
  item->span = span_from(start);
  return item;
}

// ---------- types ----------

Path Parser::parse_path() {
  Path p;
  Span start = peek().span;
  Span s;
  std::string first = expect_ident("name", &s);
  p.segments.push_back({first, s});
  while (at(K::Dot) && peek(1).kind == K::Ident) {
    advance();
    const Token& t = advance();
    p.segments.push_back({t.text, t.span});
  }
  p.span = span_from(start);
  return p;
}

std::vector<TypePtr> Parser::parse_type_args() {
  std::vector<TypePtr> out;
  expect(K::Lt, "'<'");
  while (!at(K::Gt) && !at_end()) {
    out.push_back(parse_type());
    if (!accept(K::Comma)) break;
  }
  expect(K::Gt, "'>'");
  return out;
}

// "?" applies to the whole type in front of it, including a reference:
// "&const str?" is Option<&const str>. Array and slice suffixes bind tighter
// than the reference: "&const T[]" is a reference to a slice.
TypePtr Parser::parse_type() {
  Span start = peek().span;
  TypePtr t;
  if (accept(K::Amp)) {
    bool is_const = accept(K::KwConst);
    TypePtr inner = parse_postfix_type();
    t = std::make_unique<RefType>(span_from(start), is_const, std::move(inner));
  } else if (accept(K::Star)) {
    bool is_const = accept(K::KwConst);
    TypePtr inner = parse_postfix_type();
    t = std::make_unique<PtrType>(span_from(start), is_const, std::move(inner));
  } else {
    t = parse_postfix_type();
  }
  while (at(K::Question)) {
    advance();
    t = std::make_unique<OptionalType>(span_from(start), std::move(t));
  }
  return t;
}

TypePtr Parser::parse_postfix_type() {
  Span start = peek().span;
  TypePtr t = parse_base_type();
  while (at(K::LBracket)) {
    advance();
    if (accept(K::RBracket)) {
      t = std::make_unique<SliceType>(span_from(start), std::move(t));
    } else {
      ExprPtr size = parse_expr();
      expect(K::RBracket, "']'");
      t = std::make_unique<ArrayType>(span_from(start), std::move(t), std::move(size));
    }
  }
  return t;
}

TypePtr Parser::parse_base_type() {
  Span start = peek().span;
  const Token& t = peek();
  switch (t.kind) {
    case K::KwBool: advance(); return std::make_unique<PrimitiveType>(t.span, PrimKind::Bool);
    case K::KwByte: advance(); return std::make_unique<PrimitiveType>(t.span, PrimKind::Byte);
    case K::KwShort: advance(); return std::make_unique<PrimitiveType>(t.span, PrimKind::Short);
    case K::KwInt: advance(); return std::make_unique<PrimitiveType>(t.span, PrimKind::Int);
    case K::KwLong: advance(); return std::make_unique<PrimitiveType>(t.span, PrimKind::Long);
    case K::KwUint: advance(); return std::make_unique<PrimitiveType>(t.span, PrimKind::Uint);
    case K::KwUlong: advance(); return std::make_unique<PrimitiveType>(t.span, PrimKind::Ulong);
    case K::KwUsize: advance(); return std::make_unique<PrimitiveType>(t.span, PrimKind::Usize);
    case K::KwFloat: advance(); return std::make_unique<PrimitiveType>(t.span, PrimKind::Float);
    case K::KwDouble: advance(); return std::make_unique<PrimitiveType>(t.span, PrimKind::Double);
    case K::KwChar: advance(); return std::make_unique<PrimitiveType>(t.span, PrimKind::Char);
    case K::KwVoid: advance(); return std::make_unique<PrimitiveType>(t.span, PrimKind::Void);
    case K::KwSelfType: advance(); return std::make_unique<SelfTypeRef>(t.span);
    case K::KwDyn: {
      advance();
      auto d = std::make_unique<DynType>(start);
      d->bound = parse_bound();
      d->span = span_from(start);
      return d;
    }
    case K::KwFn: {
      advance();
      auto f = std::make_unique<FnType>(start);
      if (accept(K::KwConst)) f->call_mode = CallMode::Shared;
      else if (accept(K::KwOnce)) f->call_mode = CallMode::Once;
      if (at(K::Ident) && peek().text == "send" && peek(1).kind == K::LParen) { advance(); f->is_send = true; }
      expect(K::LParen, "'('");
      while (!at(K::RParen) && !at_end()) {
        f->params.push_back(parse_type());
        if (!accept(K::Comma)) break;
      }
      expect(K::RParen, "')'");
      if (accept(K::Arrow)) f->ret = parse_type();
      f->span = span_from(start);
      return f;
    }
    case K::LParen: {
      advance();
      auto tt = std::make_unique<TupleType>(start);
      bool trailing_comma = false;
      while (!at(K::RParen) && !at_end()) {
        tt->elems.push_back(parse_type());
        trailing_comma = accept(K::Comma);
        if (!trailing_comma) break;
      }
      expect(K::RParen, "')'");
      // "(T)" is just T in parentheses; "(T,)" is a one element tuple.
      if (tt->elems.size() == 1 && !trailing_comma) return std::move(tt->elems[0]);
      tt->span = span_from(start);
      return tt;
    }
    case K::Ident: {
      auto p = std::make_unique<PathType>(start);
      p->path = parse_path();
      if (at(K::Lt)) p->args = parse_type_args();
      p->span = span_from(start);
      return p;
    }
    default:
      error_here(std::string("expected a type, found ") + token_kind_name(t.kind), "SNX0201");
      // Return a placeholder so callers can continue.
      return std::make_unique<PrimitiveType>(t.span, PrimKind::Void);
  }
}

// ---------- statements ----------

BlockPtr Parser::parse_block() {
  auto block = std::make_unique<Block>();
  Span start = peek().span;
  expect(K::LBrace, "'{'");
  parse_block_body(*block);
  expect(K::RBrace, "'}'");
  block->span = span_from(start);
  return block;
}

bool Parser::is_block_like(const Expr& e) const {
  return e.kind == ExprKind::If || e.kind == ExprKind::Match || e.kind == ExprKind::Block ||
         e.kind == ExprKind::Unsafe;
}

void Parser::parse_block_body(Block& block) {
  while (!at(K::RBrace) && !at_end()) {
    size_t before = pos_;
    size_t errors_before = sink_.error_count();
    Span start = peek().span;

    if (accept(K::Semi)) continue;

    if (at(K::KwWhile)) {
      advance();
      auto s = std::make_unique<WhileStmt>(start);
      expect(K::LParen, "'(' after while");
      s->cond = parse_expr();
      expect(K::RParen, "')'");
      s->body = parse_block();
      s->span = span_from(start);
      block.stmts.push_back(std::move(s));
    } else if (at(K::KwLoop)) {
      advance();
      auto s = std::make_unique<LoopStmt>(start);
      s->body = parse_block();
      s->span = span_from(start);
      block.stmts.push_back(std::move(s));
    } else if (at(K::KwFor)) {
      advance();
      auto s = std::make_unique<ForStmt>(start);
      expect(K::LParen, "'(' after for");
      s->pattern = parse_pattern();
      expect(K::KwIn, "'in'");
      s->iter = parse_expr();
      expect(K::RParen, "')'");
      s->body = parse_block();
      s->span = span_from(start);
      block.stmts.push_back(std::move(s));
    } else if (looks_like_var_decl()) {
      block.stmts.push_back(parse_var_decl());
    } else {
      ExprPtr e = parse_expr();
      AssignOp op;
      bool is_assign = true;
      switch (peek().kind) {
        case K::Eq: op = AssignOp::Assign; break;
        case K::PlusEq: op = AssignOp::Add; break;
        case K::MinusEq: op = AssignOp::Sub; break;
        case K::StarEq: op = AssignOp::Mul; break;
        case K::SlashEq: op = AssignOp::Div; break;
        case K::PercentEq: op = AssignOp::Rem; break;
        case K::AmpEq: op = AssignOp::BitAnd; break;
        case K::PipeEq: op = AssignOp::BitOr; break;
        case K::CaretEq: op = AssignOp::BitXor; break;
        case K::ShlEq: op = AssignOp::Shl; break;
        case K::ShrEq: op = AssignOp::Shr; break;
        default: is_assign = false; op = AssignOp::Assign; break;
      }
      if (is_assign) {
        advance();
        ExprPtr value = parse_expr();
        expect(K::Semi, "';' after assignment");
        block.stmts.push_back(
            std::make_unique<AssignStmt>(span_from(start), op, std::move(e), std::move(value)));
      } else if (accept(K::Semi)) {
        block.stmts.push_back(std::make_unique<ExprStmt>(span_from(start), std::move(e), true));
      } else if (at(K::RBrace)) {
        block.tail = std::move(e);
      } else if (is_block_like(*e)) {
        block.stmts.push_back(std::make_unique<ExprStmt>(span_from(start), std::move(e), false));
      } else {
        expect(K::Semi, "';' after expression");
        block.stmts.push_back(std::make_unique<ExprStmt>(span_from(start), std::move(e), false));
      }
    }

    if (sink_.error_count() > errors_before) {
      // Recover: skip to the end of this statement.
      while (!at_end() && !at(K::Semi) && !at(K::RBrace)) advance();
      accept(K::Semi);
    }
    if (pos_ == before) advance();
  }
}

StmtPtr Parser::parse_var_decl() {
  Span start = peek().span;
  auto s = std::make_unique<VarDeclStmt>(start);
  if (accept(K::KwVar)) {
    // inferred, mutable
  } else if (accept(K::KwConst)) {
    s->is_const = true;
    if (!(at(K::Ident) && (peek(1).kind == K::Eq || peek(1).kind == K::Semi))) s->type = parse_type();
  } else {
    s->type = parse_type();
  }
  s->name = expect_ident("variable name", &s->name_span);
  if (accept(K::Eq)) {
    s->init = parse_expr();
  } else if (!s->type) {
    error(s->name_span, "a variable without a type needs an initializer");
  }
  expect(K::Semi, "';' after declaration");
  s->span = span_from(start);
  return s;
}

// ---------- expressions ----------

ExprPtr Parser::parse_expr() { return parse_range(); }

ExprPtr Parser::parse_expr_no_struct() {
  ++no_struct_lit_;
  ExprPtr e = parse_range();
  --no_struct_lit_;
  return e;
}

bool Parser::at_range_end() const {
  K k = peek().kind;
  return k == K::RParen || k == K::RBracket || k == K::RBrace || k == K::Semi ||
         k == K::Comma || k == K::Eof || k == K::FatArrow || k == K::LBrace;
}

ExprPtr Parser::parse_range() {
  Span start = peek().span;
  ExprPtr lhs;
  if (!(at(K::DotDot) || at(K::DotDotEq))) lhs = parse_binary(1);
  if (at(K::DotDot) || at(K::DotDotEq)) {
    bool inclusive = advance().kind == K::DotDotEq;
    ExprPtr rhs;
    if (!at_range_end()) rhs = parse_binary(1);
    if (inclusive && !rhs) error_here("inclusive range needs an end value");
    return std::make_unique<RangeExpr>(span_from(start), std::move(lhs), std::move(rhs), inclusive);
  }
  return lhs;
}

// Returns precedence (higher binds tighter) or 0 if not a binary operator.
int Parser::binary_prec(BinaryOp& op, size_t& len) const {
  len = 1;
  switch (peek().kind) {
    case K::PipePipe: op = BinaryOp::Or; return 1;
    case K::AmpAmp: op = BinaryOp::And; return 2;
    case K::EqEq: op = BinaryOp::Eq; return 3;
    case K::BangEq: op = BinaryOp::Ne; return 3;
    case K::Lt: op = BinaryOp::Lt; return 3;
    case K::LtEq: op = BinaryOp::Le; return 3;
    case K::GtEq: op = BinaryOp::Ge; return 3;
    case K::Gt:
      if (peek(1).kind == K::Gt && peek(1).span.start == peek().span.end) {
        op = BinaryOp::Shr;
        len = 2;
        return 7;
      }
      op = BinaryOp::Gt;
      return 3;
    case K::Pipe: op = BinaryOp::BitOr; return 4;
    case K::Caret: op = BinaryOp::BitXor; return 5;
    case K::Amp: op = BinaryOp::BitAnd; return 6;
    case K::Shl: op = BinaryOp::Shl; return 7;
    case K::Plus: op = BinaryOp::Add; return 8;
    case K::Minus: op = BinaryOp::Sub; return 8;
    case K::Star: op = BinaryOp::Mul; return 9;
    case K::Slash: op = BinaryOp::Div; return 9;
    case K::Percent: op = BinaryOp::Rem; return 9;
    default: return 0;
  }
}

ExprPtr Parser::parse_binary(int min_prec) {
  Span start = peek().span;
  ExprPtr lhs = parse_cast();
  for (;;) {
    BinaryOp op;
    size_t len;
    int prec = binary_prec(op, len);
    if (prec == 0 || prec < min_prec) break;
    for (size_t i = 0; i < len; ++i) advance();
    ExprPtr rhs = parse_binary(prec + 1);
    lhs = std::make_unique<BinaryExpr>(span_from(start), op, std::move(lhs), std::move(rhs));
  }
  return lhs;
}

ExprPtr Parser::parse_cast() {
  Span start = peek().span;
  ExprPtr e = parse_unary();
  while (accept(K::KwAs)) {
    TypePtr t = parse_type();
    e = std::make_unique<CastExpr>(span_from(start), std::move(e), std::move(t));
  }
  return e;
}

ExprPtr Parser::parse_unary() {
  Span start = peek().span;
  switch (peek().kind) {
    case K::Minus: {
      advance();
      ExprPtr e = parse_unary();
      return std::make_unique<UnaryExpr>(span_from(start), UnaryOp::Neg, std::move(e));
    }
    case K::Bang: {
      advance();
      ExprPtr e = parse_unary();
      return std::make_unique<UnaryExpr>(span_from(start), UnaryOp::Not, std::move(e));
    }
    case K::Tilde: {
      advance();
      ExprPtr e = parse_unary();
      return std::make_unique<UnaryExpr>(span_from(start), UnaryOp::BitNot, std::move(e));
    }
    case K::Star: {
      advance();
      ExprPtr e = parse_unary();
      return std::make_unique<UnaryExpr>(span_from(start), UnaryOp::Deref, std::move(e));
    }
    case K::Amp: {
      advance();
      bool is_const = accept(K::KwConst);
      ExprPtr e = parse_unary();
      return std::make_unique<RefExpr>(span_from(start), is_const, std::move(e));
    }
    default:
      return parse_postfix();
  }
}

std::vector<ExprPtr> Parser::parse_call_args() {
  std::vector<ExprPtr> args;
  expect(K::LParen, "'('");
  int saved = no_struct_lit_;
  no_struct_lit_ = 0;
  while (!at(K::RParen) && !at_end()) {
    args.push_back(parse_expr());
    if (!accept(K::Comma)) break;
  }
  no_struct_lit_ = saved;
  expect(K::RParen, "')'");
  return args;
}

bool Parser::expr_is_path(const Expr& e, Path& out) const {
  if (e.kind == ExprKind::Name) {
    auto& n = static_cast<const NameExpr&>(e);
    if (!n.type_args.empty()) return false;
    out.segments.push_back({n.name, n.span});
    out.span = n.span;
    return true;
  }
  if (e.kind == ExprKind::Field) {
    auto& f = static_cast<const FieldExpr&>(e);
    if (!f.type_args.empty()) return false;
    if (!expr_is_path(*f.base, out)) return false;
    out.segments.push_back({f.name, f.name_span});
    out.span = Span::merge(out.span, f.name_span);
    return true;
  }
  return false;
}

// At "<" after a name. Succeeds only if a type list closes and is followed by
// "(" or ".", which is the rule from the design document.
bool Parser::try_parse_type_args_for_call(std::vector<TypePtr>& out) {
  size_t after = skip_generic_list(pos_);
  if (after == 0) return false;
  K next = tokens_[after].kind;
  if (next != K::LParen && next != K::Dot) return false;
  size_t save = pos_;
  int save_errors = spec_errors_;
  ++speculating_;
  spec_errors_ = 0;
  std::vector<TypePtr> args = parse_type_args();
  bool ok = spec_errors_ == 0;
  --speculating_;
  spec_errors_ = save_errors;
  if (!ok) {
    pos_ = save;
    return false;
  }
  out = std::move(args);
  return true;
}

ExprPtr Parser::parse_postfix() {
  Span start = peek().span;
  ExprPtr e = parse_primary();
  if (!e) return nullptr;
  for (;;) {
    if (at(K::LParen)) {
      auto call = std::make_unique<CallExpr>(start, std::move(e));
      call->args = parse_call_args();
      call->span = span_from(start);
      e = std::move(call);
    } else if (at(K::LBracket)) {
      advance();
      int saved = no_struct_lit_;
      no_struct_lit_ = 0;
      ExprPtr index = parse_expr();
      no_struct_lit_ = saved;
      expect(K::RBracket, "']'");
      e = std::make_unique<IndexExpr>(span_from(start), std::move(e), std::move(index));
    } else if (at(K::Dot)) {
      advance();
      if (at(K::IntLit)) {
        const Token& t = advance();
        e = std::make_unique<FieldExpr>(span_from(start), std::move(e), std::to_string(t.int_value), t.span);
      } else {
        Span ns;
        std::string name = expect_ident("field or method name", &ns);
        auto f = std::make_unique<FieldExpr>(start, std::move(e), std::move(name), ns);
        if (at(K::Lt)) try_parse_type_args_for_call(f->type_args);
        f->span = span_from(start);
        e = std::move(f);
      }
    } else if (at(K::Lt) && e->kind == ExprKind::Name &&
               static_cast<NameExpr&>(*e).type_args.empty()) {
      std::vector<TypePtr> args;
      if (!try_parse_type_args_for_call(args)) break;
      static_cast<NameExpr&>(*e).type_args = std::move(args);
      e->span = span_from(start);
    } else if (at(K::Question)) {
      advance();
      e = std::make_unique<TryExpr>(span_from(start), std::move(e));
    } else if (at(K::QuestionDot)) {
      advance();
      std::string name = expect_ident("field name after '?.'");
      e = std::make_unique<OptChainExpr>(span_from(start), std::move(e), std::move(name));
    } else if (at(K::QuestionQuestion)) {
      advance();
      ExprPtr fallback = parse_unary();
      e = std::make_unique<CoalesceExpr>(span_from(start), std::move(e), std::move(fallback));
    } else if (at(K::LBrace) && no_struct_lit_ == 0) {
      Path path;
      if (!expr_is_path(*e, path)) break;
      e = parse_struct_lit(std::move(path), start);
    } else {
      break;
    }
  }
  return e;
}

ExprPtr Parser::parse_struct_lit(Path path, Span start) {
  auto s = std::make_unique<StructLitExpr>(start);
  s->path = std::move(path);
  expect(K::LBrace, "'{'");
  int saved = no_struct_lit_;
  no_struct_lit_ = 0;
  while (!at(K::RBrace) && !at_end()) {
    FieldInit f;
    f.name = expect_ident("field name", &f.span);
    if (accept(K::Colon)) f.value = parse_expr();
    s->fields.push_back(std::move(f));
    if (!accept(K::Comma)) break;
  }
  no_struct_lit_ = saved;
  expect(K::RBrace, "'}'");
  s->span = span_from(start);
  return s;
}

ExprPtr Parser::parse_interp() {
  Span start = peek().span;
  advance();  // StringStart
  auto e = std::make_unique<InterpExpr>(start);
  while (!at(K::StringEnd) && !at_end()) {
    if (at(K::StringText)) {
      InterpPart p;
      p.text = advance().text;
      e->parts.push_back(std::move(p));
    } else if (accept(K::InterpStart)) {
      InterpPart p;
      int saved = no_struct_lit_;
      no_struct_lit_ = 0;
      p.expr = parse_expr();
      no_struct_lit_ = saved;
      expect(K::InterpEnd, "'}' to close interpolation");
      e->parts.push_back(std::move(p));
    } else {
      error_here("unexpected token inside string");
      advance();
    }
  }
  expect(K::StringEnd, "end of string");
  e->span = span_from(start);
  return e;
}

ExprPtr Parser::parse_if() {
  Span start = peek().span;
  advance();  // if
  auto e = std::make_unique<IfExpr>(start);
  expect(K::LParen, "'(' after if");
  e->cond = parse_expr();
  expect(K::RParen, "')'");
  e->then_block = parse_block();
  if (accept(K::KwElse)) {
    if (at(K::KwIf)) {
      e->else_expr = parse_if();
    } else {
      Span bs = peek().span;
      BlockPtr b = parse_block();
      e->else_expr = std::make_unique<BlockExpr>(span_from(bs), std::move(b));
    }
  }
  e->span = span_from(start);
  return e;
}

ExprPtr Parser::parse_match() {
  Span start = peek().span;
  advance();  // match
  auto m = std::make_unique<MatchExpr>(start);
  m->scrutinee = parse_expr_no_struct();
  expect(K::LBrace, "'{'");
  while (!at(K::RBrace) && !at_end()) {
    size_t before = pos_;
    MatchArm arm;
    Span astart = peek().span;
    arm.pattern = parse_pattern();
    if (accept(K::KwIf)) arm.guard = parse_expr_no_struct();
    expect(K::FatArrow, "'=>'");
    if (at(K::LBrace)) {
      Span bs = peek().span;
      BlockPtr b = parse_block();
      arm.body = std::make_unique<BlockExpr>(span_from(bs), std::move(b));
      accept(K::Comma);
    } else {
      arm.body = parse_expr();
      if (!accept(K::Comma) && !at(K::RBrace)) expect(K::Comma, "',' after match arm");
    }
    arm.span = span_from(astart);
    m->arms.push_back(std::move(arm));
    if (pos_ == before) advance();
  }
  expect(K::RBrace, "'}'");
  m->span = span_from(start);
  return m;
}

ExprPtr Parser::parse_closure() {
  Span start = peek().span;
  auto c = std::make_unique<ClosureExpr>(start);
  if (accept(K::LBracket)) {
    c->explicit_captures = true;
    while (!at(K::RBracket) && !at_end()) {
      ClosureExpr::Capture capture;
      capture.span = peek().span;
      capture.mode = CaptureMode::Move;
      if (accept(K::Amp)) capture.mode = accept(K::KwConst) ? CaptureMode::Shared : CaptureMode::Writable;
      if (accept(K::KwSelf)) capture.name = "self";
      else capture.name = expect_ident("captured variable");
      c->captures.push_back(std::move(capture));
      if (!accept(K::Comma)) break;
    }
    expect(K::RBracket, "']'");
  }
  if (at(K::Ident) && peek().text == "send") { advance(); c->is_send = true; }
  if (accept(K::KwConst)) c->call_mode = CallMode::Shared;
  else if (accept(K::KwOnce)) c->call_mode = CallMode::Once;
  if (accept(K::PipePipe)) {
    // no params
  } else {
    expect(K::Pipe, "'|'");
    while (!at(K::Pipe) && !at_end()) {
      ClosureParam p;
      Span ps = peek().span;
      if (at(K::Ident) && (peek(1).kind == K::Comma || peek(1).kind == K::Pipe)) {
        p.name = advance().text;
      } else {
        p.type = parse_type();
        p.name = expect_ident("parameter name");
      }
      p.span = span_from(ps);
      c->params.push_back(std::move(p));
      if (!accept(K::Comma)) break;
    }
    expect(K::Pipe, "'|'");
  }
  if (accept(K::Arrow)) c->ret = parse_type();
  if (at(K::LBrace)) {
    Span bs = peek().span;
    BlockPtr b = parse_block();
    c->body = std::make_unique<BlockExpr>(span_from(bs), std::move(b));
  } else {
    c->body = parse_expr();
  }
  c->span = span_from(start);
  return c;
}

ExprPtr Parser::parse_primary() {
  const Token& t = peek();
  Span start = t.span;
  switch (t.kind) {
    case K::IntLit: advance(); return std::make_unique<IntLitExpr>(t.span, t.int_value, false);
    case K::LongLit: advance(); return std::make_unique<IntLitExpr>(t.span, t.int_value, true);
    case K::FloatLit: advance(); return std::make_unique<FloatLitExpr>(t.span, t.float_value, true);
    case K::DoubleLit: advance(); return std::make_unique<FloatLitExpr>(t.span, t.float_value, false);
    case K::CharLit: advance(); return std::make_unique<CharLitExpr>(t.span, static_cast<uint32_t>(t.int_value));
    case K::StringLit: advance(); return std::make_unique<StringLitExpr>(t.span, t.text);
    case K::StringStart: return parse_interp();
    case K::KwTrue: advance(); return std::make_unique<BoolLitExpr>(t.span, true);
    case K::KwFalse: advance(); return std::make_unique<BoolLitExpr>(t.span, false);
    case K::Ident:
      if (t.text == "send" && (peek(1).kind == K::KwOnce || peek(1).kind == K::KwConst)) return parse_closure();
      advance(); return std::make_unique<NameExpr>(t.span, t.text);
    case K::KwSelf: advance(); return std::make_unique<NameExpr>(t.span, "self");
    case K::KwSelfType: advance(); return std::make_unique<NameExpr>(t.span, "Self");
    case K::LParen: {
      advance();
      int saved = no_struct_lit_;
      no_struct_lit_ = 0;
      if (accept(K::RParen)) {
        no_struct_lit_ = saved;
        return std::make_unique<TupleExpr>(span_from(start));
      }
      ExprPtr first = parse_expr();
      if (accept(K::Comma)) {
        auto tup = std::make_unique<TupleExpr>(start);
        tup->elems.push_back(std::move(first));
        while (!at(K::RParen) && !at_end()) {
          tup->elems.push_back(parse_expr());
          if (!accept(K::Comma)) break;
        }
        no_struct_lit_ = saved;
        expect(K::RParen, "')'");
        tup->span = span_from(start);
        return tup;
      }
      no_struct_lit_ = saved;
      expect(K::RParen, "')'");
      if (first) first->span = span_from(start);
      return first;
    }
    case K::LBracket: {
      size_t end = pos_ + 1;
      while (end < tokens_.size() && (tokens_[end].kind == K::Ident || tokens_[end].kind == K::Amp ||
             tokens_[end].kind == K::KwConst || tokens_[end].kind == K::KwSelf || tokens_[end].kind == K::Comma)) ++end;
      if (end + 1 < tokens_.size() && tokens_[end].kind == K::RBracket) {
        size_t next = end + 1;
        if (tokens_[next].kind == K::Ident && tokens_[next].text == "send") ++next;
        if (tokens_[next].kind == K::KwConst || tokens_[next].kind == K::KwOnce) ++next;
        if (next < tokens_.size() && (tokens_[next].kind == K::Pipe || tokens_[next].kind == K::PipePipe)) return parse_closure();
      }
      advance();
      int saved = no_struct_lit_;
      no_struct_lit_ = 0;
      if (accept(K::RBracket)) {
        no_struct_lit_ = saved;
        return std::make_unique<ArrayExpr>(span_from(start));
      }
      ExprPtr first = parse_expr();
      if (accept(K::Semi)) {
        ExprPtr count = parse_expr();
        no_struct_lit_ = saved;
        expect(K::RBracket, "']'");
        return std::make_unique<ArrayRepeatExpr>(span_from(start), std::move(first), std::move(count));
      }
      auto arr = std::make_unique<ArrayExpr>(start);
      arr->elems.push_back(std::move(first));
      while (accept(K::Comma)) {
        if (at(K::RBracket)) break;
        arr->elems.push_back(parse_expr());
      }
      no_struct_lit_ = saved;
      expect(K::RBracket, "']'");
      arr->span = span_from(start);
      return arr;
    }
    case K::LBrace: {
      BlockPtr b = parse_block();
      return std::make_unique<BlockExpr>(span_from(start), std::move(b));
    }
    case K::KwIf: return parse_if();
    case K::KwMatch: return parse_match();
    case K::Pipe:
    case K::KwOnce:
    case K::KwConst:
    case K::PipePipe: return parse_closure();
    case K::KwUnsafe: {
      advance();
      BlockPtr b = parse_block();
      return std::make_unique<UnsafeExpr>(span_from(start), std::move(b));
    }
    case K::KwAwait: {
      advance();
      ExprPtr e = parse_unary();
      return std::make_unique<AwaitExpr>(span_from(start), std::move(e));
    }
    case K::KwReturn: {
      advance();
      ExprPtr value;
      if (!at_range_end() || at(K::LBrace)) {
        if (!(at(K::Semi) || at(K::RBrace) || at(K::Comma) || at(K::RParen))) value = parse_expr();
      }
      return std::make_unique<ReturnExpr>(span_from(start), std::move(value));
    }
    case K::KwBreak: advance(); return std::make_unique<BreakExpr>(t.span);
    case K::KwContinue: advance(); return std::make_unique<ContinueExpr>(t.span);
    default:
      error_here(std::string("expected an expression, found ") + token_kind_name(t.kind), "SNX0201");
      // Placeholder so the caller has a node to attach to.
      return std::make_unique<TupleExpr>(t.span);
  }
}

// ---------- patterns ----------

ExprPtr Parser::parse_literal_for_pattern() {
  const Token& t = peek();
  switch (t.kind) {
    case K::IntLit: advance(); return std::make_unique<IntLitExpr>(t.span, t.int_value, false);
    case K::LongLit: advance(); return std::make_unique<IntLitExpr>(t.span, t.int_value, true);
    case K::FloatLit: advance(); return std::make_unique<FloatLitExpr>(t.span, t.float_value, true);
    case K::DoubleLit: advance(); return std::make_unique<FloatLitExpr>(t.span, t.float_value, false);
    case K::CharLit: advance(); return std::make_unique<CharLitExpr>(t.span, static_cast<uint32_t>(t.int_value));
    case K::StringLit: advance(); return std::make_unique<StringLitExpr>(t.span, t.text);
    case K::KwTrue: advance(); return std::make_unique<BoolLitExpr>(t.span, true);
    case K::KwFalse: advance(); return std::make_unique<BoolLitExpr>(t.span, false);
    case K::Minus: {
      Span start = t.span;
      advance();
      ExprPtr inner = parse_literal_for_pattern();
      return std::make_unique<UnaryExpr>(span_from(start), UnaryOp::Neg, std::move(inner));
    }
    default:
      error_here(std::string("expected a pattern, found ") + token_kind_name(t.kind), "SNX0201");
      return nullptr;
  }
}

PatternPtr Parser::parse_pattern() {
  Span start = peek().span;
  PatternPtr first = parse_pattern_primary();
  if (!at(K::Pipe)) return first;
  auto o = std::make_unique<OrPattern>(start);
  o->alts.push_back(std::move(first));
  while (accept(K::Pipe)) o->alts.push_back(parse_pattern_primary());
  o->span = span_from(start);
  return o;
}

PatternPtr Parser::parse_pattern_primary() {
  Span start = peek().span;
  const Token& t = peek();

  if (t.kind == K::Ident && t.text == "_") {
    advance();
    return std::make_unique<WildcardPattern>(t.span);
  }

  if (t.kind == K::Ident) {
    Path path = parse_path();
    if (at(K::LParen)) {
      advance();
      auto p = std::make_unique<TupleStructPattern>(start);
      p->path = std::move(path);
      while (!at(K::RParen) && !at_end()) {
        p->elems.push_back(parse_pattern());
        if (!accept(K::Comma)) break;
      }
      expect(K::RParen, "')'");
      p->span = span_from(start);
      return p;
    }
    if (at(K::LBrace)) {
      advance();
      auto p = std::make_unique<StructPattern>(start);
      p->path = std::move(path);
      while (!at(K::RBrace) && !at_end()) {
        FieldPattern f;
        f.name = expect_ident("field name", &f.span);
        if (accept(K::Colon)) f.pattern = parse_pattern();
        p->fields.push_back(std::move(f));
        if (!accept(K::Comma)) break;
      }
      expect(K::RBrace, "'}'");
      p->span = span_from(start);
      return p;
    }
    if (path.segments.size() == 1) {
      return std::make_unique<BindingPattern>(path.span, path.segments[0].name);
    }
    auto p = std::make_unique<PathPattern>(path.span);
    p->path = std::move(path);
    return p;
  }

  if (t.kind == K::LParen) {
    advance();
    auto p = std::make_unique<TuplePattern>(start);
    while (!at(K::RParen) && !at_end()) {
      p->elems.push_back(parse_pattern());
      if (!accept(K::Comma)) break;
    }
    expect(K::RParen, "')'");
    p->span = span_from(start);
    return p;
  }

  ExprPtr lit = parse_literal_for_pattern();
  if (!lit) {
    advance();
    return std::make_unique<WildcardPattern>(t.span);
  }
  if (at(K::DotDot) || at(K::DotDotEq)) {
    bool inclusive = advance().kind == K::DotDotEq;
    ExprPtr hi = parse_literal_for_pattern();
    if (!hi) hi = std::make_unique<TupleExpr>(peek().span);
    return std::make_unique<RangePattern>(span_from(start), std::move(lit), std::move(hi), inclusive);
  }
  return std::make_unique<LiteralPattern>(span_from(start), std::move(lit));
}

// ---------- test entry points ----------

ExprPtr Parser::parse_expression() { return parse_expr(); }
TypePtr Parser::parse_type_only() { return parse_type(); }
PatternPtr Parser::parse_pattern_only() { return parse_pattern(); }

StmtPtr Parser::parse_statement_only() {
  Block b;
  // Reuse the block body parser on a single statement by wrapping it.
  parse_block_body(b);
  if (!b.stmts.empty()) return std::move(b.stmts.front());
  if (b.tail) return std::make_unique<ExprStmt>(b.tail->span, std::move(b.tail), false);
  return nullptr;
}

}  // namespace synx
