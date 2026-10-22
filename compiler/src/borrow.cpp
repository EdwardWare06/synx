#include "synx/borrow.hpp"

#include <algorithm>
#include <deque>
#include <iterator>
#include <map>
#include <set>
#include <unordered_map>

namespace synx::ir {

using sema::TypeKind;

namespace {

// ---------- helpers over the IR ----------

bool is_copy(sema::Program& prog, sema::TypeContext& ctx, const sema::Type* t) {
  if (t->kind == TypeKind::Str || t->kind == TypeKind::Slice || t->kind == TypeKind::Dyn) return true;
  return sema::is_copy_type(prog, ctx, t);
}


// Source text for a place, such as `a.name` or `*r`. Field names come from
// the declared types; a field reached through a type parameter keeps its
// position, as in `p.0`.
std::string place_text(const Function& f, const Place& p) {
  if (f.locals[p.local].name.empty()) return "a temporary";
  std::string s = f.locals[p.local].name;
  const sema::Type* t = f.locals[p.local].type;
  const sema::VariantDef* variant = nullptr;
  for (auto& pr : p.proj) {
    switch (pr.kind) {
      case ProjKind::Field: {
        std::string name = std::to_string(pr.index);
        const sema::Type* next = nullptr;
        if (t && t->kind == TypeKind::Struct && pr.index < t->struct_def->fields.size()) {
          name = t->struct_def->fields[pr.index].name;
          next = t->struct_def->fields[pr.index].type;
        } else if (t && t->kind == TypeKind::Tuple && pr.index < t->elems.size()) {
          next = t->elems[pr.index];
        } else if (variant && pr.index < variant->tuple_types.size()) {
          next = variant->tuple_types[pr.index];
        } else if (variant && pr.index < variant->fields.size()) {
          name = variant->fields[pr.index].name;
          next = variant->fields[pr.index].type;
        }
        s += "." + name;
        t = next;
        variant = nullptr;
        break;
      }
      case ProjKind::Index: s += "[..]"; t = t ? t->inner : nullptr; break;
      case ProjKind::Deref: s = "*" + s; t = t ? t->inner : nullptr; break;
      case ProjKind::Downcast:
        variant = t && t->kind == TypeKind::Enum && pr.index < t->enum_def->variants.size()
                      ? &t->enum_def->variants[pr.index] : nullptr;
        break;
    }
  }
  return "`" + s + "`";
}

// Two places overlap when one is a prefix of the other. Index projections
// match any index; Downcasts are transparent.
bool overlaps(const Place& a, const Place& b) {
  if (a.local != b.local) return false;
  size_t i = 0, j = 0;
  while (i < a.proj.size() && j < b.proj.size()) {
    const Projection& x = a.proj[i];
    const Projection& y = b.proj[j];
    if (x.kind == ProjKind::Downcast) { ++i; continue; }
    if (y.kind == ProjKind::Downcast) { ++j; continue; }
    if (x.kind != y.kind) return false;
    if (x.kind == ProjKind::Field && x.index != y.index) return false;
    ++i; ++j;
  }
  return true;
}

bool starts_with_deref(const Place& p) {
  for (auto& pr : p.proj) {
    if (pr.kind == ProjKind::Downcast) continue;
    return pr.kind == ProjKind::Deref;
  }
  return false;
}

// True when the first dereference goes through a raw pointer, where moving
// out is the programmer's responsibility inside unsafe code.
bool deref_is_raw(const Function& f, const Place& p) {
  const sema::Type* t = f.locals[p.local].type;
  for (auto& pr : p.proj) {
    if (pr.kind == ProjKind::Downcast) continue;
    if (pr.kind == ProjKind::Deref) return t->kind == TypeKind::Ptr;
    return false;
  }
  return false;
}

// A point in the function: block and statement index. Index == stmts.size()
// means the terminator.
struct Point {
  uint32_t block;
  uint32_t index;
};

struct Loan {
  Place place;
  bool is_mut;
  Span span;
  uint32_t ref_local;  // the local first holding the reference
  bool external = false;  // synthetic origin of a parameter or owned capture
};

enum InitBits : uint8_t { INIT = 1, MOVED = 2, UNINIT = 4 };

using WriteEffects = std::map<const Function*, FunctionEffects>;

const sema::Type* projected_type(sema::TypeContext& ctx, const Function& fn, const Place& place) {
  auto* type = fn.locals[place.local].type;
  uint32_t variant = 0;
  for (const auto& projection : place.proj) {
    if (projection.kind == ProjKind::Downcast) { variant = projection.index; continue; }
    if (projection.kind == ProjKind::Deref || projection.kind == ProjKind::Index) { type = type->inner; continue; }
    if (type->kind == TypeKind::Tuple) type = type->elems[projection.index];
    else {
      sema::Subst subst;
      const auto& generics = type->kind == TypeKind::Struct ? type->struct_def->generics : type->enum_def->generics;
      for (size_t k = 0; k < generics.size(); ++k) subst.bind(generics[k].type, type->elems[k]);
      if (type->kind == TypeKind::Struct) type = subst.apply(ctx, type->struct_def->fields[projection.index].type);
      else {
        const auto& payload = type->enum_def->variants[variant];
        type = subst.apply(ctx, payload.tuple_types.empty() ? payload.fields[projection.index].type : payload.tuple_types[projection.index]);
      }
    }
  }
  return type;
}

bool loan_carrier_type(sema::TypeContext& ctx, const sema::Type* root, bool writable_only = false, bool raw_writes = false) {
  std::vector<const sema::Type*> work{root};
  std::set<const sema::Type*> seen;
  while (!work.empty()) {
    auto* type = work.back(); work.pop_back();
    if (!seen.insert(type).second) continue;
    if (type->kind == TypeKind::Ref) { if (!writable_only || !type->is_const) return true; continue; }
    if (raw_writes && type->kind == TypeKind::Ptr && !type->is_const) return true;
    if (type->kind == TypeKind::Dyn) return true;
    if (type->kind == TypeKind::Fn) { if (!type->is_send) return true; continue; }
    if (type->kind == TypeKind::Array || type->kind == TypeKind::Slice) work.push_back(type->inner);
    if (type->kind == TypeKind::Tuple || type->kind == TypeKind::Struct || type->kind == TypeKind::Enum)
      work.insert(work.end(), type->elems.begin(), type->elems.end());
    if (type->kind != TypeKind::Struct && type->kind != TypeKind::Enum) continue;
    sema::Subst subst;
    const auto& generics = type->kind == TypeKind::Struct ? type->struct_def->generics : type->enum_def->generics;
    for (size_t k = 0; k < generics.size(); ++k) subst.bind(generics[k].type, type->elems[k]);
    if (type->kind == TypeKind::Struct) {
      for (const auto& field : type->struct_def->fields) work.push_back(subst.apply(ctx, field.type));
    } else for (const auto& variant : type->enum_def->variants) {
      for (const auto& field : variant.fields) work.push_back(subst.apply(ctx, field.type));
      for (auto* field : variant.tuple_types) work.push_back(subst.apply(ctx, field));
    }
  }
  return false;
}

WriteEffects infer_write_effects(sema::TypeContext& ctx, Module& module) {
  WriteEffects effects;
  for (const auto& fn : module.functions) if (fn->lowered && !fn->blocks.empty()) effects[fn.get()];
  // Summaries only grow, and every transfer is a monotone union, so any visit
  // order reaches the same least fixed point. A function is analyzed again
  // only when the summary of a direct callee (or its own) has grown.
  std::map<const Function*, std::vector<const Function*>> callers;
  for (const auto& owner : module.functions) {
    if (!effects.count(owner.get())) continue;
    for (const auto& block : owner->blocks) {
      const Terminator& term = block.term;
      if (term.kind != Terminator::Kind::Call || term.callee.kind != Callee::Kind::Direct || !effects.count(term.callee.fn)) continue;
      auto& list = callers[term.callee.fn];
      if (list.empty() || list.back() != owner.get()) list.push_back(owner.get());
    }
  }
  std::vector<const Function*> queue;
  std::set<const Function*> queued;
  for (const auto& owner : module.functions) if (effects.count(owner.get())) { queue.push_back(owner.get()); queued.insert(owner.get()); }
  for (size_t next_function = 0; next_function < queue.size(); ++next_function) {
    const Function& fn = *queue[next_function];
    queued.erase(&fn);
    bool changed = false;
    // Origin sets are interned per analysis: a set is an index into pool,
    // index 0 is the empty set, and unions are cached. A block state maps
    // locals to set indices, sorted by local; missing locals are empty.
    std::deque<std::vector<uint32_t>> pool{{}};
    std::map<std::vector<uint32_t>, uint32_t> interned{{{}, 0}};
    std::unordered_map<uint64_t, uint32_t> unions;
    auto intern = [&](std::vector<uint32_t> set) {
      auto [found, inserted] = interned.try_emplace(std::move(set), static_cast<uint32_t>(pool.size()));
      if (inserted) pool.push_back(found->first);
      return found->second;
    };
    auto join = [&](uint32_t a, uint32_t b) -> uint32_t {
      if (a == b || b == 0) return a;
      if (a == 0) return b;
      if (a > b) std::swap(a, b);
      const uint64_t key = (static_cast<uint64_t>(a) << 32) | b;
      auto cached = unions.find(key);
      if (cached != unions.end()) return cached->second;
      std::vector<uint32_t> merged;
      std::set_union(pool[a].begin(), pool[a].end(), pool[b].begin(), pool[b].end(), std::back_inserter(merged));
      const uint32_t id = intern(std::move(merged));
      unions.emplace(key, id);
      return id;
    };
    auto single = [&](uint32_t local) { return intern({local}); };
    using State = std::vector<std::pair<uint32_t, uint32_t>>;
    auto origins_of = [](const State& state, uint32_t local) -> uint32_t {
      auto found = std::lower_bound(state.begin(), state.end(), local, [](const auto& entry, uint32_t key) { return entry.first < key; });
      return found != state.end() && found->first == local ? found->second : 0;
    };
    auto assign = [](State& state, uint32_t local, uint32_t value) {
      auto found = std::lower_bound(state.begin(), state.end(), local, [](const auto& entry, uint32_t key) { return entry.first < key; });
      bool present = found != state.end() && found->first == local;
      if (value == 0) { if (present) state.erase(found); }
      else if (present) found->second = value;
      else state.insert(found, {local, value});
    };
    auto merge = [&](State& state, uint32_t local, uint32_t value) {
      if (value != 0) assign(state, local, join(origins_of(state, local), value));
    };
    auto write = [&](uint32_t origins) {
      auto& written = effects[&fn].writes;
      size_t before = written.size();
      for (uint32_t root : pool[origins]) if (root > 0 && root <= fn.arg_count) written.insert(root - 1);
      if (before != written.size()) changed = true;
    };
    uint32_t all_args = 0;
    for (uint32_t arg = 1; arg <= fn.arg_count; ++arg) all_args = join(all_args, single(arg));
    std::vector<State> incoming(fn.blocks.size());
    for (uint32_t arg = 1; arg <= fn.arg_count; ++arg) incoming[0].push_back({arg, single(arg)});
    // A block is visited again only when its incoming state has grown. Every
    // block is visited once, including unreachable blocks.
    std::vector<char> dirty(fn.blocks.size(), 1);
    bool flow_changed;
    do {
      flow_changed = false;
      for (size_t block = 0; block < fn.blocks.size(); ++block) {
        if (!dirty[block]) continue;
        dirty[block] = 0;
        State state = incoming[block];
        auto operand = [&](const Operand& value) -> uint32_t {
          if (value.kind == Operand::Kind::Const) return 0;
          // A loaded scalar is independent of the address used to read it,
          // including when that operand feeds a cast or arithmetic directly.
          if (!value.place.proj.empty() && projected_type(ctx, fn, value.place)->kind == TypeKind::Prim) return 0;
          return origins_of(state, value.place.local);
        };
        auto store = [&](const Place& place, uint32_t value) {
          if (place.is_none()) return;
          if (place.is_local()) { assign(state, place.local, value); return; }
          uint32_t roots = origins_of(state, place.local); write(roots);
          merge(state, place.local, value);
          for (uint32_t root : pool[roots]) merge(state, root, value);
        };
        for (const auto& statement : fn.blocks[block].stmts) {
          if (statement.kind == Statement::Kind::Drop || statement.kind == Statement::Kind::DropIf) {
            write(origins_of(state, statement.place.local));
            continue;
          }
          if (statement.kind != Statement::Kind::Assign) continue;
          const Rvalue& r = statement.rvalue;
          uint32_t value = 0;
          if (r.kind == RvalueKind::Ref) value = join(origins_of(state, r.place.local), single(r.place.local));
          else if (r.kind == RvalueKind::Len || r.kind == RvalueKind::Discriminant) {}
          else {
            value = join(operand(r.a), operand(r.b));
            for (const auto& part : r.elems) value = join(value, operand(part));
            for (const auto& capture : r.captures) value = join(value, operand(capture));
          }
          if (r.kind == RvalueKind::Aggregate && r.type && r.type->kind == TypeKind::Fn) {
            value = 0;
            for (const auto& capture : r.captures) {
              auto* type = capture.kind == Operand::Kind::Const ? capture.constant.type : projected_type(ctx, fn, capture.place);
              if (loan_carrier_type(ctx, type)) value = join(value, operand(capture));
            }
          }
          auto* destination = projected_type(ctx, fn, statement.place);
          // Loading a number copies its value, rather than borrowing the
          // reference or pointer used to locate it. Address casts retain
          // provenance through arithmetic until converted back to a pointer.
          if (destination->kind == TypeKind::Prim && r.kind == RvalueKind::Use &&
              r.a.kind != Operand::Kind::Const && !r.a.place.proj.empty()) value = 0;
          if (destination->kind == TypeKind::Prim && destination->is_bool()) value = 0;
          if (r.kind == RvalueKind::Cast && destination->kind == TypeKind::Ptr && value == 0 &&
              !(r.a.kind == Operand::Kind::Const && r.a.constant.int_value == 0))
            value = all_args;
          store(statement.place, value);
        }
        const Terminator& term = fn.blocks[block].term;
        if (term.kind == Terminator::Kind::Call) {
          uint32_t result = 0, passed = 0;
          for (const auto& arg : term.args) passed = join(passed, operand(arg));
          auto known = term.callee.kind == Callee::Kind::Direct ? effects.find(term.callee.fn) : effects.end();
          for (size_t arg = 0; arg < term.args.size(); ++arg) {
            const auto& actual = term.args[arg];
            uint32_t origins = operand(actual);
            bool mutates = false;
            if (known != effects.end()) {
              if (known->second.returns.count(static_cast<uint32_t>(arg))) result = join(result, origins);
              mutates = known->second.writes.count(static_cast<uint32_t>(arg)) != 0;
            } else {
              result = join(result, origins);
              auto* type = actual.kind == Operand::Kind::Const ? actual.constant.type : projected_type(ctx, fn, actual.place);
              mutates = loan_carrier_type(ctx, type, true, true);
            }
            if (mutates) {
              write(origins);
              for (uint32_t root : pool[origins]) merge(state, root, passed);
            }
          }
          if (term.callee.kind == Callee::Kind::Indirect) {
            uint32_t origins = operand(term.callee.fn_operand); result = join(result, origins);
            auto* type = projected_type(ctx, fn, term.callee.fn_operand.place);
            if ((type->kind == TypeKind::Ref && !type->is_const) || type->kind == TypeKind::Fn) {
              write(origins);
              for (uint32_t root : pool[origins]) merge(state, root, passed);
            }
          }
          store(term.dest, result);
        }
        if (term.kind == Terminator::Kind::Return) {
          auto& returned = effects[&fn].returns;
          size_t before = returned.size();
          for (uint32_t root : pool[origins_of(state, 0)]) if (root > 0 && root <= fn.arg_count) returned.insert(root - 1);
          if (before != returned.size()) changed = true;
        }
        std::vector<uint32_t> successors;
        if (term.kind == Terminator::Kind::Goto || term.kind == Terminator::Kind::Assert ||
            (term.kind == Terminator::Kind::Call && !term.diverges)) successors.push_back(term.target);
        if (term.kind == Terminator::Kind::Switch) {
          successors.push_back(term.otherwise);
          for (const auto& arm : term.cases) successors.push_back(arm.second);
        }
        for (uint32_t next : successors) for (const auto& [local, origins] : state) {
          uint32_t before = origins_of(incoming[next], local), after = join(before, origins);
          if (after != before) { assign(incoming[next], local, after); flow_changed = true; dirty[next] = 1; }
        }
      }
    } while (flow_changed);
    if (changed) {
      auto found = callers.find(&fn);
      if (found != callers.end())
        for (const Function* caller : found->second) if (queued.insert(caller).second) queue.push_back(caller);
    }
  }
  return effects;
}

class FunctionCheck {
 public:
  FunctionCheck(sema::Program& prog, sema::TypeContext& ctx, DiagnosticSink& sink, Function& f, const WriteEffects* effects)
      : prog_(prog), ctx_(ctx), sink_(sink), f_(f), nlocals_(f.locals.size()), write_effects_(effects) {}

