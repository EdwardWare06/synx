#include "synx/lower.hpp"

#include <algorithm>
#include <cassert>

namespace synx::ir {

using ast::ExprKind;
using sema::CoercionKind;
using sema::NameRef;
using sema::RefKind;
using sema::TypeKind;

Lowerer::Lowerer(sema::Program& program, sema::TypeContext& ctx, const sema::CheckResult& checked,
                 DiagnosticSink& sink)
    : prog_(program), ctx_(ctx), checked_(checked), sink_(sink) {}

// ---------- instances ----------

std::string Lowerer::instance_name(const sema::FunctionDef& def, const std::vector<const Type*>& owner_args,
                                   const std::vector<const Type*>& type_args) {
  std::string name;
  auto args_str = [](const std::vector<const Type*>& args) {
    if (args.empty()) return std::string();
    std::string s = "<";
    for (size_t i = 0; i < args.size(); ++i) {
      if (i) s += ", ";
      s += args[i]->to_string();
    }
    return s + ">";
  };
  switch (def.owner) {
    case sema::FnOwner::Struct: name = def.owner_struct->name + args_str(owner_args) + "." + def.name; break;
    case sema::FnOwner::Enum: name = def.owner_enum->name + args_str(owner_args) + "." + def.name; break;
    case sema::FnOwner::Impl: {
      std::string self = def.self_type ? def.self_type->to_string() : "?";
      if (!owner_args.empty()) {
        sema::Subst s;
        for (size_t i = 0; i < def.owner_impl->generics.size() && i < owner_args.size(); ++i) {
          s.bind(def.owner_impl->generics[i].type, owner_args[i]);
        }
        self = s.apply(ctx_, def.self_type)->to_string();
      }
      std::vector<const Type*> trait_args;
      sema::Subst subst;
      for (size_t i = 0; i < def.owner_impl->generics.size() && i < owner_args.size(); ++i) subst.bind(def.owner_impl->generics[i].type, owner_args[i]);
      for (auto* a : def.owner_impl->trait.args) trait_args.push_back(subst.apply(ctx_, a));
      name = self + "." + (def.owner_impl->trait.trait ? sema::qualified_name(def.owner_impl->trait.trait->module, def.owner_impl->trait.trait->name) + args_str(trait_args) + "." : "") + def.name;
      break;
    }
    case sema::FnOwner::Trait: {
      std::vector<const Type*> trait_args;
      if (!owner_args.empty()) trait_args.assign(owner_args.begin() + 1, owner_args.end());
      name = (owner_args.empty() ? "?" : owner_args[0]->to_string()) + "." + def.owner_trait->name + args_str(trait_args) + "." + def.name;
      break;
    }
    case sema::FnOwner::Module:
      // Keep module functions separate from an equally named type method
      // in another import scope (for example a.Unit.read).
      name = def.module && !def.module->is_prelude && !def.module->name.empty() ? "@fn." + def.name : def.name;
      break;
    default: name = def.name; break;
  }
  return sema::qualified_name(def.module, name + args_str(type_args));
}

Function* Lowerer::instance(const sema::FunctionDef& def, std::vector<const Type*> owner_args,
                            std::vector<const Type*> type_args) {
  std::string name = instance_name(def, owner_args, type_args);
  auto it = instances_.find(name);
  if (it != instances_.end()) return it->second;
  auto f = std::make_unique<Function>();
  f->name = name;
  f->def = &def;
  f->owner_args = std::move(owner_args);
  f->type_args = std::move(type_args);
  Function* out = f.get();
  module_->functions.push_back(std::move(f));
  instances_[name] = out;
  worklist_.push_back(out);
  return out;
}

bool Lowerer::lower_program(Module& module, bool require_main) {
  module_ = &module;
  const sema::Symbol* main = prog_.find("main");
  if (main && main->kind == sema::SymbolKind::Function && main->fn_def->module == prog_.root_module) {
    auto* entry = main->fn_def;
    if (!entry->params.empty() || !entry->generics.empty() || (!entry->ret->is_void() && !entry->ret->is_never() && !entry->ret->is_prim(ast::PrimKind::Int))) {
      sink_.error(entry->span, "main must have no parameters or type parameters and return int or void", "SNX0701");
      return false;
    }
    module.entry = instance(*main->fn_def, {}, {});
  } else if (require_main) {
    sink_.error(Span{0, 0, prog_.root_module ? prog_.root_module->file : 0}, "no main function", "SNX0701");
    return false;
  }
  // Every user function without generics is lowered so its errors are
  // reported even when nothing calls it.
  for (auto& def : prog_.functions) {
    if (def->is_prelude || def->is_builtin || !def->ast || !def->ast->body) continue;
    if (!def->generics.empty()) continue;
    bool owner_generic = false;
    switch (def->owner) {
      case sema::FnOwner::Struct: owner_generic = !def->owner_struct->generics.empty(); break;
      case sema::FnOwner::Enum: owner_generic = !def->owner_enum->generics.empty(); break;
      case sema::FnOwner::Impl: owner_generic = !def->owner_impl->generics.empty(); break;
      case sema::FnOwner::Trait: owner_generic = true; break;
      default: break;
    }
    if (owner_generic) continue;
    instance(*def, {}, {});
  }
  drain();
  return ok_;
}

void Lowerer::drain() {
  while (!worklist_.empty()) {
    Function* f = worklist_.back();
    worklist_.pop_back();
    // Expanding recursive generic owners must fail before generating an
    // unbounded family of destructor instances.
    std::vector<std::pair<const Type*, unsigned>> pending;
    for (auto* t : f->owner_args) pending.push_back({t, 0});
    for (auto* t : f->type_args) pending.push_back({t, 0});
    while (!pending.empty()) {
      auto [t, depth] = pending.back();
      pending.pop_back();
      if (depth == 64) {
        error(f->def->span, "generic instantiation exceeds 64 nested type arguments");
        return;
      }
      for (auto* child : t->elems) pending.push_back({child, depth + 1});
      if (t->inner) pending.push_back({t->inner, depth + 1});
      if (t->ret) pending.push_back({t->ret, depth + 1});
    }
    if (!f->lowered) lower_body(*f);
  }
}

// ---------- building blocks ----------

uint32_t Lowerer::new_block(FnCtx& c) {
  c.fn->blocks.emplace_back();
  return static_cast<uint32_t>(c.fn->blocks.size() - 1);
}

uint32_t Lowerer::new_temp(FnCtx& c, const Type* t, Span span) {
  Local l;
  l.type = t;
  l.span = span;
  c.fn->locals.push_back(l);
  return static_cast<uint32_t>(c.fn->locals.size() - 1);
}

bool Lowerer::block_terminated(FnCtx& c) const {
  return c.fn->blocks[c.current].term.kind != Terminator::Kind::Unreachable ||
         c.fn->blocks[c.current].term.span.end != 0;
}

void Lowerer::emit(FnCtx& c, Statement s) {
  if (block_terminated(c)) return;  // dead code after return or break
  c.fn->blocks[c.current].stmts.push_back(std::move(s));
}

void Lowerer::assign(FnCtx& c, Place p, Rvalue r, Span span) {
  Statement s;
  s.kind = Statement::Kind::Assign;
  s.place = std::move(p);
  s.rvalue = std::move(r);
  s.span = span;
  emit(c, std::move(s));
}

void Lowerer::set_term(FnCtx& c, Terminator t) {
  if (block_terminated(c)) return;
  if (t.span.end == 0) t.span.end = 1;  // mark as set even without a span
  c.fn->blocks[c.current].term = std::move(t);
}

void Lowerer::goto_block(FnCtx& c, uint32_t target) {
  Terminator t;
  t.kind = Terminator::Kind::Goto;
  t.target = target;
  set_term(c, std::move(t));
  c.current = target;
}

void Lowerer::push_scope(FnCtx& c) { c.scopes.emplace_back(); }

void Lowerer::scope_temporary(FnCtx& c, uint32_t local) {
  if (!c.fn->locals[local].name.empty()) return;
  for (const auto& scope : c.scopes) {
    if (std::find(scope.begin(), scope.end(), local) != scope.end()) return;
  }
  c.scopes.back().push_back(local);
}

void Lowerer::emit_drops_to(FnCtx& c, size_t scope_depth, Span span) {
  for (size_t i = c.scopes.size(); i > scope_depth; --i) {
    auto& locals = c.scopes[i - 1];
    for (auto it = locals.rbegin(); it != locals.rend(); ++it) {
      if (needs_drop(c.fn->locals[*it].type)) {
        Statement s;
        s.kind = Statement::Kind::Drop;
        s.place = Place::of(*it);
        s.span = span;
        emit(c, std::move(s));
      }
      // Parameter carriers are also checked for escapes at Return.
      if (!c.fn->locals[*it].is_arg && !c.fn->locals[*it].escape_boundary) {
        Statement dead;
        dead.kind = Statement::Kind::StorageDead;
        dead.place = Place::of(*it);
        dead.span = span;
        emit(c, std::move(dead));
      }
    }
  }
}

void Lowerer::pop_scope(FnCtx& c, Span span) {
  emit_drops_to(c, c.scopes.size() - 1, span);
  c.scopes.pop_back();
}

void Lowerer::call(FnCtx& c, Callee callee, std::vector<Operand> args, Place dest, bool diverges, Span span) {
  Terminator t;
  t.kind = Terminator::Kind::Call;
  t.callee = std::move(callee);
  t.args = std::move(args);
  t.dest = std::move(dest);
  t.diverges = diverges;
  t.span = span;
  if (!diverges) {
    uint32_t next = new_block(c);
    t.target = next;
    set_term(c, std::move(t));
    c.current = next;
  } else {
    set_term(c, std::move(t));
  }
}

// ---------- types ----------

const Type* Lowerer::subst_type(FnCtx& c, const Type* t) { return c.subst.apply(ctx_, t); }

const Type* Lowerer::type_of(FnCtx& c, const ast::Expr& e) {
  const Type* t = checked_.type_of(e);
  if (!t) return ctx_.error();
  return subst_type(c, t);
}

bool Lowerer::is_copy_type(const Type* t) { return sema::is_copy_type(prog_, ctx_, t); }

bool Lowerer::needs_drop(const Type* root) {
  auto cached = drop_cache_.find(root);
  if (cached != drop_cache_.end()) return cached->second;
  // Visit every owned child, even after finding a destructor. A cycle is
  // legal through Vec; propagation below computes its cleanup requirements.
  std::map<const Type*, std::vector<const Type*>> edges;
  std::set<const Type*> droppable;
  std::vector<const Type*> pending{root};
  while (!pending.empty()) {
    const Type* t = pending.back();
    pending.pop_back();
    if (edges.count(t)) continue;
    if (edges.size() == 512) {
      error(Span{}, "cleanup type expansion exceeds 512 types");
      return false;
    }
    auto& children = edges[t];
    if (t->kind == TypeKind::Fn) droppable.insert(t);
    if (t->kind == TypeKind::Struct || t->kind == TypeKind::Enum) {
      bool structure = t->kind == TypeKind::Struct;
      bool builtin = structure && (t->struct_def == prog_.string_def || t->struct_def == prog_.vec_def);
      if (builtin) droppable.insert(t);
      auto* drop = prog_.core_trait("Drop");
      for (auto& impl : prog_.impls) {
        bool matches = structure ? impl->self_type->kind == TypeKind::Struct && impl->self_type->struct_def == t->struct_def
                                 : impl->self_type->kind == TypeKind::Enum && impl->self_type->enum_def == t->enum_def;
        if (!builtin && impl->trait.trait == drop && matches) {
          droppable.insert(t);
          if (module_ && !module_->drop_impls.count(t)) {
            auto* method = drop->find_method("drop");
            auto* inst = method ? resolve_trait_method(*method, t, {}, Span{}) : nullptr;
            if (inst) module_->drop_impls[t] = inst;
          }
          break;
        }
      }
      sema::Subst subst;
      const auto& generics = structure ? t->struct_def->generics : t->enum_def->generics;
      for (size_t i = 0; i < generics.size(); ++i) subst.bind(generics[i].type, t->elems[i]);
      if (structure) {
        for (auto& field : t->struct_def->fields) children.push_back(subst.apply(ctx_, field.type));
        if (t->struct_def == prog_.vec_def) children.push_back(t->elems[0]);
      } else {
        for (auto& variant : t->enum_def->variants) {
          for (auto* field : variant.tuple_types) children.push_back(subst.apply(ctx_, field));
          for (auto& field : variant.fields) children.push_back(subst.apply(ctx_, field.type));
        }
      }
    } else if (t->kind == TypeKind::Tuple) children = t->elems;
    else if (t->kind == TypeKind::Array) children.push_back(t->inner);
    pending.insert(pending.end(), children.begin(), children.end());
  }
  bool changed;
  do {
    changed = false;
    for (auto& [t, children] : edges) {
      if (droppable.count(t)) continue;
      for (auto* child : children) if (droppable.count(child)) {
        droppable.insert(t);
        changed = true;
        break;
      }
    }
  } while (changed);
  if (module_) module_->drop_types.insert(droppable.begin(), droppable.end());
  for (auto& [type, children] : edges) { (void)children; drop_cache_[type] = droppable.count(type) != 0; }
  return droppable.count(root) != 0;
}

// ---------- function bodies ----------

void Lowerer::lower_body(Function& f) {
  f.lowered = true;
  const sema::FunctionDef& def = *f.def;
  FnCtx c;
  c.fn = &f;

  // Build the substitution for this instantiation.
  switch (def.owner) {
    case sema::FnOwner::Struct:
      for (size_t i = 0; i < def.owner_struct->generics.size() && i < f.owner_args.size(); ++i) c.subst.bind(def.owner_struct->generics[i].type, f.owner_args[i]);
      break;
    case sema::FnOwner::Enum:
      for (size_t i = 0; i < def.owner_enum->generics.size() && i < f.owner_args.size(); ++i) c.subst.bind(def.owner_enum->generics[i].type, f.owner_args[i]);
      break;
    case sema::FnOwner::Impl:
      for (size_t i = 0; i < def.owner_impl->generics.size() && i < f.owner_args.size(); ++i) c.subst.bind(def.owner_impl->generics[i].type, f.owner_args[i]);
      if (def.owner_impl->trait.trait) {
        c.subst.bind(def.owner_impl->trait.trait->self_type, c.subst.apply(ctx_, def.owner_impl->self_type));
      }
      break;
    case sema::FnOwner::Trait:
      if (!f.owner_args.empty()) c.subst.bind(def.owner_trait->self_type, f.owner_args[0]);
      for (size_t i = 0; i < def.owner_trait->generics.size() && i + 1 < f.owner_args.size(); ++i) c.subst.bind(def.owner_trait->generics[i].type, f.owner_args[i + 1]);
      break;
    default: break;
  }
  for (size_t i = 0; i < def.generics.size() && i < f.type_args.size(); ++i) c.subst.bind(def.generics[i].type, f.type_args[i]);

  f.ret = f.closure ? f.closure_type->ret : subst_type(c, def.ret ? def.ret : ctx_.void_type());
  bool async_factory = !f.closure && !f.async_body && def.ast && def.ast->is_async;
  if (async_factory) f.ret = ctx_.struct_type(def.ast->async_send ? prog_.send_task_def : prog_.task_def, {f.ret});
  Local ret;
  ret.type = f.ret;
  ret.name = "return";
  f.locals.push_back(ret);
  if (f.closure) {
    Local env;
    env.type = f.closure_type->call_mode == ast::CallMode::Once ? ctx_.ptr(f.environment_type, false)
               : ctx_.ref(f.environment_type, f.closure_type->call_mode == ast::CallMode::Shared);
    env.name = "environment";
    env.is_arg = true;
    f.locals.push_back(env);
    auto& ids = checked_.closure_params.at(f.closure);
    for (size_t i = 0; i < ids.size(); ++i) {
      Local l;
      l.type = f.closure_type->elems[i];
      l.name = f.closure->params[i].name;
      l.is_arg = true;
      l.span = f.closure->params[i].span;
      f.locals.push_back(l);
    }
    f.arg_count = static_cast<uint32_t>(ids.size() + 1);
  } else for (auto& p : def.params) {
    Local l;
    l.type = subst_type(c, p.type);
    l.name = p.name;
    l.is_arg = true;
    l.span = p.span;
    f.locals.push_back(l);
  }
  if (!f.closure) f.arg_count = static_cast<uint32_t>(def.params.size());
  if (async_factory) { lower_async_factory(c); return; }
  if (f.async_body) {
    f.async_waker = static_cast<uint32_t>(f.locals.size());
    f.locals.push_back({ctx_.struct_type(prog_.waker_def, {}), "@wake", true, def.span});
    ++f.arg_count;
  }

  if (def.is_builtin || !def.ast || !def.ast->body) {
    // Extern and builtin functions have no body; callers use the builtin
    // callee kind. A body is required for anything else.
    f.blocks.emplace_back();
    Terminator t;
    t.kind = Terminator::Kind::Unreachable;
    t.span = Span{0, 1};
    f.blocks[0].term = t;
    return;
  }

  // Map checker locals: parameters come first in the same order.
  size_t checker_count = checker_locals(c).size();
  c.local_map.assign(checker_count, UINT32_MAX);
  if (f.closure) {
    auto& ids = checked_.closure_params.at(f.closure);
    for (uint32_t i = 0; i < ids.size(); ++i) c.local_map[ids[i]] = i + 2;
    auto& captures = captures_of(f.closure);
    for (uint32_t i = 0; i < captures.size(); ++i) {
      Place field = Place::of(1).deref().field(i + 1);
      auto mode = capture_mode(f.closure, captures[i]);
      if (mode != ast::CaptureMode::Move) field = field.deref();
      c.captures[captures[i]] = field;
    }
  } else {
    for (uint32_t i = 0; i < def.params.size() && i < checker_count; ++i) c.local_map[i] = i + 1;
  }

  c.current = new_block(c);
  c.return_block = new_block(c);
  push_scope(c);
  // Parameters own their values, including closure values passed to apply.
  // The environment parameter is a borrowed reference and needs no drop.
  for (uint32_t i = 1; i <= f.arg_count; ++i) if (i != f.async_waker) c.scopes.back().push_back(i);
  if (f.closure && f.closure_type->call_mode == ast::CallMode::Once && !captures_of(f.closure).empty()) {
    std::vector<uint32_t> owned;
    const auto& fields = f.environment_type->elems;
    for (uint32_t i = 0; i < captures_of(f.closure).size(); ++i) {
      uint32_t local = new_temp(c, fields[i + 1], f.closure->span);
      f.locals[local].escape_boundary = true;
      Rvalue capture;
      capture.kind = RvalueKind::Use;
      capture.a = operand_from_place(c, Place::of(1).deref().field(i + 1), fields[i + 1]);
      assign(c, Place::of(local), capture, f.closure->span);
      uint32_t id = captures_of(f.closure)[i];
      auto mode = capture_mode(f.closure, id);
      c.captures[id] = mode != ast::CaptureMode::Move
                        ? Place::of(local).deref() : Place::of(local);
      owned.push_back(local);
    }
    c.scopes.back().insert(c.scopes.back().begin(), owned.begin(), owned.end());
    Callee free; free.kind = Callee::Kind::Builtin; free.builtin = "synx_rt_free";
    call(c, free, {Operand::copy(Place::of(1))}, Place::none(), false, f.closure->span);
  }
  Place ret_place = Place::of(0);
  Span body_span = f.closure ? f.closure->body->span : def.ast->body->span;
  if (f.closure) {
    if (f.ret->is_void()) {
      Operand ignored = lower_operand(c, *f.closure->body);
      (void)ignored;
    } else lower_into(c, *f.closure->body, ret_place);
  } else lower_block_into(c, *def.ast->body, f.ret->is_void() ? nullptr : &ret_place);
  if (!block_terminated(c)) {
    pop_scope(c, body_span);
    goto_block(c, c.return_block);
  } else {
    c.scopes.pop_back();
  }
  c.current = c.return_block;
  Terminator r;
  r.kind = Terminator::Kind::Return;
  r.span = body_span;
  set_term(c, r);
  if (f.async_body) {
    for (uint32_t i = 1; i < f.locals.size(); ++i)
      if (i != f.async_waker && needs_drop(f.locals[i].type)) f.async_drop_locals.insert(i);
  }
}

Place Lowerer::checker_place(FnCtx& c, uint32_t id) {
  auto capture = c.captures.find(id);
  if (capture != c.captures.end()) return capture->second;
  if (id >= c.local_map.size() || c.local_map[id] == UINT32_MAX) return Place::none();
  return Place::of(c.local_map[id]);
}

const std::vector<uint32_t>& Lowerer::captures_of(const ast::Expr* closure) const {
  static const std::vector<uint32_t> none;
  auto found = checked_.closure_captures.find(closure);
  return found == checked_.closure_captures.end() ? none : found->second;
}

ast::CaptureMode Lowerer::capture_mode(const ast::Expr* closure, uint32_t id) const {
  auto modes = checked_.closure_capture_modes.find(closure);
  if (modes == checked_.closure_capture_modes.end()) return ast::CaptureMode::Move;
  auto mode = modes->second.find(id);
  return mode == modes->second.end() ? ast::CaptureMode::Move : mode->second;
}

const std::vector<sema::LocalVar>& Lowerer::checker_locals(FnCtx& c) {
  if (c.constant) return checked_.const_locals.at(c.constant);
  if (c.fn->closure) return checked_.closure_locals.at(c.fn->closure);
  return checked_.fn_locals.at(c.fn->def);
}


void Lowerer::lower_async_factory(FnCtx& c) {
  Function& f = *c.fn;
  const Type* output = f.ret->elems[0];
  const Type* raw = ctx_.ptr(ctx_.prim(ast::PrimKind::Byte), false);
  const Type* wake = ctx_.struct_type(prog_.waker_def, {});
  const Type* poll = ctx_.enum_type(prog_.poll_def, {output});
  auto body = std::make_unique<Function>();
  body->name = f.name + ".@async.poll";
  body->def = f.def;
  body->owner_args = f.owner_args;
  body->type_args = f.type_args;
  body->async_body = true;
  f.async_poll = body.get();
  instances_[body->name] = body.get();
  worklist_.push_back(body.get());
  module_->functions.push_back(std::move(body));
  auto cancel = std::make_unique<Function>();
  cancel->name = f.name + ".@async.cancel";
  cancel->ret = ctx_.void_type();
  cancel->arg_count = 1;
  cancel->lowered = true;
  cancel->locals = {{ctx_.void_type(), "return", false, f.def->span}, {raw, "frame", true, f.def->span}};
  cancel->blocks.emplace_back();
  cancel->blocks[0].term.kind = Terminator::Kind::Return;
  f.async_cancel = cancel.get();
  instances_[cancel->name] = cancel.get();
  module_->functions.push_back(std::move(cancel));
  c.current = new_block(c);
  auto address = [&](Function* fn, const Type* type) {
    Constant k; k.kind = Constant::Kind::Fn; k.fn = fn; k.type = type;
    return Operand::constant_of(k);
  };
  Constant null; null.kind = Constant::Kind::Int; null.type = raw;
  Constant no; no.kind = Constant::Kind::Bool; no.type = ctx_.bool_type();
  Rvalue result; result.kind = RvalueKind::Aggregate; result.type = f.ret;
  result.elems = {Operand::constant_of(null), address(f.async_poll, ctx_.fn({raw, wake}, poll, ast::CallMode::Writable, f.def->ast->async_send)),
                  address(f.async_cancel, ctx_.fn({raw}, ctx_.void_type(), ast::CallMode::Writable, f.def->ast->async_send)), Operand::constant_of(no)};
  for (uint32_t i = 1; i <= f.arg_count; ++i)
    result.captures.push_back(operand_from_place(c, Place::of(i), f.locals[i].type));
  assign(c, Place::of(0), result, f.def->span);
  Terminator ret; ret.kind = Terminator::Kind::Return; ret.span = f.def->span;
  set_term(c, ret);
  needs_drop(f.ret);
  needs_drop(wake);
}

Operand Lowerer::lower_await(FnCtx& c, const ast::AwaitExpr& e) {
  const Type* output = type_of(c, e);
  const Type* future = type_of(c, *e.operand);
  const Type* base = future;
  Place child;
  Operand value = lower_operand(c, *e.operand);
  if (block_terminated(c)) return value;
  child = Place::of(new_temp(c, future, e.span));
  c.scopes.back().push_back(child.local);
  // A loop may reuse this await slot. Destroy its previous completed value
  // after evaluating the replacement, before initializing the next one.
  if (needs_drop(future)) {
    Statement drop; drop.kind = Statement::Kind::Drop; drop.place = child; drop.span = e.span; emit(c, drop);
  }
  Rvalue use; use.a = value; assign(c, child, use, e.span);
  Place receiver = child;
  while (base->kind == TypeKind::Ref) { base = base->inner; receiver = receiver.deref(); }
  const Type* poll_type = ctx_.enum_type(prog_.poll_def, {output});
  uint32_t polled = new_temp(c, poll_type, e.span);
  c.scopes.back().push_back(polled);
  uint32_t loop = new_block(c);
  goto_block(c, loop);
  if (needs_drop(poll_type)) {
    Statement drop; drop.kind = Statement::Kind::Drop; drop.place = Place::of(polled); drop.span = e.span; emit(c, drop);
  }
  const Type* wake_type = ctx_.struct_type(prog_.waker_def, {});
  uint32_t wake_ref = new_temp(c, ctx_.ref(wake_type, true), e.span);
  Rvalue borrow; borrow.kind = RvalueKind::Ref; borrow.place = Place::of(c.fn->async_waker); borrow.is_const = true;
  borrow.type = c.fn->locals[wake_ref].type; assign(c, Place::of(wake_ref), borrow, e.span);
  uint32_t wake = new_temp(c, wake_type, e.span);
  c.scopes.back().push_back(wake);
  Callee clone; clone.fn = instance(*prog_.waker_def->find_method("clone"), {}, {});
  call(c, clone, {Operand::copy(Place::of(wake_ref))}, Place::of(wake), false, e.span);
  uint32_t ref = new_temp(c, ctx_.ref(base, false), e.span);
  borrow.place = receiver; borrow.is_const = false; borrow.type = c.fn->locals[ref].type;
  assign(c, Place::of(ref), borrow, e.span);
  Callee poll;
  if (base->kind == TypeKind::Dyn) {
    poll.kind = Callee::Kind::Virtual;
    poll.vtable_slot = 0;
  } else {
    std::vector<const Type*> args{output};
    poll.fn = resolve_trait_method(*prog_.core_trait("Future")->find_method("poll"), base, {}, e.span, &args);
    if (!poll.fn) return Operand();
    sema::Subst subst;
    const auto& def = *poll.fn->def;
    if (def.owner == sema::FnOwner::Impl)
      for (size_t i = 0; i < def.owner_impl->generics.size(); ++i) subst.bind(def.owner_impl->generics[i].type, poll.fn->owner_args[i]);
    else if (def.owner == sema::FnOwner::Trait) {
      subst.bind(def.owner_trait->self_type, base);
      for (size_t i = 0; i < def.owner_trait->generics.size(); ++i) subst.bind(def.owner_trait->generics[i].type, args[i]);
    }
    bool valid = def.params.size() == 2 && def.generics.empty() && !def.ast->is_async &&
                 subst.apply(ctx_, def.ret) == poll_type && subst.apply(ctx_, def.params[0].type) == ctx_.ref(base, false) &&
                 subst.apply(ctx_, def.params[1].type) == wake_type;
    if (!valid) { error(e.span, "implementation of Future.poll does not match its trait signature"); return Operand(); }
  }
  call(c, poll, {Operand::move(Place::of(ref)), Operand::move(Place::of(wake))}, Place::of(polled), false, e.span);
  uint32_t discr = new_temp(c, ctx_.usize_type(), e.span);
  Rvalue tag; tag.kind = RvalueKind::Discriminant; tag.place = Place::of(polled);
  assign(c, Place::of(discr), tag, e.span);
  uint32_t ready = new_block(c), pending = new_block(c);
  Terminator sw; sw.kind = Terminator::Kind::Switch; sw.discr = Operand::copy(Place::of(discr));
  sw.cases = {{1, ready}}; sw.otherwise = pending; sw.span = e.span; set_term(c, sw);
  c.current = pending;
  Terminator suspend; suspend.kind = Terminator::Kind::Call; suspend.callee.kind = Callee::Kind::Builtin;
  suspend.callee.builtin = "@async_suspend"; suspend.dest = Place::none(); suspend.target = loop; suspend.span = e.span;
  c.fn->async_suspends[pending] = loop;
  set_term(c, suspend);
  c.current = ready;
  uint32_t result = new_temp(c, output, e.span);
  c.scopes.back().push_back(result);
  use.a = operand_from_place(c, Place::of(polled).downcast(1).field(0), output);
  assign(c, Place::of(result), use, e.span);
  if (base->kind == TypeKind::Struct && (base->struct_def == prog_.task_def || base->struct_def == prog_.send_task_def) && future == base) {
    Statement drop; drop.kind = Statement::Kind::Drop; drop.place = child; drop.span = e.span; emit(c, drop);
  }
  return operand_from_place(c, Place::of(result), output);
}

Operand Lowerer::lower_closure(FnCtx& c, const ast::ClosureExpr& e) {
  const Type* fn_type = type_of(c, e);
  auto& captures = captures_of(&e);
  auto& locals = checker_locals(c);
  const Type* raw_pointer = ctx_.ptr(ctx_.prim(ast::PrimKind::Byte), false);
  std::vector<const Type*> fields{raw_pointer};  // environment destructor
  for (uint32_t id : captures) {
    const Type* type = subst_type(c, locals[id].type);
    auto mode = capture_mode(&e, id);
    if (mode != ast::CaptureMode::Move)
      type = ctx_.ref(type, mode == ast::CaptureMode::Shared);
    fields.push_back(type);
    needs_drop(type);  // instantiate capture destructors
  }
  const Type* env_type = ctx_.tuple(fields);
  const Type* env_pointer = ctx_.ptr(env_type, false);
  auto [closure_id, inserted] = closure_ids_.emplace(&e, static_cast<uint32_t>(closure_ids_.size()));
  (void)inserted;
  std::string name = c.fn->name + ".closure" + std::to_string(closure_id->second);
  Function* entry = nullptr;
  Function* drop = nullptr;
  auto existing = instances_.find(name);
  if (existing != instances_.end()) {
    entry = existing->second;
    if (!captures.empty()) drop = instances_.at(name + ".drop");
  } else {
    auto f = std::make_unique<Function>();
    f->name = name;
    f->def = c.fn->def;
    f->owner_args = c.fn->owner_args;
    f->type_args = c.fn->type_args;
    f->closure = &e;
    f->closure_type = fn_type;
    f->environment_type = env_type;
    entry = f.get();
    instances_[name] = entry;
    module_->functions.push_back(std::move(f));
    worklist_.push_back(entry);

    if (!captures.empty()) {
      auto d = std::make_unique<Function>();
      d->name = name + ".drop";
      d->ret = ctx_.void_type();
      d->arg_count = 1;
      d->lowered = true;
      d->locals.push_back({ctx_.void_type(), "return", false, e.span});
      d->locals.push_back({env_pointer, "environment", true, e.span});
      drop = d.get();
      instances_[d->name] = drop;
      module_->functions.push_back(std::move(d));
      FnCtx dc;
      dc.fn = drop;
      dc.current = new_block(dc);
      for (size_t i = fields.size(); i > 1; --i) {
        if (!needs_drop(fields[i - 1])) continue;
        Statement s;
        s.kind = Statement::Kind::Drop;
        s.place = Place::of(1).deref().field(static_cast<uint32_t>(i - 1));
        s.span = e.span;
        emit(dc, s);
      }
      Callee free;
      free.kind = Callee::Kind::Builtin;
      free.builtin = "synx_rt_free";
      call(dc, free, {Operand::copy(Place::of(1))}, Place::none(), false, e.span);
      Terminator ret;
      ret.kind = Terminator::Kind::Return;
      ret.span = e.span;
      set_term(dc, ret);
    }
  }

  auto function_address = [&](Function* fn) {
    Constant k;
    k.kind = Constant::Kind::Fn;
    k.fn = fn;
    k.type = raw_pointer;
    return Operand::constant_of(k);
  };
  Constant null;
  null.kind = Constant::Kind::Int;
  null.type = raw_pointer;
  Operand env = Operand::constant_of(null);
  std::vector<Operand> captured;
  if (!captures.empty()) {
    uint32_t size = new_temp(c, ctx_.usize_type(), e.span);
    Callee sizeof_env;
    sizeof_env.kind = Callee::Kind::Builtin;
    sizeof_env.builtin = "size_of";
    sizeof_env.type_args = {env_type};
    call(c, sizeof_env, {}, Place::of(size), false, e.span);
    uint32_t pointer = new_temp(c, env_pointer, e.span);
    Callee alloc;
    alloc.kind = Callee::Kind::Builtin;
    alloc.builtin = "synx_rt_alloc";
    call(c, alloc, {Operand::copy(Place::of(size))}, Place::of(pointer), false, e.span);
    Rvalue destructor;
    destructor.a = function_address(drop);
    assign(c, Place::of(pointer).deref().field(0), destructor, e.span);
    for (uint32_t i = 0; i < captures.size(); ++i) {
      Place source = checker_place(c, captures[i]);
      if (source.is_none()) { error(e.span, "closure capture has no local"); return Operand(); }
      Rvalue capture;
      auto mode = capture_mode(&e, captures[i]);
      if (mode != ast::CaptureMode::Move) {
        uint32_t borrowed = new_temp(c, fields[i + 1], e.span);
        Rvalue borrow; borrow.kind = RvalueKind::Ref; borrow.place = source;
        borrow.type = fields[i + 1]; borrow.is_const = borrow.type->is_const;
        assign(c, Place::of(borrowed), borrow, e.span);
        capture.a = operand_from_place(c, Place::of(borrowed), fields[i + 1]);
      } else capture.a = operand_from_place(c, source, fields[i + 1]);
      captured.push_back(capture.a);
      // Ownership transfers at the completed closure aggregate, where loan
      // tracking can retain the captures despite the erased heap pointer.
      if (capture.a.kind == Operand::Kind::Move) capture.a.kind = Operand::Kind::Copy;
      assign(c, Place::of(pointer).deref().field(i + 1), capture, e.span);
    }
    env = Operand::copy(Place::of(pointer));
  }
  Rvalue value;
  value.kind = RvalueKind::Aggregate;
  value.agg = AggregateKind::Tuple;
  value.type = fn_type;
  value.elems = {function_address(entry), env};
  value.captures = std::move(captured);
  uint32_t result = new_temp(c, fn_type, e.span);
  assign(c, Place::of(result), value, e.span);
  return Operand::move(Place::of(result));
}

// ---------- statements ----------

void Lowerer::lower_block_into(FnCtx& c, const ast::Block& b, Place* dest) {
  push_scope(c);
  for (auto& s : b.stmts) {
    lower_stmt(c, *s);
    if (block_terminated(c)) break;
  }
  if (!block_terminated(c)) {
    if (b.tail) {
      const Type* tt = type_of(c, *b.tail);
      if (dest && !tt->is_void() && !tt->is_never()) {
        lower_into(c, *b.tail, *dest);
      } else {
        Operand o = lower_operand(c, *b.tail);
        (void)o;
      }
    }
  }
  if (!block_terminated(c)) pop_scope(c, b.span); else c.scopes.pop_back();
}

void Lowerer::lower_stmt(FnCtx& c, const ast::Stmt& s) {
  switch (s.kind) {
    case ast::StmtKind::VarDecl: {
      auto& d = static_cast<const ast::VarDeclStmt&>(s);
      uint32_t checker_id = checked_.decl_locals.at(&d);
      const Type* t = subst_type(c, checker_locals(c)[checker_id].type);
      uint32_t local = new_temp(c, t, d.name_span);
      c.fn->locals[local].name = d.name;
      if (checker_id >= c.local_map.size()) c.local_map.resize(checker_id + 1, UINT32_MAX);
      c.local_map[checker_id] = local;
      if (d.init) lower_into(c, *d.init, Place::of(local));
      c.scopes.back().push_back(local);
      break;
    }
    case ast::StmtKind::Expr: {
      auto& e = static_cast<const ast::ExprStmt&>(s);
      const Type* t = type_of(c, *e.expr);
      if (e.expr->kind == ExprKind::If || e.expr->kind == ExprKind::Match || e.expr->kind == ExprKind::Block) {
        // Statement position: no destination needed.
        if (e.expr->kind == ExprKind::If) lower_if(c, static_cast<const ast::IfExpr&>(*e.expr), nullptr);
        else if (e.expr->kind == ExprKind::Match) lower_match(c, static_cast<const ast::MatchExpr&>(*e.expr), nullptr);
        else lower_block_into(c, *static_cast<const ast::BlockExpr&>(*e.expr).block, nullptr);
        break;
      }
      Operand o = lower_operand(c, *e.expr);
      // A temporary that needs dropping and is not used must still be dropped.
      if (o.kind == Operand::Kind::Move && o.place.is_local() && needs_drop(t) && c.fn->locals[o.place.local].name.empty()) {
        Statement d;
        d.kind = Statement::Kind::Drop;
        d.place = o.place;
        d.span = s.span;
        emit(c, d);
      }
      break;
    }
    case ast::StmtKind::Assign: {
      auto& a = static_cast<const ast::AssignStmt&>(s);
      Place target;
      if (!lower_place(c, *a.target, target)) {
        error(a.target->span, "cannot assign to this expression");
        break;
      }
      const Type* tt = type_of(c, *a.target);
      if (a.op == ast::AssignOp::Assign) {
        // Evaluate the replacement before destroying the old value. The RHS
        // may move that value, as in list = Next(Box.new(list)). Raw stores
        // may initialize memory and leave cleanup to the unsafe caller.
        if (needs_drop(tt) && !assigns_through_raw_pointer(c, *a.target)) {
          uint32_t replacement = new_temp(c, tt, a.value->span);
          c.scopes.back().push_back(replacement);
          lower_into(c, *a.value, Place::of(replacement));
          Statement d;
          d.kind = Statement::Kind::Drop;
          d.place = target;
          d.span = s.span;
          emit(c, d);
          Rvalue value;
          value.kind = RvalueKind::Use;
          value.a = operand_from_place(c, Place::of(replacement), tt);
          assign(c, target, value, s.span);
        } else lower_into(c, *a.value, target);
      } else {
        Operand rhs = lower_operand(c, *a.value);
        Rvalue r;
        r.kind = RvalueKind::Binary;
        switch (a.op) {
          case ast::AssignOp::Add: r.bop = ast::BinaryOp::Add; break;
          case ast::AssignOp::Sub: r.bop = ast::BinaryOp::Sub; break;
          case ast::AssignOp::Mul: r.bop = ast::BinaryOp::Mul; break;
          case ast::AssignOp::Div: r.bop = ast::BinaryOp::Div; break;
          case ast::AssignOp::Rem: r.bop = ast::BinaryOp::Rem; break;
          case ast::AssignOp::BitAnd: r.bop = ast::BinaryOp::BitAnd; break;
          case ast::AssignOp::BitOr: r.bop = ast::BinaryOp::BitOr; break;
          case ast::AssignOp::BitXor: r.bop = ast::BinaryOp::BitXor; break;
          case ast::AssignOp::Shl: r.bop = ast::BinaryOp::Shl; break;
          case ast::AssignOp::Shr: r.bop = ast::BinaryOp::Shr; break;
          default: break;
        }
        r.a = Operand::copy(target);
        r.b = rhs;
        r.type = tt;
        assign(c, target, r, s.span);
      }
      break;
    }
    case ast::StmtKind::While: {
      auto& w = static_cast<const ast::WhileStmt&>(s);
      uint32_t head = new_block(c);
      uint32_t body = new_block(c);
      uint32_t exit = new_block(c);
      uint32_t condition_exit = new_block(c);
      goto_block(c, head);
      // Each condition evaluation owns its temporaries for this iteration.
      // Both branch exits and loop control must clean them before reevaluation.
      size_t condition_depth = c.scopes.size();
      push_scope(c);
      Operand cond = lower_operand(c, *w.cond);
      Terminator t;
      t.kind = Terminator::Kind::Switch;
      t.discr = cond;
      t.cases = {{1, body}};
      t.otherwise = condition_exit;
      t.span = w.cond->span;
      set_term(c, t);
      c.current = body;
      c.loops.push_back({exit, head, condition_depth});
      lower_block_into(c, *w.body, nullptr);
      c.loops.pop_back();
      emit_drops_to(c, condition_depth, w.cond->span);
      if (!block_terminated(c)) goto_block(c, head);
      c.current = condition_exit;
      pop_scope(c, w.cond->span);
      goto_block(c, exit);
      c.current = exit;
      break;
    }
    case ast::StmtKind::Loop: {
      auto& l = static_cast<const ast::LoopStmt&>(s);
      uint32_t body = new_block(c);
      uint32_t exit = new_block(c);
      goto_block(c, body);
      c.loops.push_back({exit, body, c.scopes.size()});
      lower_block_into(c, *l.body, nullptr);
      c.loops.pop_back();
      if (!block_terminated(c)) goto_block(c, body);
      c.current = exit;
      break;
    }
    case ast::StmtKind::For:
      lower_for(c, static_cast<const ast::ForStmt&>(s));
      break;
  }
}

void Lowerer::lower_for(FnCtx& c, const ast::ForStmt& s) {
  const Type* it = type_of(c, *s.iter);
  int refs = 0;
  const Type* base = it;
  while (base->kind == TypeKind::Ref) { base = base->inner; ++refs; }
  bool is_const_ref = refs > 0 && it->is_const;

  push_scope(c);
  if (base->kind == TypeKind::Struct && (base->struct_def == prog_.range_def || base->struct_def == prog_.range_inclusive_def)) {
    bool inclusive = base->struct_def == prog_.range_inclusive_def;
    const Type* elem = base->elems[0];
    Operand range = lower_operand(c, *s.iter);
    uint32_t range_local = new_temp(c, base, s.iter->span);
    Rvalue use;
    use.kind = RvalueKind::Use;
    use.a = range;
    if (refs > 0) {
      // Copy the bounds out of the referenced range.
      Place rp = range.place;
      for (int i = 0; i < refs; ++i) rp = rp.deref();
      use.a = Operand::copy(rp);
    }
    assign(c, Place::of(range_local), use, s.iter->span);
    uint32_t i = new_temp(c, elem, s.pattern->span);
    uint32_t end = new_temp(c, elem, s.iter->span);
    Rvalue start_v; start_v.kind = RvalueKind::Use; start_v.a = Operand::copy(Place::of(range_local).field(0));
    assign(c, Place::of(i), start_v, s.span);
    Rvalue end_v; end_v.kind = RvalueKind::Use; end_v.a = Operand::copy(Place::of(range_local).field(1));
    assign(c, Place::of(end), end_v, s.span);

    uint32_t head = new_block(c);
    uint32_t body = new_block(c);
    uint32_t step = new_block(c);
    uint32_t exit = new_block(c);
    goto_block(c, head);
    uint32_t cond = new_temp(c, ctx_.bool_type(), s.span);
    Rvalue cmp;
    cmp.kind = RvalueKind::Binary;
    cmp.bop = inclusive ? ast::BinaryOp::Le : ast::BinaryOp::Lt;
    cmp.a = Operand::copy(Place::of(i));
    cmp.b = Operand::copy(Place::of(end));
    cmp.type = ctx_.bool_type();
    assign(c, Place::of(cond), cmp, s.span);
    Terminator t;
    t.kind = Terminator::Kind::Switch;
    t.discr = Operand::copy(Place::of(cond));
    t.cases = {{1, body}};
    t.otherwise = exit;
    t.span = s.span;
    set_term(c, t);

    c.current = body;
    push_scope(c);
    lower_pattern_bind(c, *s.pattern, Place::of(i), elem, 0);
    c.loops.push_back({exit, step, c.scopes.size() - 1});
    lower_block_into(c, *s.body, nullptr);
    c.loops.pop_back();
    if (!block_terminated(c)) { pop_scope(c, s.span); goto_block(c, step); } else c.scopes.pop_back();
    c.current = step;
    if (inclusive) {
      // The final value has already run. Do not increment past the type's
      // maximum, even when the body reached this block through continue.
      uint32_t advance = new_block(c);
      uint32_t done = new_temp(c, ctx_.bool_type(), s.span);
      Rvalue last; last.kind = RvalueKind::Binary; last.bop = ast::BinaryOp::Eq;
      last.a = Operand::copy(Place::of(i)); last.b = Operand::copy(Place::of(end)); last.type = ctx_.bool_type();
      assign(c, Place::of(done), last, s.span);
      Terminator stop; stop.kind = Terminator::Kind::Switch; stop.discr = Operand::copy(Place::of(done));
      stop.cases = {{1, exit}}; stop.otherwise = advance; stop.span = s.span;
      set_term(c, stop); c.current = advance;
    }
    Rvalue inc;
    inc.kind = RvalueKind::Binary;
    inc.bop = ast::BinaryOp::Add;
    inc.a = Operand::copy(Place::of(i));
    Constant one;
    one.kind = Constant::Kind::Int;
    one.int_value = 1;
    one.type = elem;
    inc.b = Operand::constant_of(one);
    inc.type = elem;
    assign(c, Place::of(i), inc, s.span);
    goto_block(c, head);
    c.current = exit;
    pop_scope(c, s.span);
    return;
  }

  // Arrays, slices, and Vec: index loop.
  const Type* elem_t = nullptr;
  Place container;
  const Type* container_t = base;
  if (base->kind == TypeKind::Array || base->kind == TypeKind::Slice) elem_t = base->inner;
  else if (base->kind == TypeKind::Struct && base->struct_def == prog_.vec_def) elem_t = base->elems[0];
  if (!elem_t) {
    error(s.iter->span, "cannot iterate over this value");
    c.scopes.pop_back();
    return;
  }
  if (refs > 0) {
    Operand r = lower_operand(c, *s.iter);
    uint32_t rl = new_temp(c, it, s.iter->span);
    Rvalue use; use.kind = RvalueKind::Use; use.a = r;
    assign(c, Place::of(rl), use, s.iter->span);
    container = Place::of(rl);
    for (int k = 0; k < refs; ++k) container = container.deref();
  } else {
    if (!is_copy_type(elem_t) && base->kind == TypeKind::Array) {
      error(s.iter->span, "iterating an array by value moves its elements; iterate over &const array instead");
    }
    if (base->kind == TypeKind::Struct) {
      error(s.iter->span, "iterating a Vec by value is not supported yet; iterate over &const vec or &vec");
    }
    if (!lower_place(c, *s.iter, container)) {
      Operand r = lower_operand(c, *s.iter);
      uint32_t rl = new_temp(c, it, s.iter->span);
      Rvalue use; use.kind = RvalueKind::Use; use.a = r;
      assign(c, Place::of(rl), use, s.iter->span);
      container = Place::of(rl);
      c.scopes.back().push_back(rl);
    }
  }
  uint32_t len = new_temp(c, ctx_.usize_type(), s.iter->span);
  Rvalue lenv;
  lenv.kind = RvalueKind::Len;
  lenv.place = container;
  lenv.type = ctx_.usize_type();
  assign(c, Place::of(len), lenv, s.iter->span);
  uint32_t i = new_temp(c, ctx_.usize_type(), s.span);
  Constant zero; zero.kind = Constant::Kind::Int; zero.int_value = 0; zero.type = ctx_.usize_type();
  Rvalue z; z.kind = RvalueKind::Use; z.a = Operand::constant_of(zero);
  assign(c, Place::of(i), z, s.span);

  uint32_t head = new_block(c);
  uint32_t body = new_block(c);
  uint32_t step = new_block(c);
  uint32_t exit = new_block(c);
  goto_block(c, head);
  uint32_t cond = new_temp(c, ctx_.bool_type(), s.span);
  Rvalue cmp;
  cmp.kind = RvalueKind::Binary;
  cmp.bop = ast::BinaryOp::Lt;
  cmp.a = Operand::copy(Place::of(i));
  cmp.b = Operand::copy(Place::of(len));
  cmp.type = ctx_.bool_type();
  assign(c, Place::of(cond), cmp, s.span);
  Terminator t;
  t.kind = Terminator::Kind::Switch;
  t.discr = Operand::copy(Place::of(cond));
  t.cases = {{1, body}};
  t.otherwise = exit;
  t.span = s.span;
  set_term(c, t);

  c.current = body;
  push_scope(c);
  Place elem_place;
  if (container_t->kind == TypeKind::Struct) {
    // Vec: data pointer, then index.
    elem_place = container.field(0).index(i);
  } else {
    elem_place = container.index(i);
  }
  if (refs > 0) {
    uint32_t r = new_temp(c, ctx_.ref(elem_t, is_const_ref), s.pattern->span);
    Rvalue rv;
    rv.kind = RvalueKind::Ref;
    rv.place = elem_place;
    rv.is_const = is_const_ref;
    rv.type = c.fn->locals[r].type;
    assign(c, Place::of(r), rv, s.pattern->span);
    lower_pattern_bind(c, *s.pattern, Place::of(r), c.fn->locals[r].type, 0);
  } else {
    lower_pattern_bind(c, *s.pattern, elem_place, elem_t, 0);
  }
  c.loops.push_back({exit, step, c.scopes.size() - 1});
  lower_block_into(c, *s.body, nullptr);
  c.loops.pop_back();
  if (!block_terminated(c)) { pop_scope(c, s.span); goto_block(c, step); } else c.scopes.pop_back();
  c.current = step;
  Rvalue inc;
  inc.kind = RvalueKind::Binary;
  inc.bop = ast::BinaryOp::Add;
  inc.a = Operand::copy(Place::of(i));
  Constant one; one.kind = Constant::Kind::Int; one.int_value = 1; one.type = ctx_.usize_type();
  inc.b = Operand::constant_of(one);
  inc.type = ctx_.usize_type();
  assign(c, Place::of(i), inc, s.span);
  goto_block(c, head);
  c.current = exit;
  pop_scope(c, s.span);
}

bool Lowerer::assigns_through_raw_pointer(FnCtx& c, const ast::Expr& target) {
  const ast::Expr* e = &target;
  for (;;) {
    switch (e->kind) {
      case ExprKind::Field: e = static_cast<const ast::FieldExpr*>(e)->base.get(); break;
      case ExprKind::Index: e = static_cast<const ast::IndexExpr*>(e)->base.get(); break;
      case ExprKind::Unary: {
        auto* u = static_cast<const ast::UnaryExpr*>(e);
        if (u->op != ast::UnaryOp::Deref) return false;
        if (type_of(c, *u->operand)->kind == TypeKind::Ptr) return true;
        e = u->operand.get();
        break;
      }
      default: return false;
    }
  }
}

void Lowerer::lower_return(FnCtx& c, const ast::Expr* value, Span span) {
  if (value) {
    const Type* vt = type_of(c, *value);
    if (!vt->is_void() && !vt->is_never()) lower_into(c, *value, Place::of(0));
    else { Operand o = lower_operand(c, *value); (void)o; }
  }
  emit_drops_to(c, 0, span);
  Terminator t;
  t.kind = Terminator::Kind::Goto;
  t.target = c.return_block;
  t.span = span;
  set_term(c, t);
}

// ---------- places ----------

bool Lowerer::lower_place(FnCtx& c, const ast::Expr& e, Place& out) {
  switch (e.kind) {
    case ExprKind::Name: {
      auto it = checked_.refs.find(&e);
      if (it == checked_.refs.end() || it->second.kind != RefKind::Local) return false;
      uint32_t id = it->second.local_id;
      out = checker_place(c, id);
      if (out.is_none()) return false;
      return true;
    }
    case ExprKind::Field: {
      auto& f = static_cast<const ast::FieldExpr&>(e);
      auto it = checked_.refs.find(&e);
      if (it == checked_.refs.end()) return false;
      const NameRef& ref = it->second;
      if (ref.kind != RefKind::Field && ref.kind != RefKind::TupleField) return false;
      Place base;
      if (!lower_place(c, *f.base, base)) {
        // Base is a temporary: materialize it and drop it with the scope.
        Operand o = lower_operand(c, *f.base);
        if (o.kind == Operand::Kind::Const) return false;
        base = o.place;
        if (base.is_local()) scope_temporary(c, base.local);
      }
      for (int i = 0; i < ref.auto_derefs; ++i) base = base.deref();
      out = base.field(ref.field_index);
      return true;
    }
    case ExprKind::Index: {
      auto& ix = static_cast<const ast::IndexExpr&>(e);
      auto it = checked_.refs.find(&e);
      int derefs = it != checked_.refs.end() ? it->second.auto_derefs : 0;
      const Type* bt = type_of(c, *ix.base);
      const Type* base_t = bt;
      for (int i = 0; i < derefs; ++i) base_t = base_t->inner;
      const Type* idx_t = type_of(c, *ix.index);
      if (idx_t->kind == TypeKind::Struct) {
        error(e.span, "slicing with a range is not supported in code generation yet");
        return false;
      }
      Place base;
      if (!lower_place(c, *ix.base, base)) {
        Operand o = lower_operand(c, *ix.base);
        if (o.kind == Operand::Kind::Const) return false;
        base = o.place;
        if (base.is_local()) scope_temporary(c, base.local);
      }
      for (int i = 0; i < derefs; ++i) base = base.deref();
      Operand idx = lower_operand(c, *ix.index);
      uint32_t idx_local = new_temp(c, ctx_.usize_type(), ix.index->span);
      Rvalue use; use.kind = RvalueKind::Use; use.a = idx;
      assign(c, Place::of(idx_local), use, ix.index->span);
      emit_bounds_check(c, idx_local, base, base_t, e.span);
      if (base_t->kind == TypeKind::Struct) out = base.field(0).index(idx_local);
      else out = base.index(idx_local);
      return true;
    }
    case ExprKind::Unary: {
      auto& u = static_cast<const ast::UnaryExpr&>(e);
      if (u.op != ast::UnaryOp::Deref) return false;
      Place base;
      if (!lower_place(c, *u.operand, base)) {
        Operand o = lower_operand(c, *u.operand);
        if (o.kind == Operand::Kind::Const) return false;
        base = o.place;
        if (base.is_local()) scope_temporary(c, base.local);
      }
      out = base.deref();
      return true;
    }
    default:
      return false;
  }
}

void Lowerer::emit_bounds_check(FnCtx& c, uint32_t index_local, Place container, const Type* container_t, Span span) {
  uint32_t len = new_temp(c, ctx_.usize_type(), span);
  Rvalue lenv;
  lenv.kind = RvalueKind::Len;
  lenv.place = container;
  lenv.type = ctx_.usize_type();
  (void)container_t;
  assign(c, Place::of(len), lenv, span);
  uint32_t ok = new_temp(c, ctx_.bool_type(), span);
  Rvalue cmp;
  cmp.kind = RvalueKind::Binary;
  cmp.bop = ast::BinaryOp::Lt;
  cmp.a = Operand::copy(Place::of(index_local));
  cmp.b = Operand::copy(Place::of(len));
  cmp.type = ctx_.bool_type();
  assign(c, Place::of(ok), cmp, span);
  Terminator t;
  t.kind = Terminator::Kind::Assert;
  t.cond = Operand::copy(Place::of(ok));
  t.message = "index out of bounds";
  t.span = span;
  uint32_t next = new_block(c);
  t.target = next;
  set_term(c, t);
  c.current = next;
}

Operand Lowerer::operand_from_place(FnCtx& c, Place p, const Type* t) {
  (void)c;
  if (is_copy_type(t)) return Operand::copy(std::move(p));
  return Operand::move(std::move(p));
}

// ---------- expressions ----------

Operand Lowerer::apply_coercion(FnCtx& c, const ast::Expr& e, Operand op, const Type* from) {
  auto it = checked_.coercions.find(&e);
  if (it == checked_.coercions.end()) return op;
  const sema::Coercion& co = it->second;
  auto borrow = [&](Operand o, bool is_const) {
    Place p;
    if (o.kind == Operand::Kind::Const) {
      uint32_t tmp = new_temp(c, from, e.span);
      Rvalue use; use.kind = RvalueKind::Use; use.a = o;
      assign(c, Place::of(tmp), use, e.span);
      p = Place::of(tmp);
      c.scopes.back().push_back(tmp);
    } else {
      p = o.place;
      scope_temporary(c, p.local);
    }
    uint32_t r = new_temp(c, ctx_.ref(from, is_const), e.span);
    Rvalue rv;
    rv.kind = RvalueKind::Ref;
    rv.place = p;
    rv.is_const = is_const;
    rv.type = c.fn->locals[r].type;
    assign(c, Place::of(r), rv, e.span);
    return Operand::copy(Place::of(r));
  };
  switch (co.kind) {
    case CoercionKind::None:
    case CoercionKind::MutToConst:
    case CoercionKind::NeverToAny:
      return op;
    case CoercionKind::AutoBorrowConst: return borrow(op, true);
    case CoercionKind::AutoBorrowMut: return borrow(op, false);
    case CoercionKind::ToDyn: {
      Operand ref = co.borrow_first ? borrow(op, true) : op;
      const Type* concrete = co.borrow_first ? from : from->inner;
      const Type* target = subst_type(c, co.target);
      Rvalue rv;
      rv.kind = RvalueKind::ToDyn;
      rv.a = ref;
      rv.type = target;
      rv.vtable = vtable_for(concrete, target->inner, e.span);
      uint32_t local = new_temp(c, target, e.span);
      assign(c, Place::of(local), rv, e.span);
      return Operand::copy(Place::of(local));
    }
    case CoercionKind::FnMode: {
      auto* target = subst_type(c, co.target);
      uint32_t value = new_temp(c, target, e.span);
      Rvalue r; r.kind = RvalueKind::Use; r.a = op;
      assign(c, Place::of(value), r, e.span);
      return Operand::move(Place::of(value));
    }
    case CoercionKind::StringToStr:
    case CoercionKind::ArrayToSlice:
    case CoercionKind::VecToSlice: {
      Operand ref = co.borrow_first ? borrow(op, true) : op;
      const Type* ref_t = co.borrow_first ? ctx_.ref(from, true) : from;
      const Type* inner = ref_t->inner;
      const Type* result;
      Rvalue rv;
      if (co.kind == CoercionKind::StringToStr) { rv.kind = RvalueKind::StrFromString; result = ctx_.ref(ctx_.str(), true); }
      else if (co.kind == CoercionKind::ArrayToSlice) { rv.kind = RvalueKind::ArrayToSlice; result = ctx_.ref(ctx_.slice(inner->inner), ref_t->is_const); }
      else { rv.kind = RvalueKind::VecToSlice; result = ctx_.ref(ctx_.slice(inner->elems[0]), ref_t->is_const); }
      rv.a = ref;
      rv.type = result;
      uint32_t r = new_temp(c, result, e.span);
      assign(c, Place::of(r), rv, e.span);
      return Operand::copy(Place::of(r));
    }
  }
  return op;
}

void Lowerer::lower_into(FnCtx& c, const ast::Expr& e, Place dest) {
  switch (e.kind) {
    case ExprKind::If:
      if (!checked_.coercions.count(&e)) { lower_if(c, static_cast<const ast::IfExpr&>(e), &dest); return; }
      break;
    case ExprKind::Match:
      if (!checked_.coercions.count(&e)) { lower_match(c, static_cast<const ast::MatchExpr&>(e), &dest); return; }
      break;
    case ExprKind::Block:
      if (!checked_.coercions.count(&e)) { lower_block_into(c, *static_cast<const ast::BlockExpr&>(e).block, &dest); return; }
      break;
    case ExprKind::Interp:
      if (!checked_.coercions.count(&e)) { lower_interp(c, static_cast<const ast::InterpExpr&>(e), dest); return; }
      break;
    case ExprKind::Call:
      if (!checked_.coercions.count(&e)) { lower_call(c, static_cast<const ast::CallExpr&>(e), &dest); return; }
      break;
    default:
      break;
  }
  Operand o = lower_operand(c, e);
  if (block_terminated(c)) return;
  Rvalue r;
  r.kind = RvalueKind::Use;
  r.a = std::move(o);
  assign(c, std::move(dest), std::move(r), e.span);
}

Operand Lowerer::lower_const(FnCtx& c, const sema::ConstDef& constant) {
  auto locals = checked_.const_locals.find(&constant);
  if (locals == checked_.const_locals.end()) { error(constant.span, "internal error: constant was not checked"); return Operand(); }
  auto saved_map = std::move(c.local_map);
  auto saved_captures = std::move(c.captures);
  auto* saved_constant = c.constant;
  c.constant = &constant;
  c.local_map.assign(locals->second.size(), UINT32_MAX);
  c.captures.clear();
  const Type* type = subst_type(c, constant.type);
  auto result = Place::of(new_temp(c, type, constant.span));
  push_scope(c);
  lower_into(c, *constant.ast->value, result);
  pop_scope(c, constant.span);
  c.local_map = std::move(saved_map);
  c.captures = std::move(saved_captures);
  c.constant = saved_constant;
  return operand_from_place(c, result, type);
}

Operand Lowerer::lower_operand(FnCtx& c, const ast::Expr& e) {
  const Type* t = type_of(c, e);
  auto temp_of = [&](Rvalue r) {
    uint32_t tmp = new_temp(c, t, e.span);
    assign(c, Place::of(tmp), std::move(r), e.span);
    return operand_from_place(c, Place::of(tmp), t);
  };
  auto finish = [&](Operand o) { return apply_coercion(c, e, std::move(o), t); };

  switch (e.kind) {
    case ExprKind::IntLit: {
      auto& l = static_cast<const ast::IntLitExpr&>(e);
      Constant k; k.kind = Constant::Kind::Int; k.int_value = l.value; k.type = t;
      return finish(Operand::constant_of(k));
    }
    case ExprKind::FloatLit: {
      auto& l = static_cast<const ast::FloatLitExpr&>(e);
      Constant k; k.kind = Constant::Kind::Float; k.float_value = l.value; k.type = t;
      return finish(Operand::constant_of(k));
    }
    case ExprKind::CharLit: {
      Constant k; k.kind = Constant::Kind::Char; k.int_value = static_cast<const ast::CharLitExpr&>(e).value; k.type = t;
      return finish(Operand::constant_of(k));
    }
    case ExprKind::BoolLit: {
      Constant k; k.kind = Constant::Kind::Bool; k.int_value = static_cast<const ast::BoolLitExpr&>(e).value ? 1 : 0; k.type = t;
      return finish(Operand::constant_of(k));
    }
    case ExprKind::StringLit: {
      Constant k; k.kind = Constant::Kind::Str; k.str_value = static_cast<const ast::StringLitExpr&>(e).value; k.type = t;
      return finish(Operand::constant_of(k));
    }
    case ExprKind::Interp: {
      uint32_t tmp = new_temp(c, t, e.span);
      lower_interp(c, static_cast<const ast::InterpExpr&>(e), Place::of(tmp));
      return finish(Operand::move(Place::of(tmp)));
    }
    case ExprKind::Name: {
      auto it = checked_.refs.find(&e);
      if (it == checked_.refs.end()) { error(e.span, "unresolved name"); return Operand(); }
      const NameRef& ref = it->second;
      switch (ref.kind) {
        case RefKind::Local: {
          Place p;
          if (!lower_place(c, e, p)) { error(e.span, "unmapped local"); return Operand(); }
          return finish(operand_from_place(c, p, t));
        }
        case RefKind::Function: {
          if (ref.fn->is_builtin || ref.fn->owner == sema::FnOwner::Extern) {
            error(e.span, "using a builtin or extern function as a function value is not supported yet");
            return Operand();
          }
          std::vector<const Type*> args;
          for (auto* a : ref.type_args) args.push_back(subst_type(c, a));
          Function* f = instance(*ref.fn, {}, args);
          Constant k; k.kind = Constant::Kind::Fn; k.fn = f; k.type = t;
          return finish(Operand::constant_of(k));
        }
        case RefKind::Variant: {
          if (ref.variant->kind == ast::VariantKind::Unit) {
            Rvalue r;
            r.kind = RvalueKind::Aggregate;
            r.agg = AggregateKind::Variant;
            r.variant = ref.variant->index;
            r.type = t;
            return finish(temp_of(r));
          }
          error(e.span, "using a variant constructor as a function value is not supported yet");
          return Operand();
        }
        case RefKind::Const: {
          // Constants are re-evaluated at each use for now.
          return finish(lower_const(c, *ref.const_def));
        }
        default:
          error(e.span, "this name cannot be used as a value");
          return Operand();
      }
    }
    case ExprKind::Field: {
      auto it = checked_.refs.find(&e);
      if (it != checked_.refs.end() && (it->second.kind == RefKind::Field || it->second.kind == RefKind::TupleField)) {
        Place p;
        if (!lower_place(c, e, p)) { error(e.span, "cannot read this field"); return Operand(); }
        return finish(operand_from_place(c, p, t));
      }
      if (it != checked_.refs.end() && it->second.kind == RefKind::Variant) {
        const NameRef& ref = it->second;
        if (ref.variant->kind == ast::VariantKind::Unit) {
          Rvalue r;
          r.kind = RvalueKind::Aggregate;
          r.agg = AggregateKind::Variant;
          r.variant = ref.variant->index;
          r.type = t;
          return finish(temp_of(r));
        }
      }
      if (it != checked_.refs.end() && (it->second.kind == RefKind::AssocFn || it->second.kind == RefKind::Function)) {
        const NameRef& ref = it->second;
        if (ref.fn->is_builtin || ref.fn->owner == sema::FnOwner::Extern) {
          error(e.span, "using a builtin or extern function as a function value is not supported yet");
          return Operand();
        }
        std::vector<const Type*> owner, args;
        for (auto* a : ref.owner_args) owner.push_back(subst_type(c, a));
        for (auto* a : ref.type_args) args.push_back(subst_type(c, a));
        Function* f = instance(*ref.fn, owner, args);
        Constant k; k.kind = Constant::Kind::Fn; k.fn = f; k.type = t;
        return finish(Operand::constant_of(k));
      }
      if (it != checked_.refs.end() && it->second.kind == RefKind::Const)
        return finish(lower_const(c, *it->second.const_def));
      error(e.span, "this member access cannot be used as a value yet");
      return Operand();
    }
    case ExprKind::Call: return finish(lower_call(c, static_cast<const ast::CallExpr&>(e), nullptr));
    case ExprKind::Index: {
      Place p;
      if (!lower_place(c, e, p)) return Operand();
      return finish(operand_from_place(c, p, t));
    }
    case ExprKind::Unary: {
      auto& u = static_cast<const ast::UnaryExpr&>(e);
      if (u.op == ast::UnaryOp::Deref) {
        Place p;
        if (!lower_place(c, e, p)) return Operand();
        return finish(operand_from_place(c, p, t));
      }
      if (u.op == ast::UnaryOp::Neg && t->is_prim(ast::PrimKind::Long) && u.operand->kind == ExprKind::IntLit &&
          static_cast<const ast::IntLitExpr&>(*u.operand).value == (uint64_t(1) << 63)) {
        Constant k; k.kind = Constant::Kind::Int; k.int_value = uint64_t(1) << 63; k.type = t;
        return finish(Operand::constant_of(k));
      }
      const Type* ot = type_of(c, *u.operand);
      if ((u.op == ast::UnaryOp::Neg || u.op == ast::UnaryOp::Not) &&
          (ot->kind == TypeKind::Struct || ot->kind == TypeKind::Enum)) {
        return finish(lower_unary_trait(c, u, ot, t, e.span));
      }
      Rvalue r;
      r.kind = RvalueKind::Unary;
      r.uop = u.op;
      r.a = lower_operand(c, *u.operand);
      r.type = t;
      return finish(temp_of(r));
    }
    case ExprKind::Binary: return finish(lower_binary(c, static_cast<const ast::BinaryExpr&>(e), nullptr));
    case ExprKind::Cast: {
      auto& ca = static_cast<const ast::CastExpr&>(e);
      Rvalue r;
      r.kind = RvalueKind::Cast;
      r.a = lower_operand(c, *ca.operand);
      r.type = t;
      return finish(temp_of(r));
    }
    case ExprKind::Try: return finish(lower_try(c, static_cast<const ast::TryExpr&>(e)));
    case ExprKind::OptChain: {
      error(e.span, "'?.' is not supported in code generation yet");
      return Operand();
    }
    case ExprKind::Coalesce: {
      auto& co = static_cast<const ast::CoalesceExpr&>(e);
      const Type* vt = type_of(c, *co.value);
      Operand v = lower_operand(c, *co.value);
      uint32_t vl = new_temp(c, vt, co.value->span);
      Rvalue use; use.kind = RvalueKind::Use; use.a = v;
      assign(c, Place::of(vl), use, co.value->span);
      uint32_t result = new_temp(c, t, e.span);
      uint32_t disc = new_temp(c, ctx_.usize_type(), e.span);
      Rvalue d; d.kind = RvalueKind::Discriminant; d.place = Place::of(vl); d.type = ctx_.usize_type();
      assign(c, Place::of(disc), d, e.span);
      uint32_t some_b = new_block(c), none_b = new_block(c), join = new_block(c);
      Terminator sw;
      sw.kind = Terminator::Kind::Switch;
      sw.discr = Operand::copy(Place::of(disc));
      const sema::VariantDef* some = prog_.option_def->find_variant("Some");
      sw.cases = {{some->index, some_b}};
      sw.otherwise = none_b;
      sw.span = e.span;
      set_term(c, sw);
      c.current = some_b;
      Rvalue take; take.kind = RvalueKind::Use;
      take.a = operand_from_place(c, Place::of(vl).downcast(some->index).field(0), t);
      assign(c, Place::of(result), take, e.span);
      goto_block(c, join);
      c.current = none_b;
      lower_into(c, *co.fallback, Place::of(result));
      goto_block(c, join);
      c.current = join;
      return finish(operand_from_place(c, Place::of(result), t));
    }
    case ExprKind::Range: {
      auto& r = static_cast<const ast::RangeExpr&>(e);
      Rvalue agg;
      agg.kind = RvalueKind::Aggregate;
      agg.agg = AggregateKind::Struct;
      agg.type = t;
      agg.elems.push_back(lower_operand(c, *r.start));
      agg.elems.push_back(lower_operand(c, *r.end));
      return finish(temp_of(agg));
    }
    case ExprKind::Ref: {
      auto& r = static_cast<const ast::RefExpr&>(e);
      Place p;
      if (!lower_place(c, *r.operand, p)) {
        const Type* ot = type_of(c, *r.operand);
        Operand o = lower_operand(c, *r.operand);
        uint32_t tmp = new_temp(c, ot, r.operand->span);
        Rvalue use; use.kind = RvalueKind::Use; use.a = o;
        assign(c, Place::of(tmp), use, r.operand->span);
        c.scopes.back().push_back(tmp);
        p = Place::of(tmp);
      }
      Rvalue rv;
      rv.kind = RvalueKind::Ref;
      rv.place = p;
      rv.is_const = r.is_const;
      rv.type = t;
      return finish(temp_of(rv));
    }
    case ExprKind::StructLit: {
      auto& s = static_cast<const ast::StructLitExpr&>(e);
      auto it = checked_.refs.find(&e);
      Rvalue agg;
      agg.kind = RvalueKind::Aggregate;
      agg.type = t;
      const std::vector<sema::FieldDef>* fields;
      if (it != checked_.refs.end() && it->second.kind == RefKind::Variant) {
        agg.agg = AggregateKind::Variant;
        agg.variant = it->second.variant->index;
        fields = &it->second.variant->fields;
      } else {
        agg.agg = AggregateKind::Struct;
        fields = &t->struct_def->fields;
      }
      // Evaluate in source order, then place in field order.
      std::vector<Operand> values(fields->size());
      for (auto& init : s.fields) {
        size_t idx = 0;
        for (; idx < fields->size(); ++idx) if ((*fields)[idx].name == init.name) break;
        if (idx == fields->size()) continue;
        if (init.value) {
          values[idx] = lower_operand(c, *init.value);
        } else {
          // Shorthand: local with that name.
          auto found = checked_.shorthand_locals.find(&init);
          auto& locals = checker_locals(c);
          if (found == checked_.shorthand_locals.end()) { error(init.span, "shorthand field has no local"); continue; }
          Place p = checker_place(c, found->second);
          if (p.is_none()) { error(init.span, "shorthand field has no local"); continue; }
          values[idx] = operand_from_place(c, p, subst_type(c, locals[found->second].type));
        }
      }
      agg.elems = std::move(values);
      return finish(temp_of(agg));
    }
    case ExprKind::Tuple: {
      auto& tu = static_cast<const ast::TupleExpr&>(e);
      if (tu.elems.empty()) {
        Constant k; k.kind = Constant::Kind::Unit; k.type = t;
        return finish(Operand::constant_of(k));
      }
      Rvalue agg;
      agg.kind = RvalueKind::Aggregate;
      agg.agg = AggregateKind::Tuple;
      agg.type = t;
      for (auto& el : tu.elems) agg.elems.push_back(lower_operand(c, *el));
      return finish(temp_of(agg));
    }
    case ExprKind::Array: {
      auto& a = static_cast<const ast::ArrayExpr&>(e);
      Rvalue agg;
      agg.kind = RvalueKind::Aggregate;
      agg.agg = AggregateKind::Array;
      agg.type = t;
      for (auto& el : a.elems) agg.elems.push_back(lower_operand(c, *el));
      return finish(temp_of(agg));
    }
    case ExprKind::ArrayRepeat: {
      auto& a = static_cast<const ast::ArrayRepeatExpr&>(e);
      Rvalue agg;
      agg.kind = RvalueKind::Aggregate;
      agg.agg = AggregateKind::Array;
      agg.type = t;
      Operand v = lower_operand(c, *a.value);
      if (v.kind != Operand::Kind::Const && !is_copy_type(t->inner)) {
        error(e.span, "array repeat needs a Copy element type");
        return Operand();
      }
      for (uint64_t i = 0; i < t->length; ++i) agg.elems.push_back(v);
      return finish(temp_of(agg));
    }
    case ExprKind::Block: {
      if (t->is_void() || t->is_never()) {
        lower_block_into(c, *static_cast<const ast::BlockExpr&>(e).block, nullptr);
        Constant k; k.kind = Constant::Kind::Unit; k.type = ctx_.void_type();
        return Operand::constant_of(k);
      }
      uint32_t tmp = new_temp(c, t, e.span);
      Place p = Place::of(tmp);
      lower_block_into(c, *static_cast<const ast::BlockExpr&>(e).block, &p);
      return finish(operand_from_place(c, p, t));
    }
    case ExprKind::If: {
      if (t->is_void() || t->is_never()) {
        lower_if(c, static_cast<const ast::IfExpr&>(e), nullptr);
        Constant k; k.kind = Constant::Kind::Unit; k.type = ctx_.void_type();
        return Operand::constant_of(k);
      }
      uint32_t tmp = new_temp(c, t, e.span);
      Place p = Place::of(tmp);
      lower_if(c, static_cast<const ast::IfExpr&>(e), &p);
      return finish(operand_from_place(c, p, t));
    }
    case ExprKind::Match: {
      if (t->is_void() || t->is_never()) {
        lower_match(c, static_cast<const ast::MatchExpr&>(e), nullptr);
        Constant k; k.kind = Constant::Kind::Unit; k.type = ctx_.void_type();
        return Operand::constant_of(k);
      }
      uint32_t tmp = new_temp(c, t, e.span);
      Place p = Place::of(tmp);
      lower_match(c, static_cast<const ast::MatchExpr&>(e), &p);
      return finish(operand_from_place(c, p, t));
    }
    case ExprKind::Closure:
      return finish(lower_closure(c, static_cast<const ast::ClosureExpr&>(e)));
    case ExprKind::Unsafe: {
      auto& u = static_cast<const ast::UnsafeExpr&>(e);
      if (t->is_void() || t->is_never()) {
        lower_block_into(c, *u.block, nullptr);
        Constant k; k.kind = Constant::Kind::Unit; k.type = ctx_.void_type();
        return Operand::constant_of(k);
      }
      uint32_t tmp = new_temp(c, t, e.span);
      Place p = Place::of(tmp);
      lower_block_into(c, *u.block, &p);
      return finish(operand_from_place(c, p, t));
    }
    case ExprKind::Await:
      return finish(lower_await(c, static_cast<const ast::AwaitExpr&>(e)));
    case ExprKind::Return: {
      auto& r = static_cast<const ast::ReturnExpr&>(e);
      lower_return(c, r.value.get(), e.span);
      Constant k; k.kind = Constant::Kind::Unit; k.type = ctx_.never();
      return Operand::constant_of(k);
    }
    case ExprKind::Break: {
      if (c.loops.empty()) { error(e.span, "break outside loop"); return Operand(); }
      emit_drops_to(c, c.loops.back().scope_depth, e.span);
      Terminator t2;
      t2.kind = Terminator::Kind::Goto;
      t2.target = c.loops.back().break_block;
      t2.span = e.span;
      set_term(c, t2);
      Constant k; k.kind = Constant::Kind::Unit; k.type = ctx_.never();
      return Operand::constant_of(k);
    }
    case ExprKind::Continue: {
      if (c.loops.empty()) { error(e.span, "continue outside loop"); return Operand(); }
      emit_drops_to(c, c.loops.back().scope_depth, e.span);
      Terminator t2;
      t2.kind = Terminator::Kind::Goto;
      t2.target = c.loops.back().continue_block;
      t2.span = e.span;
      set_term(c, t2);
      Constant k; k.kind = Constant::Kind::Unit; k.type = ctx_.never();
      return Operand::constant_of(k);
    }
  }
  return Operand();
}

Operand Lowerer::lower_binary(FnCtx& c, const ast::BinaryExpr& e, Place* dest) {
  const Type* t = type_of(c, e);
  if (e.op == ast::BinaryOp::And || e.op == ast::BinaryOp::Or) {
    // Short circuit.
    uint32_t result = dest && dest->is_local() ? dest->local : new_temp(c, t, e.span);
    Operand l = lower_operand(c, *e.lhs);
    Rvalue use; use.kind = RvalueKind::Use; use.a = l;
    assign(c, Place::of(result), use, e.lhs->span);
    uint32_t rhs_b = new_block(c), join = new_block(c);
    Terminator sw;
    sw.kind = Terminator::Kind::Switch;
    sw.discr = Operand::copy(Place::of(result));
    sw.cases = {{e.op == ast::BinaryOp::And ? 1u : 0u, rhs_b}};
    sw.otherwise = join;
    sw.span = e.span;
    set_term(c, sw);
    c.current = rhs_b;
    lower_into(c, *e.rhs, Place::of(result));
    goto_block(c, join);
    c.current = join;
    if (dest && !dest->is_local()) {
      Rvalue u2; u2.kind = RvalueKind::Use; u2.a = Operand::copy(Place::of(result));
      assign(c, *dest, u2, e.span);
    }
    return Operand::copy(Place::of(result));
  }

  const Type* lt = type_of(c, *e.lhs);
  const Type* lbase = lt;
  while (lbase->kind == TypeKind::Ref) lbase = lbase->inner;
  bool is_cmp = e.op == ast::BinaryOp::Eq || e.op == ast::BinaryOp::Ne || e.op == ast::BinaryOp::Lt ||
                e.op == ast::BinaryOp::Gt || e.op == ast::BinaryOp::Le || e.op == ast::BinaryOp::Ge;
  bool builtin = lbase->is_numeric() || lbase->is_bool() || lbase->is_prim(ast::PrimKind::Char) ||
                 lbase->kind == TypeKind::Ptr;
  bool is_str = lbase->kind == TypeKind::Str || (lbase->kind == TypeKind::Struct && lbase->struct_def == prog_.string_def);

  if (builtin) {
    Operand l = lower_operand(c, *e.lhs);
    Operand r = lower_operand(c, *e.rhs);
    if (is_cmp) {
      // Compare through references.
      auto strip = [&](Operand o, const Type* ot) {
        while (ot->kind == TypeKind::Ref && o.kind != Operand::Kind::Const) {
          o.place = o.place.deref();
          o.kind = Operand::Kind::Copy;
          ot = ot->inner;
        }
        return o;
      };
      l = strip(l, lt);
      r = strip(r, type_of(c, *e.rhs));
    }
    Rvalue rv;
    rv.kind = RvalueKind::Binary;
    rv.bop = e.op;
    rv.a = l;
    rv.b = r;
    rv.type = t;
    if (dest) { assign(c, *dest, rv, e.span); return Operand::copy(*dest); }
    uint32_t tmp = new_temp(c, t, e.span);
    assign(c, Place::of(tmp), rv, e.span);
    return Operand::copy(Place::of(tmp));
  }

  if (is_str && is_cmp) {
    // Runtime string comparison on &const str.
    auto as_str = [&](const ast::Expr& x) {
      const Type* xt = type_of(c, x);
      Operand o = lower_operand(c, x);
      const Type* base = xt;
      while (base->kind == TypeKind::Ref) base = base->inner;
      if (base->kind == TypeKind::Str) {
        if (xt->kind != TypeKind::Ref) { error(x.span, "str value without reference"); return o; }
        while (xt->inner->kind == TypeKind::Ref) { o.place = o.place.deref(); o.kind = Operand::Kind::Copy; xt = xt->inner; }
        return o;
      }
      // String: borrow then StrFromString.
      Place p;
      if (o.kind == Operand::Kind::Const) { error(x.span, "unexpected constant"); return o; }
      p = o.place;
      scope_temporary(c, p.local);
      while (xt->kind == TypeKind::Ref) { p = p.deref(); xt = xt->inner; }
      uint32_t r = new_temp(c, ctx_.ref(xt, true), x.span);
      Rvalue rv; rv.kind = RvalueKind::Ref; rv.place = p; rv.is_const = true; rv.type = c.fn->locals[r].type;
      assign(c, Place::of(r), rv, x.span);
      uint32_t s = new_temp(c, ctx_.ref(ctx_.str(), true), x.span);
      Rvalue conv; conv.kind = RvalueKind::StrFromString; conv.a = Operand::copy(Place::of(r)); conv.type = c.fn->locals[s].type;
      assign(c, Place::of(s), conv, x.span);
      return Operand::copy(Place::of(s));
    };
    Operand l = as_str(*e.lhs);
    Operand r = as_str(*e.rhs);
    uint32_t cmp = new_temp(c, ctx_.int_type(), e.span);
    Callee callee;
    callee.kind = Callee::Kind::Builtin;
    callee.builtin = "str_cmp";
    call(c, callee, {l, r}, Place::of(cmp), false, e.span);
    Rvalue rv;
    rv.kind = RvalueKind::Binary;
    rv.bop = e.op;
    rv.a = Operand::copy(Place::of(cmp));
    Constant zero; zero.kind = Constant::Kind::Int; zero.int_value = 0; zero.type = ctx_.int_type();
    rv.b = Operand::constant_of(zero);
    rv.type = t;
    if (dest) { assign(c, *dest, rv, e.span); return Operand::copy(*dest); }
    uint32_t tmp = new_temp(c, t, e.span);
    assign(c, Place::of(tmp), rv, e.span);
    return Operand::copy(Place::of(tmp));
  }

  // Operator trait: lhs.op(rhs) on a user type.
  const char* method = nullptr;
  const char* trait = nullptr;
  switch (e.op) {
    case ast::BinaryOp::Add: method = "add"; trait = "Add"; break;
    case ast::BinaryOp::Sub: method = "sub"; trait = "Sub"; break;
    case ast::BinaryOp::Mul: method = "mul"; trait = "Mul"; break;
    case ast::BinaryOp::Div: method = "div"; trait = "Div"; break;
    case ast::BinaryOp::Rem: method = "rem"; trait = "Rem"; break;
    case ast::BinaryOp::Eq: case ast::BinaryOp::Ne: method = "eq"; trait = "Eq"; break;
    default: method = "lt"; trait = "Ord"; break;
  }
  sema::TraitDef* td = prog_.core_trait(trait);
  sema::FunctionDef* tm = td ? td->find_method(method) : nullptr;
  if (!tm) { error(e.span, "operator trait not available"); return Operand(); }
  Function* target = resolve_trait_method(*tm, lbase, {}, e.span);
  if (!target) return Operand();
  std::vector<Operand> args;
  if (is_cmp) {
    // eq and lt take &const self, &const other.
    auto borrowed = [&](const ast::Expr& x) {
      const Type* xt = type_of(c, x);
      Operand o = lower_operand(c, x);
      if (xt->kind == TypeKind::Ref) return o;
      Place p = o.kind == Operand::Kind::Const ? Place::none() : o.place;
      if (o.kind == Operand::Kind::Const) {
        uint32_t tmp = new_temp(c, xt, x.span);
        Rvalue use; use.kind = RvalueKind::Use; use.a = o;
        assign(c, Place::of(tmp), use, x.span);
        p = Place::of(tmp);
      }
      uint32_t r = new_temp(c, ctx_.ref(xt, true), x.span);
      Rvalue rv; rv.kind = RvalueKind::Ref; rv.place = p; rv.is_const = true; rv.type = c.fn->locals[r].type;
      assign(c, Place::of(r), rv, x.span);
      return Operand::copy(Place::of(r));
    };
    args.push_back(borrowed(*e.lhs));
    args.push_back(borrowed(*e.rhs));
    // Operands are evaluated left to right. a > b calls b.lt(a); a <= b is !(b.lt(a)).
    if (e.op == ast::BinaryOp::Gt || e.op == ast::BinaryOp::Le) std::swap(args[0], args[1]);
  } else {
    args.push_back(lower_operand(c, *e.lhs));
    args.push_back(lower_operand(c, *e.rhs));
  }
  uint32_t res = new_temp(c, t, e.span);
  Callee callee;
  callee.kind = Callee::Kind::Direct;
  callee.fn = target;
  call(c, callee, std::move(args), Place::of(res), false, e.span);
  Operand out = operand_from_place(c, Place::of(res), t);
  bool negate = e.op == ast::BinaryOp::Ne || e.op == ast::BinaryOp::Le || e.op == ast::BinaryOp::Ge;
  if (negate) {
    Rvalue n;
    n.kind = RvalueKind::Unary;
    n.uop = ast::UnaryOp::Not;
    n.a = out;
    n.type = ctx_.bool_type();
    uint32_t nt = new_temp(c, ctx_.bool_type(), e.span);
    assign(c, Place::of(nt), n, e.span);
    out = Operand::copy(Place::of(nt));
  }
  if (dest) {
    Rvalue use; use.kind = RvalueKind::Use; use.a = out;
    assign(c, *dest, use, e.span);
    return Operand::copy(*dest);
  }
  return out;
}

Operand Lowerer::lower_unary_trait(FnCtx& c, const ast::UnaryExpr& u, const Type* operand_type, const Type* t,
                                   Span span) {
  bool neg = u.op == ast::UnaryOp::Neg;
  sema::TraitDef* td = prog_.core_trait(neg ? "Neg" : "Not");
  sema::FunctionDef* tm = td ? td->find_method(neg ? "neg" : "not") : nullptr;
  if (!tm) { error(span, "operator trait not available"); return Operand(); }
  Function* target = resolve_trait_method(*tm, operand_type, {}, span);
  if (!target) return Operand();
  std::vector<Operand> args;
  args.push_back(lower_operand(c, *u.operand));
  uint32_t res = new_temp(c, t, span);
  Callee callee;
  callee.kind = Callee::Kind::Direct;
  callee.fn = target;
  call(c, callee, std::move(args), Place::of(res), false, span);
  return operand_from_place(c, Place::of(res), t);
}

void Lowerer::lower_if(FnCtx& c, const ast::IfExpr& e, Place* dest) {
  Operand cond = lower_operand(c, *e.cond);
  uint32_t then_b = new_block(c);
  uint32_t else_b = new_block(c);
  uint32_t join = new_block(c);
  Terminator sw;
  sw.kind = Terminator::Kind::Switch;
  sw.discr = cond;
  sw.cases = {{1, then_b}};
  sw.otherwise = else_b;
  sw.span = e.cond->span;
  set_term(c, sw);
  c.current = then_b;
  lower_block_into(c, *e.then_block, dest);
  if (!block_terminated(c)) goto_block(c, join);
  c.current = else_b;
  if (e.else_expr) {
    if (dest) lower_into(c, *e.else_expr, *dest);
    else { Operand o = lower_operand(c, *e.else_expr); (void)o; }
  }
  if (!block_terminated(c)) goto_block(c, join);
  c.current = join;
}

// ---------- patterns ----------

Place Lowerer::auto_deref(FnCtx& c, Place place, const Type*& t, int& ref_mode, int count) {
  (void)c;
  for (int i = 0; i < count && t->kind == TypeKind::Ref; ++i) {
    ref_mode = (ref_mode == 1 || t->is_const) ? 1 : 2;
    t = t->inner;
    place = place.deref();
  }
  return place;
}

uint32_t Lowerer::lower_pattern_test(FnCtx& c, const ast::Pattern& p, Place place, const Type* t, uint32_t fail) {
  auto derefs = checked_.pattern_derefs.find(&p);
  int ref_mode = 0;
  if (derefs != checked_.pattern_derefs.end()) place = auto_deref(c, place, t, ref_mode, derefs->second);

  auto switch_on = [&](Operand discr, uint64_t want) {
    uint32_t ok = new_block(c);
    Terminator sw;
    sw.kind = Terminator::Kind::Switch;
    sw.discr = discr;
    sw.cases = {{want, ok}};
    sw.otherwise = fail;
    sw.span = p.span;
    set_term(c, sw);
    c.current = ok;
    return ok;
  };
  auto variant_test = [&](uint32_t variant) {
    uint32_t disc = new_temp(c, ctx_.usize_type(), p.span);
    Rvalue d; d.kind = RvalueKind::Discriminant; d.place = place; d.type = ctx_.usize_type();
    assign(c, Place::of(disc), d, p.span);
    return switch_on(Operand::copy(Place::of(disc)), variant);
  };
  auto compare = [&](ast::BinaryOp op, Operand value) {
    uint32_t b = new_temp(c, ctx_.bool_type(), p.span);
    Rvalue cmp; cmp.kind = RvalueKind::Binary; cmp.bop = op; cmp.a = Operand::copy(place); cmp.b = value; cmp.type = ctx_.bool_type();
    assign(c, Place::of(b), cmp, p.span);
    return switch_on(Operand::copy(Place::of(b)), 1);
  };

  switch (p.kind) {
    case ast::PatternKind::Wildcard:
      return c.current;
    case ast::PatternKind::Binding: {
      if (checked_.pattern_locals.count(&p)) return c.current;
      // A unit variant named without a path.
      auto& b = static_cast<const ast::BindingPattern&>(p);
      const sema::VariantDef* v = nullptr;
      if (t->kind == TypeKind::Enum) v = t->enum_def->find_variant(b.name);
      if (!v) { error(p.span, "pattern variant not found"); return c.current; }
      return variant_test(v->index);
    }
    case ast::PatternKind::Literal: {
      auto& l = static_cast<const ast::LiteralPattern&>(p);
      if (t->kind == TypeKind::Str) {
        Operand lit = lower_operand(c, *l.value);
        uint32_t r = new_temp(c, ctx_.ref(ctx_.str(), true), p.span);
        Rvalue rv; rv.kind = RvalueKind::Ref; rv.place = place; rv.is_const = true; rv.type = c.fn->locals[r].type;
        assign(c, Place::of(r), rv, p.span);
        uint32_t cmp = new_temp(c, ctx_.int_type(), p.span);
        Callee callee; callee.kind = Callee::Kind::Builtin; callee.builtin = "str_cmp";
        call(c, callee, {Operand::copy(Place::of(r)), lit}, Place::of(cmp), false, p.span);
        Constant zero; zero.kind = Constant::Kind::Int; zero.int_value = 0; zero.type = ctx_.int_type();
        uint32_t b = new_temp(c, ctx_.bool_type(), p.span);
        Rvalue eq; eq.kind = RvalueKind::Binary; eq.bop = ast::BinaryOp::Eq; eq.a = Operand::copy(Place::of(cmp)); eq.b = Operand::constant_of(zero); eq.type = ctx_.bool_type();
        assign(c, Place::of(b), eq, p.span);
        return switch_on(Operand::copy(Place::of(b)), 1);
      }
      Operand lit = lower_operand(c, *l.value);
      if (lit.kind == Operand::Kind::Const && lit.constant.kind == Constant::Kind::Bool) {
        return switch_on(Operand::copy(place), lit.constant.int_value);
      }
      return compare(ast::BinaryOp::Eq, lit);
    }
    case ast::PatternKind::Range: {
      auto& r = static_cast<const ast::RangePattern&>(p);
      compare(ast::BinaryOp::Ge, lower_operand(c, *r.lo));
      return compare(r.inclusive ? ast::BinaryOp::Le : ast::BinaryOp::Lt, lower_operand(c, *r.hi));
    }
    case ast::PatternKind::Path: {
      auto& pp = static_cast<const ast::PathPattern&>(p);
      const sema::VariantDef* v = t->kind == TypeKind::Enum ? t->enum_def->find_variant(pp.path.segments.back().name) : nullptr;
      if (!v) { error(p.span, "pattern variant not found"); return c.current; }
      return variant_test(v->index);
    }
    case ast::PatternKind::TupleStruct: {
      auto& ts = static_cast<const ast::TupleStructPattern&>(p);
      const sema::VariantDef* v = t->kind == TypeKind::Enum ? t->enum_def->find_variant(ts.path.segments.back().name) : nullptr;
      if (!v) { error(p.span, "pattern variant not found"); return c.current; }
      variant_test(v->index);
      sema::Subst s;
      for (size_t i = 0; i < t->enum_def->generics.size(); ++i) s.bind(t->enum_def->generics[i].type, t->elems[i]);
      for (size_t i = 0; i < ts.elems.size() && i < v->tuple_types.size(); ++i) {
        const Type* ft = s.apply(ctx_, v->tuple_types[i]);
        lower_pattern_test(c, *ts.elems[i], place.downcast(v->index).field(static_cast<uint32_t>(i)), ft, fail);
      }
      return c.current;
    }
    case ast::PatternKind::Struct: {
      auto& sp = static_cast<const ast::StructPattern&>(p);
      const std::vector<sema::FieldDef>* fields = nullptr;
      Place base = place;
      sema::Subst s;
      if (t->kind == TypeKind::Enum) {
        const sema::VariantDef* v = t->enum_def->find_variant(sp.path.segments.back().name);
        if (!v) { error(p.span, "pattern variant not found"); return c.current; }
        variant_test(v->index);
        base = place.downcast(v->index);
        fields = &v->fields;
        for (size_t i = 0; i < t->enum_def->generics.size(); ++i) s.bind(t->enum_def->generics[i].type, t->elems[i]);
      } else if (t->kind == TypeKind::Struct) {
        fields = &t->struct_def->fields;
        for (size_t i = 0; i < t->struct_def->generics.size(); ++i) s.bind(t->struct_def->generics[i].type, t->elems[i]);
      } else {
        return c.current;
      }
      for (auto& fp : sp.fields) {
        if (!fp.pattern) continue;
        size_t idx = 0;
        for (; idx < fields->size(); ++idx) if ((*fields)[idx].name == fp.name) break;
        if (idx == fields->size()) continue;
        lower_pattern_test(c, *fp.pattern, base.field(static_cast<uint32_t>(idx)), s.apply(ctx_, (*fields)[idx].type), fail);
      }
      return c.current;
    }
    case ast::PatternKind::Tuple: {
      auto& tp = static_cast<const ast::TuplePattern&>(p);
      for (size_t i = 0; i < tp.elems.size() && i < t->elems.size(); ++i) {
        lower_pattern_test(c, *tp.elems[i], place.field(static_cast<uint32_t>(i)), t->elems[i], fail);
      }
      return c.current;
    }
    case ast::PatternKind::Or: {
      // Try each alternative; any success continues.
      auto& o = static_cast<const ast::OrPattern&>(p);
      uint32_t success = new_block(c);
      for (size_t i = 0; i < o.alts.size(); ++i) {
        uint32_t next_alt = i + 1 < o.alts.size() ? new_block(c) : fail;
        lower_pattern_test(c, *o.alts[i], place, t, next_alt);
        goto_block(c, success);
        c.current = next_alt;
        if (i + 1 == o.alts.size()) c.current = success;
      }
      c.current = success;
      return success;
    }
  }
  return c.current;
}

void Lowerer::lower_pattern_bind(FnCtx& c, const ast::Pattern& p, Place place, const Type* t, int ref_mode) {
  auto derefs = checked_.pattern_derefs.find(&p);
  if (derefs != checked_.pattern_derefs.end()) place = auto_deref(c, place, t, ref_mode, derefs->second);

  switch (p.kind) {
    case ast::PatternKind::Binding: {
      auto it = checked_.pattern_locals.find(&p);
      if (it == checked_.pattern_locals.end()) return;  // a unit variant, nothing to bind
      uint32_t checker_id = it->second;
      const Type* lt = subst_type(c, checker_locals(c)[checker_id].type);
      uint32_t local = new_temp(c, lt, p.span);
      c.fn->locals[local].name = static_cast<const ast::BindingPattern&>(p).name;
      if (checker_id >= c.local_map.size()) c.local_map.resize(checker_id + 1, UINT32_MAX);
      c.local_map[checker_id] = local;
      Rvalue r;
      if (ref_mode != 0) {
        r.kind = RvalueKind::Ref;
        r.place = place;
        r.is_const = ref_mode == 1;
        r.type = lt;
      } else {
        r.kind = RvalueKind::Use;
        r.a = operand_from_place(c, place, t);
      }
      assign(c, Place::of(local), r, p.span);
      c.scopes.back().push_back(local);
      return;
    }
    case ast::PatternKind::TupleStruct: {
      auto& ts = static_cast<const ast::TupleStructPattern&>(p);
      if (t->kind != TypeKind::Enum) return;
      const sema::VariantDef* v = t->enum_def->find_variant(ts.path.segments.back().name);
      if (!v) return;
      sema::Subst s;
      for (size_t i = 0; i < t->enum_def->generics.size(); ++i) s.bind(t->enum_def->generics[i].type, t->elems[i]);
      for (size_t i = 0; i < ts.elems.size() && i < v->tuple_types.size(); ++i) {
        lower_pattern_bind(c, *ts.elems[i], place.downcast(v->index).field(static_cast<uint32_t>(i)), s.apply(ctx_, v->tuple_types[i]), ref_mode);
      }
      return;
    }
    case ast::PatternKind::Struct: {
      auto& sp = static_cast<const ast::StructPattern&>(p);
      const std::vector<sema::FieldDef>* fields = nullptr;
      Place base = place;
      sema::Subst s;
      if (t->kind == TypeKind::Enum) {
        const sema::VariantDef* v = t->enum_def->find_variant(sp.path.segments.back().name);
        if (!v) return;
        base = place.downcast(v->index);
        fields = &v->fields;
        for (size_t i = 0; i < t->enum_def->generics.size(); ++i) s.bind(t->enum_def->generics[i].type, t->elems[i]);
      } else if (t->kind == TypeKind::Struct) {
        fields = &t->struct_def->fields;
        for (size_t i = 0; i < t->struct_def->generics.size(); ++i) s.bind(t->struct_def->generics[i].type, t->elems[i]);
      } else {
        return;
      }
      for (auto& fp : sp.fields) {
        size_t idx = 0;
        for (; idx < fields->size(); ++idx) if ((*fields)[idx].name == fp.name) break;
        if (idx == fields->size()) continue;
        const Type* ft = s.apply(ctx_, (*fields)[idx].type);
        Place fplace = base.field(static_cast<uint32_t>(idx));
        if (fp.pattern) {
          lower_pattern_bind(c, *fp.pattern, fplace, ft, ref_mode);
        } else {
          // Shorthand binding: the checker declared a local for it. Find it by name.
          auto& locals = checker_locals(c);
          uint32_t checker_id = UINT32_MAX;
          for (uint32_t k = 0; k < locals.size(); ++k) {
            if (locals[k].name == fp.name && locals[k].span.start == fp.span.start) checker_id = k;
          }
          if (checker_id == UINT32_MAX) { error(fp.span, "unmapped shorthand binding"); continue; }
          const Type* lt = subst_type(c, locals[checker_id].type);
          uint32_t local = new_temp(c, lt, fp.span);
          c.fn->locals[local].name = fp.name;
          if (checker_id >= c.local_map.size()) c.local_map.resize(checker_id + 1, UINT32_MAX);
          c.local_map[checker_id] = local;
          Rvalue r;
          if (ref_mode != 0) { r.kind = RvalueKind::Ref; r.place = fplace; r.is_const = ref_mode == 1; r.type = lt; }
          else { r.kind = RvalueKind::Use; r.a = operand_from_place(c, fplace, ft); }
          assign(c, Place::of(local), r, fp.span);
          c.scopes.back().push_back(local);
        }
      }
      return;
    }
    case ast::PatternKind::Tuple: {
      auto& tp = static_cast<const ast::TuplePattern&>(p);
      for (size_t i = 0; i < tp.elems.size() && i < t->elems.size(); ++i) {
        lower_pattern_bind(c, *tp.elems[i], place.field(static_cast<uint32_t>(i)), t->elems[i], ref_mode);
      }
      return;
    }
    default:
      return;  // wildcard, literal, range, path, or: no bindings
  }
}

void Lowerer::lower_match(FnCtx& c, const ast::MatchExpr& e, Place* dest) {
  const Type* st = type_of(c, *e.scrutinee);
  Place scrut;
  if (!lower_place(c, *e.scrutinee, scrut)) {
    Operand o = lower_operand(c, *e.scrutinee);
    uint32_t tmp = new_temp(c, st, e.scrutinee->span);
    Rvalue use; use.kind = RvalueKind::Use; use.a = o;
    assign(c, Place::of(tmp), use, e.scrutinee->span);
    scrut = Place::of(tmp);
    c.scopes.back().push_back(tmp);
  }
  uint32_t join = new_block(c);
  uint32_t next_arm = new_block(c);
  goto_block(c, next_arm);
  for (size_t i = 0; i < e.arms.size(); ++i) {
    auto& arm = e.arms[i];
    c.current = next_arm;
    uint32_t fail = new_block(c);
    next_arm = fail;
    push_scope(c);
    lower_pattern_test(c, *arm.pattern, scrut, st, fail);
    lower_pattern_bind(c, *arm.pattern, scrut, st, 0);
    if (arm.guard) {
      Operand g = lower_operand(c, *arm.guard);
      uint32_t ok = new_block(c);
      Terminator sw;
      sw.kind = Terminator::Kind::Switch;
      sw.discr = g;
      sw.cases = {{1, ok}};
      sw.otherwise = fail;
      sw.span = arm.guard->span;
      set_term(c, sw);
      c.current = ok;
    }
    const Type* bt = type_of(c, *arm.body);
    if (dest && !bt->is_void() && !bt->is_never()) lower_into(c, *arm.body, *dest);
    else { Operand o = lower_operand(c, *arm.body); (void)o; }
    if (!block_terminated(c)) { pop_scope(c, arm.span); goto_block(c, join); } else c.scopes.pop_back();
  }
  // No arm matched: the checker proved exhaustiveness, so this is unreachable.
  c.current = next_arm;
  Terminator u;
  u.kind = Terminator::Kind::Unreachable;
  u.span = e.span;
  set_term(c, u);
  c.current = join;
}

Operand Lowerer::lower_try(FnCtx& c, const ast::TryExpr& e) {
  const Type* ot = type_of(c, *e.operand);
  const Type* t = type_of(c, e);
  Operand v = lower_operand(c, *e.operand);
  uint32_t vl = new_temp(c, ot, e.operand->span);
  Rvalue use; use.kind = RvalueKind::Use; use.a = v;
  assign(c, Place::of(vl), use, e.operand->span);
  bool is_result = ot->enum_def == prog_.result_def;
  const sema::VariantDef* good = ot->enum_def->find_variant(is_result ? "Ok" : "Some");
  const sema::VariantDef* bad = ot->enum_def->find_variant(is_result ? "Err" : "None");
  uint32_t disc = new_temp(c, ctx_.usize_type(), e.span);
  Rvalue d; d.kind = RvalueKind::Discriminant; d.place = Place::of(vl); d.type = ctx_.usize_type();
  assign(c, Place::of(disc), d, e.span);
  uint32_t good_b = new_block(c), bad_b = new_block(c);
  Terminator sw;
  sw.kind = Terminator::Kind::Switch;
  sw.discr = Operand::copy(Place::of(disc));
  sw.cases = {{good->index, good_b}};
  sw.otherwise = bad_b;
  sw.span = e.span;
  set_term(c, sw);

  // Error path: build the function's return value and return.
  c.current = bad_b;
  const Type* ret_t = c.fn->ret;
  Rvalue agg;
  agg.kind = RvalueKind::Aggregate;
  agg.agg = AggregateKind::Variant;
  agg.type = ret_t;
  const sema::VariantDef* ret_bad = ret_t->enum_def->find_variant(is_result ? "Err" : "None");
  agg.variant = ret_bad->index;
  if (is_result) {
    agg.elems.push_back(operand_from_place(c, Place::of(vl).downcast(bad->index).field(0), ot->elems[1]));
  }
  assign(c, Place::of(0), agg, e.span);
  emit_drops_to(c, 0, e.span);
  Terminator g;
  g.kind = Terminator::Kind::Goto;
  g.target = c.return_block;
  g.span = e.span;
  set_term(c, g);

  c.current = good_b;
  uint32_t out = new_temp(c, t, e.span);
  Rvalue take; take.kind = RvalueKind::Use;
  take.a = operand_from_place(c, Place::of(vl).downcast(good->index).field(0), t);
  assign(c, Place::of(out), take, e.span);
  return operand_from_place(c, Place::of(out), t);
}

// ---------- interpolation ----------

void Lowerer::append_display(FnCtx& c, Place string_place, Operand value, const Type* t, Span span) {
  const Type* base = t;
  Operand v = value;
  while (base->kind == TypeKind::Ref) {
    if (v.kind != Operand::Kind::Const) {
      v.place = v.place.deref();
      v.kind = Operand::Kind::Copy;
    }
    base = base->inner;
  }
  // The destination String, by writable reference.
  uint32_t sref = new_temp(c, ctx_.ref(c.fn->locals[string_place.local].type, false), span);
  Rvalue rv; rv.kind = RvalueKind::Ref; rv.place = string_place; rv.is_const = false; rv.type = c.fn->locals[sref].type;
  assign(c, Place::of(sref), rv, span);
  Operand dst = Operand::copy(Place::of(sref));

  Callee callee;
  callee.kind = Callee::Kind::Builtin;
  if (base->kind == TypeKind::Str) {
    callee.builtin = "string_push_str";
    // A constant is already a &const str. A place of type str behind a
    // reference is re-borrowed as &const str.
    if (t->kind == TypeKind::Ref && v.kind != Operand::Kind::Const) {
      uint32_t r = new_temp(c, ctx_.ref(ctx_.str(), true), span);
      Rvalue rr; rr.kind = RvalueKind::Ref; rr.place = v.place; rr.is_const = true; rr.type = c.fn->locals[r].type;
      assign(c, Place::of(r), rr, span);
      v = Operand::copy(Place::of(r));
    }
    call(c, callee, {dst, v}, Place::none(), false, span);
    return;
  }
  if (base->kind == TypeKind::Struct && base->struct_def == prog_.string_def) {
    // Borrow the String, convert to str, push.
    uint32_t r = new_temp(c, ctx_.ref(base, true), span);
    Rvalue rr; rr.kind = RvalueKind::Ref; rr.place = v.place; rr.is_const = true; rr.type = c.fn->locals[r].type;
    assign(c, Place::of(r), rr, span);
    uint32_t s = new_temp(c, ctx_.ref(ctx_.str(), true), span);
    Rvalue conv; conv.kind = RvalueKind::StrFromString; conv.a = Operand::copy(Place::of(r)); conv.type = c.fn->locals[s].type;
    assign(c, Place::of(s), conv, span);
    callee.builtin = "string_push_str";
    call(c, callee, {dst, Operand::copy(Place::of(s))}, Place::none(), false, span);
    return;
  }
  if (base->kind == TypeKind::Prim) {
    switch (base->prim) {
      case ast::PrimKind::Bool: callee.builtin = "fmt_bool"; break;
      case ast::PrimKind::Char: callee.builtin = "fmt_char"; break;
      case ast::PrimKind::Float: case ast::PrimKind::Double: callee.builtin = "fmt_double"; break;
      case ast::PrimKind::Byte: case ast::PrimKind::Uint: case ast::PrimKind::Ulong: case ast::PrimKind::Usize:
        callee.builtin = "fmt_uint"; break;
      default: callee.builtin = "fmt_int"; break;
    }
    // Widen to the runtime's argument type.
    const Type* wide = base->is_float() ? ctx_.double_type()
                     : callee.builtin == "fmt_uint" ? ctx_.prim(ast::PrimKind::Ulong)
                     : callee.builtin == "fmt_int" ? ctx_.prim(ast::PrimKind::Long) : base;
    if (wide != base) {
      uint32_t w = new_temp(c, wide, span);
      Rvalue cast; cast.kind = RvalueKind::Cast; cast.a = v; cast.type = wide;
      assign(c, Place::of(w), cast, span);
      v = Operand::copy(Place::of(w));
    }
    call(c, callee, {dst, v}, Place::none(), false, span);
    return;
  }
  // User type with Display: call to_string, push, drop.
  sema::TraitDef* display = prog_.core_trait("Display");
  sema::FunctionDef* tm = display ? display->find_method("to_string") : nullptr;
  Function* target = tm ? resolve_trait_method(*tm, base, {}, span) : nullptr;
  if (!target) { error(span, "no Display implementation for '" + base->to_string() + "'"); return; }
  uint32_t r = new_temp(c, ctx_.ref(base, true), span);
  Place vp = v.place;
  if (v.kind == Operand::Kind::Const) { error(span, "cannot format a constant of this type"); return; }
  Rvalue rr; rr.kind = RvalueKind::Ref; rr.place = vp; rr.is_const = true; rr.type = c.fn->locals[r].type;
  assign(c, Place::of(r), rr, span);
  const Type* string_t = ctx_.struct_type(prog_.string_def, {});
  uint32_t s = new_temp(c, string_t, span);
  Callee direct; direct.kind = Callee::Kind::Direct; direct.fn = target;
  call(c, direct, {Operand::copy(Place::of(r))}, Place::of(s), false, span);
  uint32_t sr = new_temp(c, ctx_.ref(string_t, true), span);
  Rvalue r2; r2.kind = RvalueKind::Ref; r2.place = Place::of(s); r2.is_const = true; r2.type = c.fn->locals[sr].type;
  assign(c, Place::of(sr), r2, span);
  uint32_t ss = new_temp(c, ctx_.ref(ctx_.str(), true), span);
  Rvalue conv; conv.kind = RvalueKind::StrFromString; conv.a = Operand::copy(Place::of(sr)); conv.type = c.fn->locals[ss].type;
  assign(c, Place::of(ss), conv, span);
  callee.builtin = "string_push_str";
  call(c, callee, {dst, Operand::copy(Place::of(ss))}, Place::none(), false, span);
  Statement drop; drop.kind = Statement::Kind::Drop; drop.place = Place::of(s); drop.span = span;
  emit(c, drop);
}

void Lowerer::lower_interp(FnCtx& c, const ast::InterpExpr& e, Place dest) {
  // dest = string_new(); then append each part.
  Callee nw; nw.kind = Callee::Kind::Builtin; nw.builtin = "string_new";
  call(c, nw, {}, dest, false, e.span);
  for (auto& part : e.parts) {
    if (!part.expr) {
      if (part.text.empty()) continue;
      Constant k; k.kind = Constant::Kind::Str; k.str_value = part.text; k.type = ctx_.ref(ctx_.str(), true);
      append_display(c, dest, Operand::constant_of(k), k.type, e.span);
    } else {
      const Type* t = type_of(c, *part.expr);
      Operand v = lower_operand(c, *part.expr);
      if (v.kind == Operand::Kind::Const && t->kind != TypeKind::Ref) {
        uint32_t tmp = new_temp(c, t, part.expr->span);
        Rvalue use; use.kind = RvalueKind::Use; use.a = v;
        assign(c, Place::of(tmp), use, part.expr->span);
        v = Operand::copy(Place::of(tmp));
      }
      append_display(c, dest, v, t, part.expr->span);
      // Drop a temporary produced for the part. A named local, or the
      // unnamed local that holds a capture of a once closure, is only
      // borrowed by the display call and stays alive.
      bool is_temp = v.kind == Operand::Kind::Move && v.place.is_local() && c.fn->locals[v.place.local].name.empty() &&
                     !c.fn->locals[v.place.local].escape_boundary;
      if (is_temp && needs_drop(t)) {
        Statement d; d.kind = Statement::Kind::Drop; d.place = v.place; d.span = part.expr->span;
        emit(c, d);
      }
    }
  }
}

// ---------- calls ----------

Function* Lowerer::resolve_trait_method(const sema::FunctionDef& trait_method, const Type* self_type,
                                        std::vector<const Type*> fn_args, Span span,
                                        const std::vector<const Type*>* trait_args) {
  sema::TraitDef* trait = trait_method.owner_trait;
  // Find the impl for this type.
  for (auto& impl : prog_.impls) {
    if (impl->trait.trait != trait) continue;
    // Bind impl parameters from both Self and the requested trait arguments.
    sema::Subst s;
    std::vector<const Type*> owner_args;
    std::vector<std::pair<const Type*, const Type*>> work{{impl->self_type, self_type}};
    if (trait_args) {
      if (trait_args->size() != impl->trait.args.size()) continue;
      for (size_t i = 0; i < trait_args->size(); ++i) work.push_back({impl->trait.args[i], (*trait_args)[i]});
    }
    bool matched = true;
    std::map<const Type*, const Type*> binding;
    while (!work.empty() && matched) {
      auto [pat, con] = work.back();
      work.pop_back();
      if (pat == con) continue;
      if (pat->kind == TypeKind::Param && pat->owner == impl.get()) {
        auto it = binding.find(pat);
        if (it == binding.end()) binding[pat] = con;
        else if (it->second != con) matched = false;
        continue;
      }
      if (pat->kind != con->kind || pat->is_const != con->is_const || pat->length != con->length ||
          pat->struct_def != con->struct_def || pat->enum_def != con->enum_def || pat->trait_def != con->trait_def ||
          (pat->kind == TypeKind::Prim && pat->prim != con->prim) || pat->kind == TypeKind::Param ||
          pat->elems.size() != con->elems.size()) { matched = false; break; }
      if (pat->inner) work.push_back({pat->inner, con->inner});
      if (pat->ret) work.push_back({pat->ret, con->ret});
      for (size_t i = 0; i < pat->elems.size(); ++i) work.push_back({pat->elems[i], con->elems[i]});
    }
    for (auto& g : impl->generics) {
      auto it = binding.find(g.type);
      if (it == binding.end()) { matched = false; break; }
      if (it->second->is_unsized()) { matched = false; break; }
      owner_args.push_back(it->second);
      s.bind(g.type, it->second);
    }
    if (!matched) continue;
    sema::FunctionDef* m = impl->find_method(trait_method.name);
    if (m) return instance(*m, owner_args, fn_args);
    // Default body on the trait.
    if (trait_method.ast && trait_method.ast->body) {
      std::vector<const Type*> owner{self_type};
      for (auto* a : impl->trait.args) owner.push_back(s.apply(ctx_, a));
      return instance(trait_method, owner, fn_args);
    }
  }
  error(span, "no implementation of '" + trait->name + "." + trait_method.name + "' for '" + self_type->to_string() + "'");
  return nullptr;
}

Vtable* Lowerer::vtable_for(const Type* concrete, const Type* dyn_type, Span span) {
  for (auto& table : module_->vtables)
    if (table->concrete == concrete && table->dyn_type == dyn_type) return table.get();
  auto table = std::make_unique<Vtable>();
  table->name = "vtable." + std::to_string(module_->vtables.size());
  table->concrete = concrete;
  table->dyn_type = dyn_type;
  for (auto* method : dyn_type->trait_def->methods) {
    auto* target = resolve_trait_method(*method, concrete, {}, span, &dyn_type->elems);
    if (!target) return nullptr;
    // A mismatched impl signature would otherwise corrupt the call frame.
    sema::Subst expected;
    expected.bind(dyn_type->trait_def->self_type, concrete);
    for (size_t i = 0; i < dyn_type->elems.size(); ++i)
      expected.bind(dyn_type->trait_def->generics[i].type, dyn_type->elems[i]);
    sema::Subst actual;
    if (target->def->owner == sema::FnOwner::Impl) {
      auto* impl = target->def->owner_impl;
      for (size_t i = 0; i < impl->generics.size(); ++i) actual.bind(impl->generics[i].type, target->owner_args[i]);
    } else actual = expected;
    bool same = target->def->params.size() == method->params.size() && target->def->generics.empty() &&
                actual.apply(ctx_, target->def->ret) == expected.apply(ctx_, method->ret);
    for (size_t i = 0; same && i < method->params.size(); ++i)
      same = actual.apply(ctx_, target->def->params[i].type) == expected.apply(ctx_, method->params[i].type);
    if (!same) { error(span, "implementation of '" + method->name + "' does not match its trait signature"); return nullptr; }
    table->methods.push_back(target);
  }
  // Register any destructor required by the concrete type. Trait objects
  // borrow their data; this entry is metadata for later owning objects.
  needs_drop(concrete);
  auto* out = table.get();
  module_->vtables.push_back(std::move(table));
  return out;
}

Function* Lowerer::resolve_call_target(FnCtx& c, const NameRef& ref, const Type* receiver_base) {
  std::vector<const Type*> owner, args;
  for (auto* a : ref.owner_args) owner.push_back(subst_type(c, a));
  for (auto* a : ref.type_args) args.push_back(subst_type(c, a));
  if (ref.fn->owner == sema::FnOwner::Trait) {
    // Through a generic bound or a default method: find the impl now.
    return resolve_trait_method(*ref.fn, receiver_base, args, Span{}, &owner);
  }
  return instance(*ref.fn, owner, args);
}

Operand Lowerer::lower_method_call(FnCtx& c, const ast::CallExpr& e, const ast::FieldExpr& callee,
                                   const NameRef& ref, Place* dest) {
  const Type* t = type_of(c, e);
  const Type* recv_t = type_of(c, *callee.base);
  const Type* base = recv_t;
  for (int i = 0; i < ref.auto_derefs && base->kind == TypeKind::Ref; ++i) base = base->inner;

  // Builtin str methods.
  if (ref.fn->is_builtin) {
    Operand recv = lower_operand(c, *callee.base);
    // Re-borrow to &const str.
    if (recv.kind != Operand::Kind::Const) {
      Place p = recv.place;
      const Type* rt = recv_t;
      while (rt->kind == TypeKind::Ref) { p = p.deref(); rt = rt->inner; }
      uint32_t r = new_temp(c, ctx_.ref(ctx_.str(), true), callee.base->span);
      Rvalue rr; rr.kind = RvalueKind::Ref; rr.place = p; rr.is_const = true; rr.type = c.fn->locals[r].type;
      assign(c, Place::of(r), rr, callee.base->span);
      recv = Operand::copy(Place::of(r));
    }
    std::vector<Operand> args{recv};
    for (auto& a : e.args) args.push_back(lower_operand(c, *a));
    Callee cl; cl.kind = Callee::Kind::Builtin; cl.builtin = "str_" + ref.fn->name;
    Place out = dest ? *dest : Place::of(new_temp(c, t, e.span));
    call(c, cl, std::move(args), out, false, e.span);
    return operand_from_place(c, out, t);
  }

  bool virtual_call = base->kind == TypeKind::Dyn;
  Function* target = virtual_call ? nullptr : resolve_call_target(c, ref, base);
  if (!virtual_call && !target) return Operand();

  // Arguments are evaluated before the receiver is borrowed, so that
  // v.push(v.len()) does not conflict with itself.
  std::vector<Operand> arg_values;
  for (auto& a : e.args) arg_values.push_back(lower_operand(c, *a));

  // Receiver: by value, or borrowed as the method asks.
  ast::SelfKind kind = ref.fn->params[0].self_kind;
  std::vector<Operand> args;
  Operand recv;
  auto coercion = checked_.coercions.find(callee.base.get());
  bool auto_borrow = coercion != checked_.coercions.end() &&
                     (coercion->second.kind == CoercionKind::AutoBorrowConst || coercion->second.kind == CoercionKind::AutoBorrowMut);
  if (kind == ast::SelfKind::Value) {
    Place p;
    if (lower_place(c, *callee.base, p)) {
      for (int i = 0; i < ref.auto_derefs; ++i) p = p.deref();
      recv = operand_from_place(c, p, base);
    } else {
      recv = lower_operand(c, *callee.base);
      // A reference produced by a call, such as f().unwrap(), is read
      // through like a reference held in a place.
      if (ref.auto_derefs > 0 && recv.kind != Operand::Kind::Const) {
        Place q = recv.place;
        for (int i = 0; i < ref.auto_derefs; ++i) q = q.deref();
        recv = operand_from_place(c, q, base);
      }
    }
  } else {
    bool want_const = kind == ast::SelfKind::ConstRef;
    Place p;
    bool have_place = lower_place(c, *callee.base, p);
    if (!have_place) {
      Operand o = lower_operand(c, *callee.base);
      if (auto_borrow) {
        // lower_operand already applied the receiver's borrow coercion.
        assert(o.kind != Operand::Kind::Const && o.place.is_local());
        const Type* borrowed = c.fn->locals[o.place.local].type;
        assert(borrowed->kind == TypeKind::Ref && borrowed->is_const == want_const && borrowed->inner == base);
        (void)borrowed;
        args.push_back(o);
        goto args_done;
      }
      if (o.kind == Operand::Kind::Const) {
        uint32_t tmp = new_temp(c, recv_t, callee.base->span);
        Rvalue use; use.kind = RvalueKind::Use; use.a = o;
        assign(c, Place::of(tmp), use, callee.base->span);
        p = Place::of(tmp);
        c.scopes.back().push_back(tmp);
      } else {
        p = o.place;
      }
    }
    const Type* pt = recv_t;
    if (!auto_borrow && pt->kind == TypeKind::Ref && ref.auto_derefs <= 1) {
      // Already a reference of the right shape: pass it (re-borrow for const).
      if (ref.auto_derefs == 1 && (want_const == pt->is_const)) {
        recv = Operand::copy(p);
        args.push_back(recv);
        goto args_done;
      }
    }
    // Deref down to the base and borrow.
    for (int i = 0; i < ref.auto_derefs && pt->kind == TypeKind::Ref; ++i) { p = p.deref(); pt = pt->inner; }
    {
      uint32_t r = new_temp(c, ctx_.ref(base, want_const), callee.base->span);
      Rvalue rr; rr.kind = RvalueKind::Ref; rr.place = p; rr.is_const = want_const; rr.type = c.fn->locals[r].type;
      assign(c, Place::of(r), rr, callee.base->span);
      recv = Operand::copy(Place::of(r));
    }
  }
  args.push_back(recv);
args_done:
  for (auto& a : arg_values) args.push_back(a);
  bool diverges = t->is_never();
  Place out = dest ? *dest : (t->is_void() || diverges ? Place::none() : Place::of(new_temp(c, t, e.span)));
  Callee cl;
  cl.kind = virtual_call ? Callee::Kind::Virtual : Callee::Kind::Direct;
  cl.fn = target;
  if (virtual_call) {
    auto& methods = base->trait_def->methods;
    auto method = std::find(methods.begin(), methods.end(), ref.fn);
    if (method == methods.end()) { error(e.span, "internal error: virtual method has no vtable slot"); return Operand(); }
    cl.vtable_slot = static_cast<uint32_t>(method - methods.begin());
  }
  call(c, cl, std::move(args), out, diverges, e.span);
  if (t->is_void() || diverges) { Constant k; k.kind = Constant::Kind::Unit; k.type = t; return Operand::constant_of(k); }
  return operand_from_place(c, out, t);
}

Operand Lowerer::lower_call(FnCtx& c, const ast::CallExpr& e, Place* dest) {
  const Type* t = type_of(c, e);
  const ast::Expr& callee = *e.callee;

  // Method call recorded on the call expression itself.
  auto cit = checked_.refs.find(&e);
  if (cit != checked_.refs.end() && cit->second.kind == RefKind::Method) {
    if (callee.kind == ExprKind::OptChain) { error(e.span, "'?.' calls are not supported in code generation yet"); return Operand(); }
    return lower_method_call(c, e, static_cast<const ast::FieldExpr&>(callee), cit->second, dest);
  }

  auto it = checked_.refs.find(&callee);
  const NameRef* ref = it != checked_.refs.end() ? &it->second : nullptr;
  Place out = dest ? *dest : Place::none();

  if (ref && ref->kind == RefKind::Variant) {
    // Variant constructor.
    Rvalue agg;
    agg.kind = RvalueKind::Aggregate;
    agg.agg = AggregateKind::Variant;
    agg.variant = ref->variant->index;
    agg.type = t;
    for (auto& a : e.args) agg.elems.push_back(lower_operand(c, *a));
    if (!dest) out = Place::of(new_temp(c, t, e.span));
    assign(c, out, agg, e.span);
    return operand_from_place(c, out, t);
  }

  std::vector<Operand> args;
  Callee cl;
  bool diverges = false;
  Place callee_temp = Place::none();
  Place callee_place = Place::none();
  size_t callee_scope_depth = c.scopes.size();
  if (ref && (ref->kind == RefKind::Function || ref->kind == RefKind::AssocFn)) {
    if (ref->fn->is_builtin) {
      cl.kind = Callee::Kind::Builtin;
      cl.builtin = ref->fn->name;
      for (auto* a : ref->type_args) cl.type_args.push_back(subst_type(c, a));
      if (cl.builtin == "drop_in_place" && !cl.type_args.empty()) needs_drop(cl.type_args[0]);
      diverges = ref->fn->ret && ref->fn->ret->is_never();
    } else if (ref->fn->owner == sema::FnOwner::Extern) {
      cl.kind = Callee::Kind::Builtin;
      cl.builtin = ref->fn->name;
      diverges = ref->fn->ret && ref->fn->ret->is_never();
    } else {
      Function* target = resolve_call_target(c, *ref, nullptr);
      if (!target) return Operand();
      cl.kind = Callee::Kind::Direct;
      cl.fn = target;
      diverges = target->def->ret && target->def->ret->is_never();
    }
    // The callee expression itself produces nothing; skip lowering it.
  } else {
    // Indirect call through a function value.
    cl.kind = Callee::Kind::Indirect;
    Place p;
    // lower_place scopes a temporary owner to the block; this call drops it
    // right after the call instead, so reclaim it from the scope.
    const size_t scoped_before = c.scopes.back().size();
    bool placed = lower_place(c, callee, p);
    if (placed && c.scopes.back().size() > scoped_before) {
      auto& scope = c.scopes.back();
      auto found = std::find(scope.begin() + static_cast<std::ptrdiff_t>(scoped_before), scope.end(), p.local);
      if (found != scope.end()) scope.erase(found);
    }
    if (!placed) {
      Operand value = lower_operand(c, callee);
      p = Place::of(new_temp(c, type_of(c, callee), callee.span));
      Rvalue use;
      use.a = value;
      assign(c, p, use, callee.span);
      callee_temp = p;
    } else if (c.fn->locals[p.local].name.empty()) {
      // A function field can belong to an aggregate returned as a temporary.
      // Keep and drop that whole owner, including its other fields.
      bool scoped = false;
      for (auto& scope : c.scopes) {
        if (std::find(scope.begin(), scope.end(), p.local) != scope.end()) scoped = true;
      }
      if (!scoped) callee_temp = Place::of(p.local);
    }
    if (!callee_temp.is_none()) {
      // Arguments can return, break, or use ?. Such paths must drop the
      // already evaluated callee even when they never reach the call.
      c.scopes.back().push_back(callee_temp.local);
    }
    callee_place = p;
    auto* callable = type_of(c, callee);
    while (callable->kind == TypeKind::Ref) { callee_place = callee_place.deref(); callable = callable->inner; }
    diverges = callable->ret->is_never();
  }
  for (auto& a : e.args) args.push_back(lower_operand(c, *a));
  if (cl.kind == Callee::Kind::Indirect) {
    auto* callable = type_of(c, callee);
    while (callable->kind == TypeKind::Ref) callable = callable->inner;
    if (callable->call_mode == ast::CallMode::Once) {
      cl.fn_operand = Operand::move(callee_place);
    } else {
      const Type* ref_type = ctx_.ref(callable, callable->call_mode == ast::CallMode::Shared);
      uint32_t borrowed = new_temp(c, ref_type, callee.span);
      Rvalue borrow;
      borrow.kind = RvalueKind::Ref;
      borrow.place = callee_place;
      borrow.type = ref_type;
      borrow.is_const = ref_type->is_const;
      assign(c, Place::of(borrowed), borrow, callee.span);
      cl.fn_operand = Operand::copy(Place::of(borrowed));
    }
  }

  if (!dest && !t->is_void() && !diverges) out = Place::of(new_temp(c, t, e.span));
  call(c, cl, std::move(args), out, diverges, e.span);
  auto* callable_type = type_of(c, callee);
  while (callable_type->kind == TypeKind::Ref) callable_type = callable_type->inner;
  if (!callee_temp.is_none() && !diverges && callable_type->call_mode != ast::CallMode::Once) {
    Statement drop;
    drop.kind = Statement::Kind::Drop;
    drop.place = callee_temp;
    drop.span = e.span;
    emit(c, drop);
  }
  if (!callee_temp.is_none()) {
    assert(c.scopes.size() == callee_scope_depth);
    auto& scope = c.scopes[callee_scope_depth - 1];
    auto scope_it = std::find(scope.begin(), scope.end(), callee_temp.local);
    if (scope_it != scope.end()) scope.erase(scope_it);
  }
  if (t->is_void() || diverges) { Constant k; k.kind = Constant::Kind::Unit; k.type = t; return Operand::constant_of(k); }
  return operand_from_place(c, out, t);
}

}  // namespace synx::ir
