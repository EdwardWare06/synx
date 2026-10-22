#include "synx/regalloc.hpp"

#include <algorithm>
#include <limits>
#include <set>

namespace synx::codegen {
namespace {

using Locals = std::set<uint32_t>;
struct Event {
  Locals uses, defs;
  Locals after;  // Read after a call returns: indexes of its destination.
  bool call = false;
  size_t pos = 0;
};
struct Block {
  std::vector<Event> events;
  std::vector<uint32_t> successors;
  Locals uses, defs, in, out;
};
struct Interval {
  uint32_t local;
  size_t start = std::numeric_limits<size_t>::max(), end = 0;
  bool crosses_call = false;
};

}  // namespace

RegisterAllocation allocate_registers(const ir::Function& f, LayoutTable& lay) {
  RegisterAllocation result;
  result.registers.assign(f.locals.size(), -1);
  if (f.blocks.empty()) return result;
  std::vector<bool> pinned(f.locals.size(), false);
  pinned[0] = true;  // Common epilogue reads the return slot.
  if (f.closure) pinned[1] = true;  // R10 is spilled before parameter loads.
  std::vector<Block> blocks(f.blocks.size());
  std::vector<size_t> calls;
  size_t pos = 0;
  auto place = [&](Event& e, const ir::Place& p, bool write) {
    if (p.is_none()) return;
    if (write && p.is_local()) e.defs.insert(p.local);
    else e.uses.insert(p.local);
    if (!p.is_local()) pinned[p.local] = true;
    for (auto& pr : p.proj)
      if (pr.kind == ir::ProjKind::Index) e.uses.insert(pr.index);
  };
  auto operand = [&](Event& e, const ir::Operand& o) {
    if (o.kind != ir::Operand::Kind::Const) place(e, o.place, false);
  };
  for (size_t bi = 0; bi < f.blocks.size(); ++bi) {
    auto& b = blocks[bi];
    auto add = [&](Event e) {
      e.pos = pos;
      pos += 2;  // Uses precede definitions, including call results.
      if (e.call) calls.push_back(e.pos);
      for (auto local : e.uses) if (!b.defs.count(local)) b.uses.insert(local);
      b.defs.insert(e.defs.begin(), e.defs.end());
      b.events.push_back(std::move(e));
    };
    for (auto& s : f.blocks[bi].stmts) {
      Event e;
      switch (s.kind) {
        case ir::Statement::Kind::Assign: {
          place(e, s.place, true);
          const auto& r = s.rvalue;
          bool direct = r.kind == ir::RvalueKind::Use || r.kind == ir::RvalueKind::Binary ||
                        r.kind == ir::RvalueKind::Unary || r.kind == ir::RvalueKind::Cast;
          // Remaining emitters write through an address. Their destinations
          // retain the stack path until they gain register writes.
          if (!direct) pinned[s.place.local] = true;
          if (r.kind == ir::RvalueKind::Cast && r.type->is_float()) pinned[s.place.local] = true;
          switch (r.kind) {
            case ir::RvalueKind::Ref:
              pinned[r.place.local] = true;
              place(e, r.place, false);
              break;
            case ir::RvalueKind::Len: case ir::RvalueKind::Discriminant:
              pinned[r.place.local] = true;
              place(e, r.place, false);
              break;
            case ir::RvalueKind::Aggregate:
              for (auto& o : r.elems) operand(e, o);
              for (auto& o : r.captures) operand(e, o);
              break;
            case ir::RvalueKind::Binary:
              operand(e, r.b);
              [[fallthrough]];
            default: operand(e, r.a); break;
          }
          break;
        }
        case ir::Statement::Kind::DropIf:
          e.uses.insert(s.flag_local);
          pinned[s.flag_local] = true;
          [[fallthrough]];
        case ir::Statement::Kind::Drop:
          place(e, s.place, false);
          pinned[s.place.local] = true;
          e.call = true;  // Drop glue can call user code and the C runtime.
          break;
        case ir::Statement::Kind::StorageDead:
          e.defs.insert(s.place.local);
          break;
        case ir::Statement::Kind::Nop: break;
      }
      add(std::move(e));
    }
    Event e;
    const auto& t = f.blocks[bi].term;
    switch (t.kind) {
      case ir::Terminator::Kind::Goto: b.successors.push_back(t.target); break;
      case ir::Terminator::Kind::Switch:
        operand(e, t.discr);
        for (auto& c : t.cases) b.successors.push_back(c.second);
        b.successors.push_back(t.otherwise);
        break;
      case ir::Terminator::Kind::Call:
        for (auto& o : t.args) operand(e, o);
        if (t.callee.kind == ir::Callee::Kind::Indirect) operand(e, t.callee.fn_operand);
        place(e, t.dest, true);
        // The result is stored after the call returns, so destination
        // indexes must survive the call.
        for (auto& pr : t.dest.proj)
          if (pr.kind == ir::ProjKind::Index) e.after.insert(pr.index);
        e.call = true;  // Includes intrinsics, conservatively.
        if (!t.diverges) b.successors.push_back(t.target);
        break;
      case ir::Terminator::Kind::Assert:
        operand(e, t.cond);
        b.successors.push_back(t.target);
        break;  // Panic never returns; the success path makes no call.
      case ir::Terminator::Kind::Return: e.uses.insert(0); break;
      case ir::Terminator::Kind::Unreachable: break;
    }
    add(std::move(e));
  }

  bool changed;
  do {
    changed = false;
    for (size_t i = blocks.size(); i-- > 0;) {
      auto& b = blocks[i];
      Locals out, in = b.uses;
      for (auto successor : b.successors)
        out.insert(blocks[successor].in.begin(), blocks[successor].in.end());
      for (auto local : out) if (!b.defs.count(local)) in.insert(local);
      if (in != b.in || out != b.out) {
        b.in = std::move(in);
        b.out = std::move(out);
        changed = true;
      }
    }
  } while (changed);

  std::vector<Interval> intervals;
  for (uint32_t local = 0; local < f.locals.size(); ++local) {
    const auto* t = f.locals[local].type;
    const auto size = lay.size_of(t);
    bool scalar = (t->kind == sema::TypeKind::Prim && !t->is_float()) ||
                  t->kind == sema::TypeKind::Ptr || t->kind == sema::TypeKind::Ref;
    if (pinned[local] || !scalar || !size || size > 8) continue;
    Interval interval{local};
    auto touch = [&](size_t p) {
      interval.start = std::min(interval.start, p);
      interval.end = std::max(interval.end, p);
    };
    // All arguments are loaded in the prologue. They interfere from entry.
    if (local <= f.arg_count) touch(0);
    for (auto& b : blocks) {
      if (b.in.count(local)) touch(b.events.front().pos);
      if (b.out.count(local)) touch(b.events.back().pos + 1);
      for (auto& e : b.events) {
        if (e.uses.count(local)) touch(e.pos);
        if (e.after.count(local)) touch(e.pos + 1);
        // StorageDead kills liveness but does not define a machine value.
        if (e.defs.count(local)) touch(e.pos + 1);
      }
    }
    if (interval.start == std::numeric_limits<size_t>::max()) continue;
    for (auto call : calls)
      if (interval.start <= call && call < interval.end) interval.crosses_call = true;
    intervals.push_back(interval);
  }
  std::sort(intervals.begin(), intervals.end(), [](auto& a, auto& b) {
    return a.start != b.start ? a.start < b.start : a.local < b.local;
  });
  std::vector<Interval> active;
  // Other volatile registers belong to the instruction selector.
  const Reg pool[] = {R8, R9, RBX, R12, R13, R14, R15};
  for (auto& interval : intervals) {
    active.erase(std::remove_if(active.begin(), active.end(), [&](auto& a) {
      return a.end < interval.start;
    }), active.end());
    for (Reg reg : pool) {
      if (interval.crosses_call && (reg == R8 || reg == R9)) continue;
      bool used = std::any_of(active.begin(), active.end(), [&](auto& a) {
        return result.registers[a.local] == reg;
      });
      if (used) continue;
      result.registers[interval.local] = reg;
      if (reg != R8 && reg != R9 && std::find(result.saved.begin(), result.saved.end(), reg) == result.saved.end())
        result.saved.push_back(reg);
      active.push_back(interval);
      break;
    }
  }
  return result;
}

}  // namespace synx::codegen