  bool run() {
    for (auto& block : f_.blocks) {
      for (auto& statement : block.stmts) check_readonly_access(statement_accesses(statement), statement.span);
      check_readonly_access(terminator_accesses(block.term), block.term.span);
    }
    compute_successors();
    move_analysis();
    elaborate_drops();
    liveness();
    loans();
    return ok_;
  }

 private:
  // ----- CFG -----
  void compute_successors() {
    succ_.assign(f_.blocks.size(), {});
    for (uint32_t b = 0; b < f_.blocks.size(); ++b) {
      const Terminator& t = f_.blocks[b].term;
      switch (t.kind) {
        case Terminator::Kind::Goto: succ_[b].push_back(t.target); break;
        case Terminator::Kind::Switch:
          for (auto& c : t.cases) succ_[b].push_back(c.second);
          succ_[b].push_back(t.otherwise);
          break;
        case Terminator::Kind::Call: if (!t.diverges) succ_[b].push_back(t.target); break;
        case Terminator::Kind::Assert: succ_[b].push_back(t.target); break;
        default: break;
      }
    }
    // Reverse postorder for forward analyses.
    std::vector<bool> seen(f_.blocks.size(), false);
    std::vector<uint32_t> post;
    std::vector<std::pair<uint32_t, size_t>> stack{{0, 0}};
    seen[0] = true;
    while (!stack.empty()) {
      auto& [b, i] = stack.back();
      if (i < succ_[b].size()) {
        uint32_t s = succ_[b][i++];
        if (!seen[s]) { seen[s] = true; stack.push_back({s, 0}); }
      } else {
        post.push_back(b);
        stack.pop_back();
      }
    }
    rpo_.assign(post.rbegin(), post.rend());
    reachable_ = seen;
  }

