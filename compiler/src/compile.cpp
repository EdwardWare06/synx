#include "synx/compile.hpp"

#include "synx/borrow.hpp"
#include "synx/async.hpp"
#include "synx/lexer.hpp"
#include "synx/lower.hpp"
#include "synx/parser.hpp"

namespace synx {

bool Compilation::lower_ir(bool require_main) {
  ir::Lowerer lowerer(program, types, checker->result(), sink);
  return lowerer.lower_program(ir_module, require_main);
}

bool Compilation::check_ownership() {
  ir::BorrowChecker borrow(program, types, sink);
  return borrow.check_module(ir_module);
}

bool Compilation::convert_async() { return ir::lower_async(program, types, ir_module, sink, *checker); }

bool Compilation::run_lowering(bool require_main) {
  return lower_ir(require_main) && check_ownership() && convert_async();
}

Compilation::Compilation(SourceFile prelude, SourceFile user)
    : prelude_file(std::move(prelude)), user_file(std::move(user)) {
  prelude_file.set_id(1);
  user_file.set_id(0);
  sink.add_source(prelude_file);
  sink.add_source(user_file);
}

bool Compilation::run_declarations() {
  {
    Lexer lexer(prelude_file, prelude_sink);
    Parser parser(prelude_file, lexer.tokenize(), prelude_sink);
    prelude_ast = parser.parse_module();
  }
  {
    Lexer lexer(user_file, sink);
    Parser parser(user_file, lexer.tokenize(), sink);
    user_ast = parser.parse_module();
  }
  if (sink.has_errors() || prelude_sink.has_errors()) return false;

  resolver = std::make_unique<sema::Resolver>(program, types, sink);
  checker = std::make_unique<sema::Checker>(program, types, *resolver, sink);
  checker->register_builtins();
  return load_modules();
}

bool Compilation::run_signatures() {
  if (!run_declarations()) return false;
  resolver->resolve_signatures();
  return !sink.has_errors();
}

bool Compilation::run_front_end() {
  if (!run_signatures()) return false;
  checker->check_program();
  return !sink.has_errors();
}

std::unique_ptr<Compilation> check_source(const std::string& path, const std::string& text) {
  auto c = std::make_unique<Compilation>(SourceFile("<prelude>", sema::prelude_source()),
                                         SourceFile(path, text));
  c->run_front_end();
  return c;
}

}  // namespace synx
