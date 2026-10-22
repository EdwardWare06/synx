#pragma once

#include <string>
#include <vector>

#include "synx/ast.hpp"
#include "synx/diagnostics.hpp"
#include "synx/source.hpp"
#include "synx/token.hpp"

namespace synx {

// Recursive descent parser. Reports errors to the sink, recovers at the next
// statement or item, and always returns a module so later phases can still
// run on the parts that parsed.
class Parser {
 public:
  Parser(const SourceFile& file, std::vector<Token> tokens, DiagnosticSink& sink);

  ast::Module parse_module();

  // Entry points for tests.
  ast::ExprPtr parse_expression();
  ast::TypePtr parse_type_only();
  ast::PatternPtr parse_pattern_only();
  ast::StmtPtr parse_statement_only();

  // Index of the next unread token. Parity tests compare it after each entry point.
  size_t position() const { return pos_; }

 private:
  // ----- token access -----
  const Token& peek(size_t ahead = 0) const;
  const Token& advance();
  bool at(TokenKind k) const { return peek().kind == k; }
  bool at_end() const { return at(TokenKind::Eof); }
  bool accept(TokenKind k);
  bool expect(TokenKind k, const char* what);
  std::string expect_ident(const char* what, Span* span_out = nullptr);
  void error(Span span, std::string message, const char* code = "");
  void error_here(std::string message, const char* code = "");
  Span span_from(Span start) const;

  // ----- lookahead -----
  // Index just after the ">" that closes a generic list starting at index i
  // (which must be "<"), or 0 if the list does not close.
  size_t skip_generic_list(size_t i) const;
  // Index just after the ")" that closes a paren group starting at i.
  size_t skip_paren_group(size_t i) const;
  // True when the tokens at the cursor are: ident [<generics>] "(".
  bool looks_like_function_name() const;
  // True when the tokens at the cursor form: type ident ("=" | ";").
  bool looks_like_var_decl();

  // ----- items -----
  ast::ItemPtr parse_item();
  ast::ItemPtr parse_import(Span start);
  ast::ItemPtr parse_const_item(Span start);
  ast::ItemPtr parse_struct(Span start);
  ast::ItemPtr parse_enum(Span start);
  ast::ItemPtr parse_trait(Span start);
  ast::ItemPtr parse_impl(Span start);
  ast::ItemPtr parse_extern(Span start);
  void sync_item();

  enum class FnContext { Item, Method, TraitMethod, Extern };
  ast::FunctionPtr parse_function(FnContext ctx, std::string doc, bool is_pub);
  ast::FunctionPtr parse_function_after_head(FnContext ctx, std::string doc, bool is_pub,
                                             bool is_async, Span start, ast::TypePtr ret,
                                             std::string name, Span name_span);
  std::vector<ast::GenericParam> parse_generic_params();
  ast::Bound parse_bound();
  std::vector<ast::Param> parse_params(FnContext ctx);
  // Parses one member of a struct, enum, trait, impl, or extern body.
  // Appends to fields or methods. Returns false when nothing was consumed.
  bool parse_member(FnContext ctx, std::vector<ast::Field>* fields,
                    std::vector<ast::FunctionPtr>& methods);

  // ----- types -----
  ast::TypePtr parse_type();
  ast::TypePtr parse_postfix_type();
  ast::TypePtr parse_base_type();
  ast::Path parse_path();
  std::vector<ast::TypePtr> parse_type_args();

  // ----- statements -----
  ast::BlockPtr parse_block();
  void parse_block_body(ast::Block& block);
  ast::StmtPtr parse_var_decl();
  bool is_block_like(const ast::Expr& e) const;

  // ----- expressions -----
  ast::ExprPtr parse_expr();
  ast::ExprPtr parse_expr_no_struct();
  ast::ExprPtr parse_range();
  ast::ExprPtr parse_binary(int min_prec);
  ast::ExprPtr parse_cast();
  ast::ExprPtr parse_unary();
  ast::ExprPtr parse_postfix();
  ast::ExprPtr parse_primary();
  ast::ExprPtr parse_if();
  ast::ExprPtr parse_match();
  ast::ExprPtr parse_closure();
  ast::ExprPtr parse_interp();
  ast::ExprPtr parse_struct_lit(ast::Path path, Span start);
  std::vector<ast::ExprPtr> parse_call_args();
  bool try_parse_type_args_for_call(std::vector<ast::TypePtr>& out);
  bool expr_is_path(const ast::Expr& e, ast::Path& out) const;
  bool at_range_end() const;
  int binary_prec(ast::BinaryOp& op, size_t& len) const;

  // ----- patterns -----
  ast::PatternPtr parse_pattern();
  ast::PatternPtr parse_pattern_primary();
  ast::ExprPtr parse_literal_for_pattern();

  const SourceFile& file_;
  std::vector<Token> tokens_;
  DiagnosticSink& sink_;
  size_t pos_ = 0;
  int speculating_ = 0;     // >0: errors are counted, not reported
  int spec_errors_ = 0;
  int no_struct_lit_ = 0;   // >0: "Path {" is not a struct literal
};

}  // namespace synx