  void error(Span span, const std::string& msg, const char* code = "SNX0501") {
    sink_.error(span, msg, code);
    ok_ = false;
  }

  // ----- operands and places used by a statement -----
  struct Access {
    enum Kind { Read, Move, Write, BorrowShared, BorrowMut, Drop, Dead };
    Kind kind;
    Place place;
  };

  void operand_accesses(const Operand& o, std::vector<Access>& out) {
    if (o.kind == Operand::Kind::Copy) out.push_back({Access::Read, o.place});
    else if (o.kind == Operand::Kind::Move) out.push_back({Access::Move, o.place});
    // Index locals inside places are reads too.
    if (o.kind != Operand::Kind::Const) index_reads(o.place, out);
  }

  void check_readonly_access(const std::vector<Access>& accesses, Span span) {
    for (auto& access : accesses) {
      if (access.kind != Access::Write && access.kind != Access::BorrowMut) continue;
      Place prefix = Place::of(access.place.local);
      bool readonly = false;
      for (const auto& projection : access.place.proj) {
        auto* type = place_type(prefix);
        if (projection.kind == ProjKind::Deref) {
          if (type->kind == TypeKind::Ref && type->is_const) readonly = true;
          // A raw dereference is an unsafe operation with its own contract.
          if (type->kind == TypeKind::Ptr) readonly = false;
        }
        prefix.proj.push_back(projection);
      }
      if (readonly) {
        error(span, "cannot change a value through a shared reference", "SNX0505");
        return;
      }
    }
  }

  void index_reads(const Place& p, std::vector<Access>& out) {
    for (auto& pr : p.proj) {
      if (pr.kind == ProjKind::Index) out.push_back({Access::Read, Place::of(pr.index)});
    }
  }

  void rvalue_accesses(const Rvalue& r, std::vector<Access>& out) {
    switch (r.kind) {
      case RvalueKind::Use: case RvalueKind::Unary: case RvalueKind::Cast:
      case RvalueKind::ToDyn: case RvalueKind::StrFromString: case RvalueKind::ArrayToSlice: case RvalueKind::VecToSlice:
        operand_accesses(r.a, out);
        break;
      case RvalueKind::Binary:
        operand_accesses(r.a, out);
        operand_accesses(r.b, out);
        break;
      case RvalueKind::Ref:
        out.push_back({r.is_const ? Access::BorrowShared : Access::BorrowMut, r.place});
        index_reads(r.place, out);
        break;
      case RvalueKind::Aggregate:
        for (auto& e : r.elems) operand_accesses(e, out);
        for (auto& e : r.captures) operand_accesses(e, out);
        break;
      case RvalueKind::Discriminant: case RvalueKind::Len:
        out.push_back({Access::Read, r.place});
        index_reads(r.place, out);
        break;
    }
  }

  std::vector<Access> statement_accesses(const Statement& s) {
    std::vector<Access> out;
    switch (s.kind) {
      case Statement::Kind::Assign:
        rvalue_accesses(s.rvalue, out);
        out.push_back({Access::Write, s.place});
        index_reads(s.place, out);
        break;
      case Statement::Kind::Drop:
      case Statement::Kind::DropIf:
        out.push_back({Access::Drop, s.place});
        break;
      case Statement::Kind::StorageDead:
        out.push_back({Access::Dead, s.place});
        break;
      case Statement::Kind::Nop: break;
    }
    return out;
  }

  std::vector<Access> terminator_accesses(const Terminator& t) {
    std::vector<Access> out;
    switch (t.kind) {
      case Terminator::Kind::Switch: operand_accesses(t.discr, out); break;
      case Terminator::Kind::Assert: operand_accesses(t.cond, out); break;
      case Terminator::Kind::Call:
        if (t.callee.kind == Callee::Kind::Indirect) operand_accesses(t.callee.fn_operand, out);
        for (auto& a : t.args) operand_accesses(a, out);
        if (!t.diverges && !t.dest.is_none()) {
          out.push_back({Access::Write, t.dest});
          index_reads(t.dest, out);
        }
        break;
      case Terminator::Kind::Return:
        if (f_.ret && !f_.ret->is_void()) out.push_back({Access::Read, Place::of(0)});
        break;
      default: break;
    }
    return out;
  }

  // ----- move analysis -----
  void move_analysis() {
    size_t nb = f_.blocks.size();
    std::vector<std::vector<uint8_t>> in(nb, std::vector<uint8_t>(nlocals_, 0));
    // Entry: return slot and user locals uninitialized, arguments initialized.
    std::vector<uint8_t> entry(nlocals_, UNINIT);
    for (uint32_t i = 1; i <= f_.arg_count; ++i) entry[i] = INIT;
    if (f_.ret && f_.ret->is_void()) entry[0] = INIT;  // void has no value to assign
    in[0] = entry;
    std::vector<bool> has_in(nb, false);
    has_in[0] = true;
    bool changed = true;
    while (changed) {
      changed = false;
      for (uint32_t b : rpo_) {
        if (!has_in[b]) continue;
        std::vector<uint8_t> state = in[b];
        transfer_block(b, state, false);
        for (uint32_t s : succ_[b]) {
          if (!has_in[s]) { in[s] = state; has_in[s] = true; changed = true; continue; }
          for (size_t i = 0; i < nlocals_; ++i) {
            uint8_t merged = in[s][i] | state[i];
            if (merged != in[s][i]) { in[s][i] = merged; changed = true; }
          }
        }
      }
    }
    move_in_ = std::move(in);
    // Report errors in a final pass.
    for (uint32_t b : rpo_) {
      std::vector<uint8_t> state = move_in_[b];
      transfer_block(b, state, true);
    }
  }

