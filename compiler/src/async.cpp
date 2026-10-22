#include "synx/async.hpp"

#include <algorithm>
#include <functional>
#include <limits>

namespace synx::ir {
namespace {

struct Builder {
  Function& f;
  sema::TypeContext& types;
  Span span;
  uint32_t local(const sema::Type* type, const std::string& name = "") {
    f.locals.push_back({type, name, false, span});
    return static_cast<uint32_t>(f.locals.size() - 1);
  }
  uint32_t block() { f.blocks.emplace_back(); return static_cast<uint32_t>(f.blocks.size() - 1); }
  Operand number(uint64_t value, const sema::Type* type = nullptr) {
    Constant c; c.kind = type && type->is_bool() ? Constant::Kind::Bool : Constant::Kind::Int;
    c.type = type ? type : types.usize_type(); c.int_value = value;
    return Operand::constant_of(c);
  }
  void assign(uint32_t block, Place place, Rvalue value) {
    Statement s; s.kind = Statement::Kind::Assign; s.place = std::move(place); s.rvalue = std::move(value); s.span = span;
    f.blocks[block].stmts.push_back(std::move(s));
  }
  void use(uint32_t block, Place dest, Operand value) { Rvalue r; r.a = std::move(value); assign(block, std::move(dest), std::move(r)); }
  void ret(uint32_t block) { f.blocks[block].term.kind = Terminator::Kind::Return; f.blocks[block].term.span = span; }
  uint32_t call(uint32_t block, const std::string& name, std::vector<Operand> args, Place dest,
                std::vector<const sema::Type*> type_args = {}) {
    uint32_t next = this->block();
    Terminator t; t.kind = Terminator::Kind::Call; t.callee.kind = Callee::Kind::Builtin;
    t.callee.builtin = name; t.callee.type_args = std::move(type_args); t.args = std::move(args);
    t.dest = std::move(dest); t.target = next; t.span = span;
    f.blocks[block].term = std::move(t);
    return next;
  }
  void panic(uint32_t block, const std::string& message) {
    Constant c; c.kind = Constant::Kind::Str; c.type = types.ref(types.str(), true); c.str_value = message;
    Terminator t; t.kind = Terminator::Kind::Call; t.callee.kind = Callee::Kind::Builtin; t.callee.builtin = "panic";
    t.args = {Operand::constant_of(c)}; t.dest = Place::none(); t.diverges = true; t.span = span;
    f.blocks[block].term = std::move(t);
  }
};

bool transform(sema::Program& program, sema::TypeContext& types, Function& factory, DiagnosticSink& sink) {
  Function& poll = *factory.async_poll;
  Function& cancel = *factory.async_cancel;
  const auto* output = factory.ret->elems[0];
  const auto* poll_type = types.enum_type(program.poll_def, {output});
  const auto* wake_type = types.struct_type(program.waker_def, {});
  const auto* raw = types.ptr(types.prim(ast::PrimKind::Byte), false);
  auto old_locals = std::move(poll.locals);
  auto old_blocks = std::move(poll.blocks);
  uint32_t old_wake = poll.async_waker;
  Span span = factory.def->span;
  std::vector<const sema::Type*> fields{types.usize_type()};
  std::vector<uint32_t> field(old_locals.size(), UINT32_MAX);
  for (uint32_t i = 0; i < old_locals.size(); ++i) {
    if (i == old_wake) continue;
    field[i] = static_cast<uint32_t>(fields.size()); fields.push_back(old_locals[i].type);
  }
  const auto* frame = types.tuple(std::move(fields));
  const auto* frame_pointer = types.ptr(frame, false);
  auto at = [&](uint32_t local) { return Place::of(1).deref().field(field[local]); };
  auto state = Place::of(1).deref().field(0);
  std::map<uint32_t, uint64_t> states;
  uint64_t next_state = 1;
  for (auto& [block, resume] : poll.async_suspends) { (void)resume; states[block] = next_state++; }
  constexpr uint64_t complete = std::numeric_limits<uint64_t>::max();

  // Cancellation observes references held by destructors. Order each state
  // by its may-borrow graph, with reverse declaration order for free choices.
  std::map<uint64_t, std::vector<uint32_t>> orders;
  orders[0] = {poll.async_drop_locals.rbegin(), poll.async_drop_locals.rend()};
  for (auto& [block, number] : states) {
    std::map<uint32_t, std::set<uint32_t>> graph, drops;
    for (auto [holder, referent] : poll.async_borrows[block]) graph[holder].insert(referent);
    std::map<uint32_t, unsigned> incoming;
    for (uint32_t local : poll.async_drop_locals) incoming[local] = 0;
    for (uint32_t holder : poll.async_drop_locals) {
      std::set<uint32_t> seen;
      std::vector<uint32_t> work(graph[holder].begin(), graph[holder].end());
      while (!work.empty()) {
        uint32_t referent = work.back(); work.pop_back();
        if (!seen.insert(referent).second || referent == holder) continue;
        if (poll.async_drop_locals.count(referent) && drops[holder].insert(referent).second) ++incoming[referent];
        const auto& children = graph[referent]; work.insert(work.end(), children.begin(), children.end());
      }
    }
    std::set<uint32_t, std::greater<uint32_t>> ready;
    for (auto [local, count] : incoming) if (count == 0) ready.insert(local);
    auto& order = orders[number];
    while (!ready.empty()) {
      uint32_t local = *ready.begin(); ready.erase(ready.begin()); order.push_back(local);
      for (uint32_t referent : drops[local]) if (--incoming[referent] == 0) ready.insert(referent);
    }
    if (order.size() != poll.async_drop_locals.size()) {
      sink.error(old_blocks[block].term.span, "cannot suspend here; values with destructors borrow each other", "SNX0507");
      return false;
    }
  }

  // The factory transfers arguments into stable storage without running the
  // body. Its checked capture metadata established the input lifetimes.
  Rvalue handle = factory.blocks[0].stmts[0].rvalue;
  factory.blocks.clear();
  Builder make{factory, types, span};
  uint32_t b = make.block();
  uint32_t size = make.local(types.usize_type());
  b = make.call(b, "size_of", {}, Place::of(size), {frame});
  uint32_t pointer = make.local(frame_pointer, "@frame");
  b = make.call(b, "synx_rt_alloc", {Operand::copy(Place::of(size))}, Place::of(pointer));
  auto factory_field = [&](uint32_t index) { return Place::of(pointer).deref().field(index); };
  make.use(b, factory_field(0), make.number(0));
  for (uint32_t arg = 1; arg <= factory.arg_count; ++arg)
    make.use(b, factory_field(field[arg]), Operand::copy(Place::of(arg)));
  for (auto [local, flag] : poll.async_drop_flags)
    make.use(b, factory_field(field[flag]), make.number(local > 0 && local <= factory.arg_count, types.bool_type()));
  uint32_t bytes = make.local(raw);
  Rvalue cast; cast.kind = RvalueKind::Cast; cast.a = Operand::copy(Place::of(pointer)); cast.type = raw;
  make.assign(b, Place::of(bytes), cast);
  handle.elems[0] = Operand::copy(Place::of(bytes)); handle.captures.clear();
  make.assign(b, Place::of(0), handle); make.ret(b);

  poll.ret = poll_type; poll.arg_count = 2;
  poll.locals = {{poll_type, "return", false, span}, {frame_pointer, "frame", true, span}, {wake_type, "wake", true, span}};
  poll.blocks.resize(old_blocks.size() + 1);
  Builder resume{poll, types, span};
  auto load = [&](uint32_t old, std::vector<Statement>& before) {
    uint32_t temp = resume.local(old_locals[old].type);
    Statement s; s.kind = Statement::Kind::Assign; s.place = Place::of(temp); s.rvalue.a = Operand::copy(at(old)); s.span = span;
    before.push_back(s); return temp;
  };
  auto place = [&](Place original, std::vector<Statement>& before) {
    if (original.is_none()) return original;
    Place result = original.local == old_wake ? Place::of(2) : at(original.local);
    for (auto projection : original.proj) {
      if (projection.kind == ProjKind::Index) projection.index = load(projection.index, before);
      result.proj.push_back(projection);
    }
    return result;
  };
  auto operand = [&](Operand& value, std::vector<Statement>& before) {
    if (value.kind != Operand::Kind::Const) value.place = place(value.place, before);
  };
  auto finish = [&](uint32_t block, bool ready, uint64_t number) {
    resume.use(block, state, resume.number(number));
    Rvalue result; result.kind = RvalueKind::Aggregate; result.agg = AggregateKind::Variant;
    result.type = poll_type; result.variant = ready ? 1 : 0;
    if (ready) {
      if (output->is_void() || output->is_never()) {
        Constant unit; unit.kind = Constant::Kind::Unit; unit.type = output;
        result.elems = {Operand::constant_of(unit)};
      } else result.elems = {sema::is_copy_type(program, types, output) ? Operand::copy(at(0)) : Operand::move(at(0))};
    }
    resume.assign(block, Place::of(0), result);
    Statement drop; drop.kind = Statement::Kind::Drop; drop.place = Place::of(2); drop.span = span;
    poll.blocks[block].stmts.push_back(drop);
    resume.ret(block);
  };
  for (uint32_t index = 0; index < old_blocks.size(); ++index) {
    uint32_t block = index + 1;
    for (auto statement : old_blocks[index].stmts) {
      if (statement.kind == Statement::Kind::StorageDead || statement.kind == Statement::Kind::Nop) continue;
      std::vector<Statement> before;
      statement.place = place(statement.place, before);
      auto& r = statement.rvalue;
      operand(r.a, before); operand(r.b, before);
      for (auto& value : r.elems) operand(value, before);
      r.captures.clear();
      if (r.kind == RvalueKind::Ref || r.kind == RvalueKind::Len || r.kind == RvalueKind::Discriminant) r.place = place(r.place, before);
      if (statement.kind == Statement::Kind::DropIf) statement.flag_local = load(statement.flag_local, before);
      auto& dest = poll.blocks[block].stmts;
      dest.insert(dest.end(), before.begin(), before.end()); dest.push_back(std::move(statement));
    }
    auto term = old_blocks[index].term;
    if (states.count(index)) { finish(block, false, states[index]); continue; }
    if (term.kind == Terminator::Kind::Return) { finish(block, true, complete); continue; }
    std::vector<Statement> before;
    operand(term.discr, before); operand(term.cond, before); operand(term.callee.fn_operand, before);
    for (auto& arg : term.args) operand(arg, before);
    if (term.kind == Terminator::Kind::Call) term.dest = place(term.dest, before);
    if (term.kind == Terminator::Kind::Goto || term.kind == Terminator::Kind::Call || term.kind == Terminator::Kind::Assert) ++term.target;
    if (term.kind == Terminator::Kind::Switch) {
      ++term.otherwise; for (auto& choice : term.cases) ++choice.second;
    }
    auto& dest = poll.blocks[block].stmts;
    dest.insert(dest.end(), before.begin(), before.end()); poll.blocks[block].term = std::move(term);
  }
  uint32_t invalid = resume.block(); resume.panic(invalid, "async task polled after completion");
  Terminator dispatch; dispatch.kind = Terminator::Kind::Switch; dispatch.discr = Operand::copy(state);
  dispatch.cases = {{0, 1}}; dispatch.otherwise = invalid; dispatch.span = span;
  for (auto [marker, target] : poll.async_suspends) dispatch.cases.push_back({states[marker], target + 1});
  poll.blocks[0].term = dispatch;

  cancel.locals = {{types.void_type(), "return", false, span}, {frame_pointer, "frame", true, span}};
  cancel.blocks.clear();
  Builder cleanup{cancel, types, span}; cleanup.block();
  dispatch.cases.clear();
  for (auto& [number, order] : orders) {
    uint32_t block = cleanup.block(); dispatch.cases.push_back({number, block});
    for (uint32_t local : order) {
      uint32_t flag = cleanup.local(types.bool_type());
      cleanup.use(block, Place::of(flag), Operand::copy(at(poll.async_drop_flags.at(local))));
      Statement drop; drop.kind = Statement::Kind::DropIf; drop.place = at(local); drop.flag_local = flag; drop.span = span;
      cancel.blocks[block].stmts.push_back(drop);
    }
    block = cleanup.call(block, "synx_rt_free", {Operand::copy(Place::of(1))}, Place::none()); cleanup.ret(block);
  }
  uint32_t done = cleanup.block(); dispatch.cases.push_back({complete, done});
  done = cleanup.call(done, "synx_rt_free", {Operand::copy(Place::of(1))}, Place::none()); cleanup.ret(done);
  invalid = cleanup.block(); cleanup.panic(invalid, "invalid async cancellation state");
  dispatch.otherwise = invalid; cancel.blocks[0].term = dispatch;
  return true;
}

}  // namespace

bool lower_async(sema::Program& program, sema::TypeContext& types, Module& module, DiagnosticSink& sink, sema::Checker& checker) {
  for (const auto& function : module.functions)
    if (function->async_poll) {
      auto& body = *function->async_poll;
      if (function->def->ast->async_send) {
        for (auto [block, live] : body.async_live) {
          bool changed = true;
          while (changed) {
            changed = false;
            for (auto [holder, root] : body.async_borrows[block])
              if (live.count(holder) && live.insert(root).second) changed = true;
          }
          for (auto local : live) {
            if (local == body.async_waker) continue;
            const auto* type = body.locals[local].type;
            bool strict = checker.has_marker(type, "Send");
            bool borrowed = false;
            for (auto [holder, root] : body.async_borrows[block]) if (holder == local && live.count(root)) borrowed = true;
            if (!strict && !(borrowed && checker.has_marker(type, "Send", true))) {
              sink.error(body.blocks[block].term.span,
                "cannot suspend a send task while " + (body.locals[local].name.empty() ? std::string("a temporary") : "`" + body.locals[local].name + "`") +
                " of type '" + type->to_string() + "' is retained; it does not implement Send", "SNX0420");
              return false;
            }
          }
        }
      }
      if (!transform(program, types, *function, sink)) return false;
    }
  return true;
}

}  // namespace synx::ir