  // Type of the value a place names, following its projections.
  const sema::Type* place_type(const Place& p) {
    const sema::Type* base = f_.locals[p.local].type;
    for (auto& pr : p.proj) {
      switch (pr.kind) {
        case ProjKind::Deref: case ProjKind::Index:
          if (base->inner) base = base->inner;
          break;
        case ProjKind::Field:
          if (base->kind == TypeKind::Tuple && pr.index < base->elems.size()) base = base->elems[pr.index];
          else if (base->kind == TypeKind::Struct && pr.index < base->struct_def->fields.size()) {
            sema::Subst s;
            for (size_t i = 0; i < base->struct_def->generics.size(); ++i) s.bind(base->struct_def->generics[i].type, base->elems[i]);
            base = s.apply(ctx_, base->struct_def->fields[pr.index].type);
          } else if (base->kind == TypeKind::Enum && current_variant_ < base->enum_def->variants.size()) {
            sema::Subst s;
            for (size_t i = 0; i < base->enum_def->generics.size(); ++i) s.bind(base->enum_def->generics[i].type, base->elems[i]);
            const sema::VariantDef& v = base->enum_def->variants[current_variant_];
            if (pr.index < v.tuple_types.size()) base = s.apply(ctx_, v.tuple_types[pr.index]);
            else if (pr.index < v.fields.size()) base = s.apply(ctx_, v.fields[pr.index].type);
          }
          break;
        case ProjKind::Downcast:
          current_variant_ = pr.index;
          break;
      }
    }
    return base;
  }
  size_t current_variant_ = 0;

  void note_use(const Place& p, Access::Kind kind, Span span, std::vector<uint8_t>& state, bool report) {
    uint32_t l = p.local;
    uint8_t st = state[l];
    const sema::Type* t = place_type(p);
    bool through_ref = starts_with_deref(p);
    if (report) {
      std::string what = place_text(f_, p);
      if (kind == Access::Drop) {
        // handled by elaboration
      } else if (st == MOVED) {
        error(span, "use of moved value " + what + "; it was moved earlier");
      } else if (st == UNINIT) {
        error(span, what + " is used before it is assigned");
      } else if (st & MOVED) {
        error(span, what + " may have been moved on another path");
      } else if (st & UNINIT) {
        error(span, what + " may not be assigned on every path");
      }
      if (kind == Access::Move && through_ref && !deref_is_raw(f_, p) && !is_copy(prog_, ctx_, t)) {
        error(span, "cannot move " + what + " out of a reference; it is borrowed, not owned");
      }
      if (kind == Access::Move && !through_ref && !is_copy(prog_, ctx_, t) &&
          std::any_of(p.proj.begin(), p.proj.end(), [](const Projection& pr) { return pr.kind == ProjKind::Index; })) {
        // The other elements would have no owner left to drop them.
        error(span, "cannot move an element out of " + place_text(f_, Place::of(l)) +
                        "; use swap, pop, or std.mem.replace", "SNX0508");
      }
      if (kind == Access::Move && !p.proj.empty() && !through_ref && !is_copy(prog_, ctx_, t) && needs_drop_local(l)) {
        // Moving out of a field of a value with a destructor leaves it half
        // alive. Allowed only when the whole local is treated as moved.
      }
    }
    if (kind == Access::Move && !through_ref) state[l] = MOVED;
  }

  bool needs_drop_local(uint32_t l) {
    for (auto& b : f_.blocks) {
      for (auto& s : b.stmts) {
        if ((s.kind == Statement::Kind::Drop || s.kind == Statement::Kind::DropIf) && s.place.local == l) return true;
      }
    }
    return false;
  }

  void transfer_block(uint32_t b, std::vector<uint8_t>& state, bool report) {
    BasicBlock& blk = f_.blocks[b];
    for (auto& s : blk.stmts) {
      if (s.kind == Statement::Kind::Assign) {
        for (auto& a : statement_accesses(s)) {
          if (a.kind == Access::Write) {
            if (starts_with_deref(a.place)) note_use(Place::of(a.place.local), Access::Read, s.span, state, report);
            continue;
          }
          note_use(a.place, a.kind, s.span, state, report);
        }
        if (s.place.is_local()) {
          state[s.place.local] = INIT;
        } else {
          // Writing a field needs the base initialized.
          if (report && !starts_with_deref(s.place) && !(state[s.place.local] & INIT)) {
            error(s.span, place_text(f_, s.place) + " is assigned a field before the whole value is assigned");
          }
        }
      } else if (s.kind == Statement::Kind::StorageDead) {
        state[s.place.local] = UNINIT;
      } else if (s.kind == Statement::Kind::Drop) {
        // Nothing: elaboration decides.
      }
    }
    Terminator& t = blk.term;
    for (auto& a : terminator_accesses(t)) {
      if (a.kind == Access::Write) {
        if (starts_with_deref(a.place)) note_use(Place::of(a.place.local), Access::Read, t.span, state, report);
        continue;
      }
      note_use(a.place, a.kind, t.span, state, report);
    }
    if (t.kind == Terminator::Kind::Call && !t.diverges && !t.dest.is_none() && t.dest.is_local()) {
      state[t.dest.local] = INIT;
    }
  }

  // ----- drop elaboration -----
  size_t predecessors(uint32_t block) const {
    size_t count = 0;
    for (const auto& targets : succ_) count += static_cast<size_t>(std::count(targets.begin(), targets.end(), block));
    return count;
  }

  // Moving a field out of an initialized local makes the whole local unusable
  // (later uses and borrows are errors), so its scope-end drop is removed.
  // Returns drops for every non-Copy part that no move in this statement or
  // terminator took. Paths through Deref or Index are not owned parts of the
  // local here; those values keep the previous behavior.
  std::vector<Statement> partial_move_drops(const std::vector<Access>& accesses, const std::vector<uint8_t>& state, Span span) {
    std::map<uint32_t, std::vector<const Place*>> moved;
    for (const auto& a : accesses) {
      if (a.kind != Access::Move || a.place.is_local() || starts_with_deref(a.place)) continue;
      if (state[a.place.local] != INIT) continue;
      moved[a.place.local].push_back(&a.place);
    }
    std::vector<Statement> out;
    for (auto& [local, paths] : moved) {
      // A whole-local move in the same access list moves everything.
      bool whole = false;
      for (const auto& a : accesses) if (a.kind == Access::Move && a.place.is_local() && a.place.local == local) whole = true;
      if (!whole) remaining_parts(Place::of(local), paths, 0, span, out);
    }
    return out;
  }

  void remaining_parts(const Place& prefix, const std::vector<const Place*>& paths, size_t depth, Span span, std::vector<Statement>& out) {
    for (auto* p : paths) if (p->proj.size() == depth) return;  // this part moved as a whole
    const auto& next = paths.front()->proj[depth];
    if (next.kind == ProjKind::Deref || next.kind == ProjKind::Index) return;
    if (next.kind == ProjKind::Downcast) {
      for (auto* p : paths) if (p->proj[depth].kind != ProjKind::Downcast || p->proj[depth].index != next.index) return;
      remaining_parts(prefix.downcast(next.index), paths, depth + 1, span, out);
      return;
    }
    const sema::Type* type = projected_type(ctx_, f_, prefix);
    size_t count = 0;
    if (type->kind == TypeKind::Tuple) count = type->elems.size();
    else if (type->kind == TypeKind::Struct) count = type->struct_def->fields.size();
    else if (type->kind == TypeKind::Enum && !prefix.proj.empty() && prefix.proj.back().kind == ProjKind::Downcast) {
      const auto& variant = type->enum_def->variants[prefix.proj.back().index];
      count = variant.tuple_types.empty() ? variant.fields.size() : variant.tuple_types.size();
    } else return;
    for (uint32_t j = 0; j < count; ++j) {
      std::vector<const Place*> below;
      for (auto* p : paths) {
        const auto& step = p->proj[depth];
        if (step.kind == ProjKind::Field && step.index == j) below.push_back(p);
      }
      Place part = prefix.field(j);
      if (!below.empty()) { remaining_parts(part, below, depth + 1, span, out); continue; }
      if (is_copy(prog_, ctx_, projected_type(ctx_, f_, part))) continue;
      Statement drop;
      drop.kind = Statement::Kind::Drop;
      drop.place = part;
      drop.span = span;
      out.push_back(std::move(drop));
    }
  }

  void elaborate_drops() {
    // Recompute state per statement and rewrite drops.
    std::map<uint32_t, uint32_t> flags;  // local -> flag local
    std::map<uint32_t, std::vector<Statement>> continuation_drops;  // block -> remaining-field drops
    for (uint32_t local : f_.async_drop_locals) {
      Local flag; flag.type = ctx_.bool_type(); flag.span = f_.locals[local].span;
      flags[local] = static_cast<uint32_t>(f_.locals.size());
      f_.locals.push_back(flag);
    }
    for (uint32_t b : rpo_) {
      std::vector<uint8_t> state = move_in_[b];
      BasicBlock& blk = f_.blocks[b];
      std::vector<Statement> rewritten;
      for (auto& s : blk.stmts) {
        if (s.kind == Statement::Kind::Drop) {
          uint32_t l = s.place.local;
          uint8_t st = state[l];
          if (f_.async_body && s.place.is_local() && flags.count(l)) {
            if (!(st & INIT)) continue;
            Statement drop = s; drop.kind = Statement::Kind::DropIf; drop.flag_local = flags[l];
            rewritten.push_back(drop);
            continue;
          }
          if (st == INIT) {
            rewritten.push_back(s);
          } else if (st == MOVED || st == UNINIT || st == (MOVED | UNINIT)) {
            // Nothing to drop.
          } else {
            uint32_t flag;
            auto it = flags.find(l);
            if (it == flags.end()) {
              Local fl;
              fl.type = ctx_.bool_type();
              fl.name = "";
              fl.span = f_.locals[l].span;
              f_.locals.push_back(fl);
              flag = static_cast<uint32_t>(f_.locals.size() - 1);
              flags[l] = flag;
            } else {
              flag = it->second;
            }
            Statement d = s;
            d.kind = Statement::Kind::DropIf;
            d.flag_local = flag;
            rewritten.push_back(d);
          }
          continue;
        }
        rewritten.push_back(s);
        if (s.kind == Statement::Kind::Assign) {
          auto accesses = statement_accesses(s);
          std::vector<Statement> remaining = partial_move_drops(accesses, state, s.span);
          for (auto& a : accesses) {
            if (a.kind == Access::Move && !starts_with_deref(a.place)) state[a.place.local] = MOVED;
          }
          // A destination that overwrites the moved local holds a new value.
          for (auto& d : remaining) if (!(s.place.is_local() && s.place.local == d.place.local)) rewritten.push_back(std::move(d));
          if (s.place.is_local()) state[s.place.local] = INIT;
        }
      }
      blk.stmts = std::move(rewritten);
      Terminator& t = blk.term;
      auto accesses = terminator_accesses(t);
      std::vector<Statement> remaining = partial_move_drops(accesses, state, t.span);
      for (auto& a : accesses) {
        if (a.kind == Access::Move && !starts_with_deref(a.place)) state[a.place.local] = MOVED;
      }
      if (!remaining.empty()) {
        // Call arguments may still borrow the remaining fields; drop them in
        // the continuation when it has no other predecessor.
        bool continuation = t.kind == Terminator::Kind::Call && !t.diverges && predecessors(t.target) == 1;
        if (continuation) {
          auto& pending = continuation_drops[t.target];
          pending.insert(pending.end(), remaining.begin(), remaining.end());
        } else if (t.kind != Terminator::Kind::Call || !t.diverges) {
          blk.stmts.insert(blk.stmts.end(), remaining.begin(), remaining.end());
        }
      }
    }
    for (auto& [block, drops] : continuation_drops) {
      auto& stmts = f_.blocks[block].stmts;
      stmts.insert(stmts.begin(), drops.begin(), drops.end());
    }
    if (f_.async_body) f_.async_drop_flags = flags;
    if (flags.empty()) return;
    // Maintain the flags: set on init, clear on move, false at entry.
    nlocals_ = f_.locals.size();
    auto set_flag = [&](uint32_t flag, bool value, Span span) {
      Statement s;
      s.kind = Statement::Kind::Assign;
      s.place = Place::of(flag);
      Constant k;
      k.kind = Constant::Kind::Bool;
      k.int_value = value ? 1 : 0;
      k.type = ctx_.bool_type();
      s.rvalue.kind = RvalueKind::Use;
      s.rvalue.a = Operand::constant_of(k);
      s.span = span;
      return s;
    };
    for (uint32_t b = 0; b < f_.blocks.size(); ++b) {
      BasicBlock& blk = f_.blocks[b];
      std::vector<Statement> rewritten;
      if (b == 0) {
        for (auto& [l, flag] : flags) rewritten.push_back(set_flag(flag, l >= 1 && l <= f_.arg_count, f_.locals[l].span));
      }
      for (auto& s : blk.stmts) {
        if (s.kind == Statement::Kind::Assign) {
          for (auto& a : statement_accesses(s)) {
            if (a.kind == Access::Move && !starts_with_deref(a.place)) {
              auto it = flags.find(a.place.local);
              if (it != flags.end()) rewritten.push_back(set_flag(it->second, false, s.span));
            }
          }
          rewritten.push_back(s);
          if (s.place.is_local()) {
            auto it = flags.find(s.place.local);
            if (it != flags.end()) rewritten.push_back(set_flag(it->second, true, s.span));
          }
          continue;
        }
        if (s.kind == Statement::Kind::DropIf) {
          rewritten.push_back(s);
          rewritten.push_back(set_flag(s.flag_local, false, s.span));
          continue;
        }
        rewritten.push_back(s);
      }
      Terminator& t = blk.term;
      for (auto& a : terminator_accesses(t)) {
        if (a.kind == Access::Move && !starts_with_deref(a.place)) {
          auto it = flags.find(a.place.local);
          if (it != flags.end()) rewritten.push_back(set_flag(it->second, false, t.span));
        }
      }
      blk.stmts = std::move(rewritten);
      if (t.kind == Terminator::Kind::Call && !t.diverges && !t.dest.is_none() && t.dest.is_local()) {
        auto it = flags.find(t.dest.local);
        if (it != flags.end()) {
          // The flag is set at the start of the continuation block.
          f_.blocks[t.target].stmts.insert(f_.blocks[t.target].stmts.begin(), set_flag(it->second, true, t.span));
        }
      }
    }
  }

  // ----- liveness -----
  void liveness() {
    size_t nb = f_.blocks.size();
    live_in_.assign(nb, std::vector<bool>(nlocals_, false));
    std::vector<std::vector<bool>> live_out(nb, std::vector<bool>(nlocals_, false));
    bool changed = true;
    while (changed) {
      changed = false;
      for (auto it = rpo_.rbegin(); it != rpo_.rend(); ++it) {
        uint32_t b = *it;
        std::vector<bool> out(nlocals_, false);
        for (uint32_t s : succ_[b]) for (size_t i = 0; i < nlocals_; ++i) if (live_in_[s][i]) out[i] = true;
        live_out[b] = out;
        std::vector<bool> live = out;
        // Backward through the terminator and statements.
        apply_backward(f_.blocks[b], live);
        if (live != live_in_[b]) { live_in_[b] = live; changed = true; }
      }
    }
    live_out_ = std::move(live_out);
  }

  void uses_and_defs(const std::vector<Access>& accesses, std::vector<uint32_t>& uses, std::vector<uint32_t>& defs) {
    for (auto& a : accesses) {
      if ((a.kind == Access::Write || a.kind == Access::Dead) && a.place.is_local()) defs.push_back(a.place.local);
      else uses.push_back(a.place.local);
    }
  }

  void apply_backward(const BasicBlock& blk, std::vector<bool>& live) {
    std::vector<uint32_t> uses, defs;
    uses_and_defs(terminator_accesses(blk.term), uses, defs);
    if (blk.term.kind == Terminator::Kind::Return) uses.push_back(0);
    for (uint32_t d : defs) live[d] = false;
    for (uint32_t u : uses) live[u] = true;
    for (auto it = blk.stmts.rbegin(); it != blk.stmts.rend(); ++it) {
      uses.clear();
      defs.clear();
      uses_and_defs(statement_accesses(*it), uses, defs);
      if (it->kind == Statement::Kind::DropIf) uses.push_back(it->flag_local);
      for (uint32_t d : defs) live[d] = false;
      for (uint32_t u : uses) live[u] = true;
    }
  }

  // Live-before sets for every point in a block, computed on demand.
  std::vector<std::vector<bool>> live_before_points(uint32_t b) {
    const BasicBlock& blk = f_.blocks[b];
    std::vector<std::vector<bool>> out(blk.stmts.size() + 1);
    std::vector<bool> live = live_out_[b];
    std::vector<uint32_t> uses, defs;
    uses_and_defs(terminator_accesses(blk.term), uses, defs);
    if (blk.term.kind == Terminator::Kind::Return) uses.push_back(0);
    out[blk.stmts.size()] = live;  // live after the last statement = before the terminator? compute properly below
    // live before terminator:
    {
      std::vector<bool> l = live;
      for (uint32_t d : defs) l[d] = false;
      for (uint32_t u : uses) l[u] = true;
      out[blk.stmts.size()] = l;
      live = l;
    }
    for (size_t i = blk.stmts.size(); i-- > 0;) {
      uses.clear();
      defs.clear();
      uses_and_defs(statement_accesses(blk.stmts[i]), uses, defs);
      if (blk.stmts[i].kind == Statement::Kind::DropIf) uses.push_back(blk.stmts[i].flag_local);
      std::vector<bool> l = live;
      for (uint32_t d : defs) l[d] = false;
      for (uint32_t u : uses) l[u] = true;
      out[i] = l;
      live = l;
    }
    return out;
  }

  // ----- loans -----
  using Carriers = std::vector<std::set<uint32_t>>;  // per local: loan ids

  std::vector<const sema::Type*> value_parts(const sema::Type* t) {
    std::vector<const sema::Type*> parts;
    if (t->kind == TypeKind::Fn && t->is_send) return parts;
    if (t->kind == TypeKind::Array || t->kind == TypeKind::Slice) parts.push_back(t->inner);
    if (t->kind == TypeKind::Fn && t->ret) parts.push_back(t->ret);
    if (t->kind == TypeKind::Tuple || t->kind == TypeKind::Fn || t->kind == TypeKind::Struct || t->kind == TypeKind::Enum)
      parts.insert(parts.end(), t->elems.begin(), t->elems.end());
    sema::Subst subst;
    if (t->kind == TypeKind::Struct) {
      for (size_t i = 0; i < t->elems.size(); ++i) subst.bind(t->struct_def->generics[i].type, t->elems[i]);
      for (auto& field : t->struct_def->fields) parts.push_back(subst.apply(ctx_, field.type));
    } else if (t->kind == TypeKind::Enum) {
      for (size_t i = 0; i < t->elems.size(); ++i) subst.bind(t->enum_def->generics[i].type, t->elems[i]);
      for (auto& variant : t->enum_def->variants) {
        for (auto* field : variant.tuple_types) parts.push_back(subst.apply(ctx_, field));
        for (auto& field : variant.fields) parts.push_back(subst.apply(ctx_, field.type));
      }
    }
    return parts;
  }

  bool type_has_ref(const sema::Type* t) {
    auto cached = has_refs_cache_.find(t);
    if (cached != has_refs_cache_.end()) return cached->second;
    std::vector<const sema::Type*> work{t};
    std::set<const sema::Type*> seen;
    while (!work.empty()) {
      auto* type = work.back(); work.pop_back();
      if (!seen.insert(type).second) continue;
      if (type->kind == TypeKind::Ref || type->kind == TypeKind::Dyn) return has_refs_cache_[t] = true;
      if (type->kind == TypeKind::Fn) { if (!type->is_send) return has_refs_cache_[t] = true; continue; }
      auto parts = value_parts(type);
      work.insert(work.end(), parts.begin(), parts.end());
    }
    return has_refs_cache_[t] = false;
  }

  bool external_mutability(const sema::Type* type) { return loan_carrier_type(ctx_, type, true); }

  bool stores_reference(const sema::Type* container, const sema::Type* ref) {
    std::vector<const sema::Type*> work{container};
    std::set<const sema::Type*> seen;
    while (!work.empty()) {
      auto* type = work.back(); work.pop_back();
      if (!seen.insert(type).second) continue;
      if (type->kind == TypeKind::Dyn) return true;
      if (type->kind == TypeKind::Ref) {
        if (type->inner == ref->inner) return true;
        continue;
      }
      auto parts = value_parts(type);
      work.insert(work.end(), parts.begin(), parts.end());
    }
    return false;
  }

  bool carries_refs(uint32_t l) { return type_has_ref(f_.locals[l].type); }

  void loans() {
    // Assign loan ids to every Ref rvalue.
    for (uint32_t b = 0; b < f_.blocks.size(); ++b) {
      for (auto& s : f_.blocks[b].stmts) {
        if (s.kind == Statement::Kind::Assign && s.rvalue.kind == RvalueKind::Ref) {
          Loan l;
          l.place = s.rvalue.place;
          l.is_mut = !s.rvalue.is_const;
          l.span = s.span;
          l.ref_local = s.place.local;
          loan_list_.push_back(l);
        }
      }
    }
    // A writable reference may be moved or wrapped before it is used.
    // Seed an external origin so stores through every alias still reach the
    // original parameter/capture boundary and are checked at Return.
    for (uint32_t local = 1; local < nlocals_; ++local) {
      const Local& value = f_.locals[local];
      if (!(value.is_arg || value.escape_boundary) || !external_mutability(value.type)) continue;
      Loan origin;
      origin.place = value.type->kind == TypeKind::Ref ? Place::of(local).deref() : Place::of(local);
      origin.is_mut = true; origin.span = value.span; origin.ref_local = local; origin.external = true;
      boundary_origins_[local] = static_cast<uint32_t>(loan_list_.size());
      loan_list_.push_back(origin);
    }
    if (loan_list_.empty()) return;

    // Forward dataflow of carriers.
    size_t nb = f_.blocks.size();
    std::vector<Carriers> in(nb, Carriers(nlocals_));
    for (const auto& [local, origin] : boundary_origins_)
      if (f_.locals[local].is_arg) in[0][local].insert(origin);
    bool changed = true;
    while (changed) {
      changed = false;
      provenance_changed_ = false;
      for (uint32_t b : rpo_) {
        Carriers state = in[b];
        transfer_carriers(b, state);
        for (uint32_t s : succ_[b]) {
          for (size_t i = 0; i < nlocals_; ++i) {
            size_t before = in[s][i].size();
            in[s][i].insert(state[i].begin(), state[i].end());
            if (in[s][i].size() != before) changed = true;
          }
        }
      }
      changed = changed || provenance_changed_;
    }
    carriers_in_ = std::move(in);

    // Conflict checking at every point.
    uint32_t loan_id = 0;
    (void)loan_id;
    for (uint32_t b : rpo_) {
      Carriers state = carriers_in_[b];
      std::vector<std::vector<bool>> live = live_before_points(b);
      BasicBlock& blk = f_.blocks[b];
      for (size_t i = 0; i < blk.stmts.size(); ++i) {
        check_point(blk.stmts[i].span, statement_accesses(blk.stmts[i]), state, live[i], blk.stmts[i].kind == Statement::Kind::Drop || blk.stmts[i].kind == Statement::Kind::DropIf);
        step_carriers_stmt(blk.stmts[i], state);
      }
      check_point(blk.term.span, terminator_accesses(blk.term), state, live[blk.stmts.size()], false);
      if (blk.term.kind == Terminator::Kind::Return) check_return(state, blk.term.span);
      if (f_.async_suspends.count(b)) {
        auto& edges = f_.async_borrows[b];
        auto& retained = f_.async_live[b];
        for (uint32_t local = 1; local < nlocals_; ++local)
          if (live[blk.stmts.size()][local]) retained.insert(local);
        for (uint32_t holder = 1; holder < nlocals_; ++holder) {
          for (uint32_t id : state[holder]) {
            const auto& loan = loan_list_[id];
            if (!loan.external && !starts_with_deref(loan.place) && holder != loan.place.local)
              edges.insert({holder, loan.place.local});
          }
        }
        // Cancellation can run user destructors that store held loans into
        // external writable roots, then destroys the frame's local storage.
        auto cancelled = state;
        for (uint32_t local : f_.async_drop_locals) {
          auto held = cancelled[local];
          retain_external_aliases(held, held, cancelled);
        }
        check_return(cancelled, blk.term.span);
      }
      step_carriers_term(blk.term, state);
    }
  }

  std::set<uint32_t> operand_loans(const Operand& o, const Carriers& state) {
    if (o.kind == Operand::Kind::Const) return {};
    return state[o.place.local];
  }

  uint32_t loan_index_for(const Statement& s) {
    for (uint32_t i = 0; i < loan_list_.size(); ++i) {
      if (loan_list_[i].span.start == s.span.start && loan_list_[i].span.end == s.span.end &&
          loan_list_[i].ref_local == s.place.local && overlaps(loan_list_[i].place, s.rvalue.place) &&
          loan_list_[i].place.proj.size() == s.rvalue.place.proj.size()) return i;
    }
    return UINT32_MAX;
  }

  void retain_in_roots(uint32_t carrier, const std::set<uint32_t>& receiver_loans,
                       const std::set<uint32_t>& stored, Carriers& state, bool erased = false) {
    std::set<uint32_t> roots{carrier};
    for (uint32_t id : receiver_loans) {
      const auto& loan = loan_list_[id];
      if (erased || type_has_ref(place_type(loan.place))) roots.insert(loan.place.local);
    }
    for (uint32_t root : roots) for (uint32_t id : stored) {
      if (receiver_loans.count(id)) continue;
      state[root].insert(id);
    }
  }

  void retain_external_aliases(const std::set<uint32_t>& receiver_loans,
                               const std::set<uint32_t>& stored, Carriers& state) {
    for (uint32_t id : receiver_loans) {
      const Loan& receiver = loan_list_[id];
      if (!receiver.is_mut || !type_has_ref(place_type(receiver.place))) continue;
      uint32_t root = receiver.place.local;
      for (uint32_t value : stored) {
        const Loan& loan = loan_list_[value];
        if (loan.place.local != root || starts_with_deref(loan.place)) state[root].insert(value);
      }
    }
  }

  void step_carriers_stmt(const Statement& s, Carriers& state) {
    if (s.kind == Statement::Kind::StorageDead) { state[s.place.local].clear(); return; }
    if (s.kind == Statement::Kind::Drop || s.kind == Statement::Kind::DropIf) {
      auto stored = state[s.place.local];
      retain_external_aliases(stored, stored, state);
      return;
    }
    if (s.kind != Statement::Kind::Assign) return;
    std::set<uint32_t> incoming;
    const Rvalue& r = s.rvalue;
    switch (r.kind) {
      case RvalueKind::Ref: {
        uint32_t id = loan_index_for(s);
        if (id != UINT32_MAX) incoming.insert(id);
        // Reborrowing through a reference keeps the original loans too.
        if (starts_with_deref(r.place) || type_has_ref(place_type(r.place))) {
          incoming.insert(state[r.place.local].begin(), state[r.place.local].end());
          // A second dereference follows a reference stored in the object,
          // rather than pointing into the object itself. Closure environment
          // projections are erased captures, so keep their conservative roots.
          size_t derefs = 0;
          for (const auto& projection : r.place.proj) if (projection.kind == ProjKind::Deref) ++derefs;
          bool within_owner = derefs == 1 || (f_.closure && r.place.local == 1);
          if (id != UINT32_MAX && starts_with_deref(r.place) && within_owner && !deref_is_raw(f_, r.place)) {
            auto& bases = loan_bases_[id];
            size_t before = bases.size();
            bases.insert(state[r.place.local].begin(), state[r.place.local].end());
            provenance_changed_ = provenance_changed_ || bases.size() != before;
          }
        }
        break;
      }
      case RvalueKind::Use: case RvalueKind::Cast: case RvalueKind::Unary:
      case RvalueKind::ToDyn: case RvalueKind::StrFromString: case RvalueKind::ArrayToSlice: case RvalueKind::VecToSlice:
        incoming = operand_loans(r.a, state);
        break;
      case RvalueKind::Binary:
        break;  // results are plain values
      case RvalueKind::Aggregate:
        for (auto& e : r.elems) { auto l = operand_loans(e, state); incoming.insert(l.begin(), l.end()); }
        for (auto& e : r.captures) { auto l = operand_loans(e, state); incoming.insert(l.begin(), l.end()); }
        break;
      default: break;
    }
    if (!type_has_ref(place_type(s.place))) incoming.clear();
    if (s.place.is_local()) {
      auto origin = boundary_origins_.find(s.place.local);
      if (origin != boundary_origins_.end()) incoming.insert(origin->second);
      state[s.place.local] = incoming;
    }
    else if (starts_with_deref(s.place) && !deref_is_raw(f_, s.place) && type_has_ref(place_type(s.place))) {
      auto receiver_loans = state[s.place.local];
      record_stored_loans(receiver_loans, incoming);
      retain_in_roots(s.place.local, receiver_loans, incoming, state);
    } else state[s.place.local].insert(incoming.begin(), incoming.end());
  }

  void record_stored_loans(const std::set<uint32_t>& roots, const std::set<uint32_t>& values) {
    for (uint32_t id : roots) {
      if (!loan_list_[id].external) continue;
      auto& stored = boundary_stores_[loan_list_[id].ref_local];
      size_t before = stored.size();
      stored.insert(values.begin(), values.end());
      provenance_changed_ = provenance_changed_ || stored.size() != before;
    }
  }

  void step_carriers_term(const Terminator& t, Carriers& state) {
    if (t.kind != Terminator::Kind::Call || t.diverges) return;
    // A call can retain reference arguments in a writable container. Carry
    // those loans to the container even when the call returns void. Until
    // function effects exist this is conservative for every such call.
    const Carriers before = state;
    // A writable closure can store argument references in its captures.
    // Its borrowed function value is an implicit writable receiver.
    std::vector<const Operand*> receivers;
    for (auto& arg : t.args) receivers.push_back(&arg);
    if (t.callee.kind == Callee::Kind::Indirect) receivers.push_back(&t.callee.fn_operand);
    for (const Operand* candidate : receivers) {
      const Operand& receiver = *candidate;
      if (write_effects_ && t.callee.kind == Callee::Kind::Direct && candidate != &t.callee.fn_operand) {
        auto effect = write_effects_->find(t.callee.fn);
        if (effect != write_effects_->end()) {
          uint32_t index = static_cast<uint32_t>(candidate - t.args.data());
          if (!effect->second.writes.count(index)) continue;
        }
      }
      if (receiver.kind == Operand::Kind::Const) continue;
      const sema::Type* rt = place_type(receiver.place);
      if (rt->kind == TypeKind::Ref) {
        if (rt->is_const || !type_has_ref(rt->inner)) continue;
      } else if (!external_mutability(rt)) continue;
      auto receiver_loans = operand_loans(receiver, before);
      std::set<uint32_t> stored;
      std::set<uint32_t> passed_values;
      for (uint32_t id : receiver_loans) {
        // The temporary borrow used to access a receiver is not a reference
        // stored in its payload. Retaining it would make an element borrow
        // escape into its own container after a call.
        if (rt->kind != TypeKind::Ref || loan_list_[id].ref_local != receiver.place.local) stored.insert(id);
      }
      for (size_t j = 0; j < t.args.size(); ++j) {
        if (candidate == &t.args[j]) continue;
        const Operand& value = t.args[j];
        const sema::Type* vt = value.kind == Operand::Kind::Const ? value.constant.type : place_type(value.place);
        if (!type_has_ref(vt)) continue;
        auto loans = operand_loans(value, before);
        if (rt->kind == TypeKind::Ref && vt->kind == TypeKind::Ref && type_has_ref(vt->inner) && !stores_reference(rt->inner, vt)) {
          // append(&other) transfers contained references, not a borrow of
          // the other container's own storage.
          for (uint32_t id : loans) {
            const auto& contents = before[loan_list_[id].place.local];
            stored.insert(contents.begin(), contents.end());
            passed_values.insert(contents.begin(), contents.end());
          }
        } else {
          stored.insert(loans.begin(), loans.end());
          passed_values.insert(loans.begin(), loans.end());
        }
      }
      record_stored_loans(receiver_loans, passed_values);
      retain_in_roots(receiver.place.local, receiver_loans, stored, state, rt->kind == TypeKind::Ref && rt->inner->kind == TypeKind::Dyn);
      retain_external_aliases(receiver_loans, stored, state);
    }
    if (t.dest.is_none()) return;
    std::set<uint32_t> incoming;
    if (type_has_ref(dest_type(t))) {
      for (size_t arg = 0; arg < t.args.size(); ++arg) {
        const auto& value = t.args[arg];
        auto* type = value.kind == Operand::Kind::Const ? value.constant.type : place_type(value.place);
        if (!type_has_ref(type)) continue;
        if (write_effects_ && t.callee.kind == Callee::Kind::Direct) {
          auto effect = write_effects_->find(t.callee.fn);
          if (effect != write_effects_->end() && !effect->second.returns.count(static_cast<uint32_t>(arg))) continue;
        }
        auto loans = operand_loans(value, state);
        // A compiler-created Task cannot return a borrow of its frame: its
        // logical body is checked before frame lowering. Its poll result
        // borrows captured inputs, independently of the movable handle.
        bool task_poll = arg == 0 && t.callee.kind == Callee::Kind::Direct && t.callee.fn->def &&
                         t.callee.fn->def->is_prelude && t.callee.fn->def->name == "poll" &&
                         t.callee.fn->def->owner == sema::FnOwner::Impl &&
                         t.callee.fn->def->self_type->kind == TypeKind::Struct &&
                         (t.callee.fn->def->self_type->struct_def == prog_.task_def || t.callee.fn->def->self_type->struct_def == prog_.send_task_def);
        if (task_poll && value.kind != Operand::Kind::Const) {
          for (auto it = loans.begin(); it != loans.end();) {
            if (!loan_list_[*it].external && loan_list_[*it].ref_local == value.place.local) it = loans.erase(it);
            else ++it;
          }
        }
        incoming.insert(loans.begin(), loans.end());
      }
      if (t.callee.kind == Callee::Kind::Indirect) {
        auto loans = operand_loans(t.callee.fn_operand, state);
        incoming.insert(loans.begin(), loans.end());
      }
    }
    if (t.dest.is_local()) state[t.dest.local] = incoming;
    else if (starts_with_deref(t.dest) && !deref_is_raw(f_, t.dest) && type_has_ref(dest_type(t))) {
      auto receiver_loans = state[t.dest.local];
      retain_in_roots(t.dest.local, receiver_loans, incoming, state);
    } else state[t.dest.local].insert(incoming.begin(), incoming.end());
  }

  const sema::Type* dest_type(const Terminator& t) { return place_type(t.dest); }

  void transfer_carriers(uint32_t b, Carriers& state) {
    for (auto& s : f_.blocks[b].stmts) step_carriers_stmt(s, state);
    step_carriers_term(f_.blocks[b].term, state);
  }

  // Active loans at a point: carried by a live local.
  std::vector<std::pair<uint32_t, uint32_t>> active_loans(const Carriers& state, const std::vector<bool>& live) {
    std::vector<std::pair<uint32_t, uint32_t>> out;  // loan id, carrier local
    for (uint32_t l = 0; l < nlocals_; ++l) {
      if (!live[l]) continue;
      for (uint32_t id : state[l]) out.push_back({id, l});
    }
    return out;
  }

  void check_point(Span span, const std::vector<Access>& accesses, const Carriers& state,
                   const std::vector<bool>& live, bool is_drop) {
    auto active = active_loans(state, live);
    if (active.empty()) return;
    for (auto& a : accesses) {
      for (auto& [id, carrier] : active) {
        const Loan& loan = loan_list_[id];
        if (!overlaps(loan.place, a.place)) continue;
        // Ending a reference variable's storage leaves its pointee alive.
        // Reborrows keep the original pointee loans too, so a local referent
        // still cannot escape through a short-lived reference alias.
        if (a.kind == Access::Dead && a.place.is_local() && starts_with_deref(loan.place)) continue;
        // Moving a reference carrier is fine, but moving the borrowed owner
        // itself is not. A call with disjoint field arguments can store a
        // loan back into the containing local, making it its own carrier.
        bool ends_owner = !loan.external && a.place.is_local() && a.place.local == loan.place.local &&
                          (a.kind == Access::Move || a.kind == Access::Drop || a.kind == Access::Dead);
        if (!ends_owner && (a.place.local == carrier || state[a.place.local].count(id))) continue;
        std::string what = place_text(f_, loan.place);
        switch (a.kind) {
          case Access::Read:
            if (loan.is_mut) {
              error(span, "cannot read " + what + " while it is borrowed as writable");
              sink_.note(loan.span, "the writable borrow is here and is still in use");
              return;
            }
            break;
          case Access::Move:
            error(span, "cannot move " + what + " while it is borrowed");
            sink_.note(loan.span, "the borrow is here and is still in use");
            return;
          case Access::Write:
            error(span, "cannot assign to " + what + " while it is borrowed");
            sink_.note(loan.span, "the borrow is here and is still in use");
            return;
          case Access::BorrowShared:
            if (loan.is_mut) {
              error(span, "cannot borrow " + what + " as read only while it is borrowed as writable");
              sink_.note(loan.span, "the writable borrow is here and is still in use");
              return;
            }
            break;
          case Access::BorrowMut:
            error(span, "cannot borrow " + what + " as writable while it is already borrowed");
            sink_.note(loan.span, "the other borrow is here and is still in use");
            return;
          case Access::Dead:
            error(span, what + " does not live past this scope; a reference is still in use", "SNX0504");
            sink_.note(loan.span, "the borrow is here");
            return;
          case Access::Drop:
            if (is_drop) {
              error(span, what + " is dropped while it is still borrowed");
              sink_.note(loan.span, "the borrow is here and is still in use");
              return;
            }
            break;
        }
      }
    }
  }

  void check_return(const Carriers& state, Span span) {
    auto check_escape = [&](uint32_t local) {
      const Loan* self_reference = nullptr;
      auto escaping = state[local];
      // Rebinding a parameter does not undo writes made through its previous
      // value. Keep those writes checked after aliases or parameters die.
      if (local != 0) {
        const auto& stored = boundary_stores_[local];
        escaping.insert(stored.begin(), stored.end());
      }
      for (uint32_t id : escaping) {
        const Loan& loan = loan_list_[id];
        if (loan.external) continue;
        if (starts_with_deref(loan.place)) {
          // Returning a borrow of a parameter is valid. Storing that borrow
          // inside the same owner would leave a self-reference after a move.
          // Keep the provenance from the reborrow point, since an alias may
          // have been reassigned before this boundary is checked.
          auto boundary = boundary_origins_.find(local);
          if (local != 0 && boundary != boundary_origins_.end() && boundary_stores_[local].count(id)) {
            std::vector<uint32_t> work{id};
            std::set<uint32_t> seen;
            while (!work.empty()) {
              uint32_t base = work.back(); work.pop_back();
              if (!seen.insert(base).second) continue;
              if (base == boundary->second) {
                self_reference = &loan;
                break;
              }
              auto parents = loan_bases_.find(base);
              if (parents != loan_bases_.end()) work.insert(work.end(), parents->second.begin(), parents->second.end());
            }
          }
          continue;  // external pointee
        }
        error(loan.span, "this returns a reference to " + place_text(f_, loan.place) +
              ", which does not live past the function", "SNX0502");
        (void)span;
        return;
      }
      if (self_reference) error(self_reference->span, "cannot store a reference to an owner inside that same owner; moving it would invalidate the reference", "SNX0506");
    };
    check_escape(0);
    // Owning a capture can still give access to borrowed external storage.
    // Once entries keep these boundaries until Return even after unpacking.

    for (uint32_t local = 1; local < nlocals_; ++local) {
      if ((f_.locals[local].is_arg || f_.locals[local].escape_boundary) && external_mutability(f_.locals[local].type)) check_escape(local);
    }
  }

  sema::Program& prog_;
  sema::TypeContext& ctx_;
  DiagnosticSink& sink_;
  Function& f_;
  size_t nlocals_;
  bool ok_ = true;
  std::vector<std::vector<uint32_t>> succ_;
  std::vector<uint32_t> rpo_;
  std::vector<bool> reachable_;
  std::vector<std::vector<uint8_t>> move_in_;
  std::vector<std::vector<bool>> live_in_, live_out_;
  std::vector<Loan> loan_list_;
  std::map<uint32_t, uint32_t> boundary_origins_;
  std::map<uint32_t, std::set<uint32_t>> loan_bases_;
  std::map<uint32_t, std::set<uint32_t>> boundary_stores_;
  bool provenance_changed_ = false;
  std::vector<Carriers> carriers_in_;
  std::map<const sema::Type*, bool> has_refs_cache_;
  const WriteEffects* write_effects_;
};

}  // namespace

BorrowChecker::BorrowChecker(sema::Program& program, sema::TypeContext& ctx, DiagnosticSink& sink)
    : prog_(program), ctx_(ctx), sink_(sink) {}

bool BorrowChecker::check_function(Function& f) {
  if (!f.lowered || f.blocks.empty()) return true;
  FunctionCheck check(prog_, ctx_, sink_, f, write_effects_.empty() ? nullptr : &write_effects_);
  return check.run();
}

bool BorrowChecker::check_module(Module& module) {
  write_effects_ = infer_write_effects(ctx_, module);
  bool ok = true;
  for (auto& f : module.functions) {
    if (!check_function(*f)) ok = false;
  }
  return ok;
}

}  // namespace synx::ir
