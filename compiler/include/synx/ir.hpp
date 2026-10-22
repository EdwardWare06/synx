// Dendrite: the Synx intermediate representation.
//
// A function is a list of typed locals and a graph of basic blocks. Every
// block ends in one terminator. Values live in places (a local plus a chain
// of projections) and move between them through operands. Generics are gone
// by this point: every function here is one concrete instantiation.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

#include "synx/ast.hpp"
#include "synx/defs.hpp"
#include "synx/source.hpp"
#include "synx/types.hpp"

namespace synx::ir {

using sema::Type;

struct Function;
struct Vtable;

// ---------- places ----------

enum class ProjKind {
  Field,     // struct field, tuple element, or variant payload field (after Downcast)
  Index,     // element of an array or slice, index held in a local
  Deref,     // through a reference or raw pointer
  Downcast   // view an enum as one of its variants
};

struct Projection {
  ProjKind kind;
  uint32_t index = 0;  // Field: field number, Downcast: variant number, Index: local holding the index
};

struct Place {
  uint32_t local = 0;
  std::vector<Projection> proj;

  static Place of(uint32_t local) { return Place{local, {}}; }
  // A call with no result writes nowhere.
  static Place none() { return Place{UINT32_MAX, {}}; }
  bool is_none() const { return local == UINT32_MAX; }
  Place field(uint32_t i) const { Place p = *this; p.proj.push_back({ProjKind::Field, i}); return p; }
  Place index(uint32_t index_local) const { Place p = *this; p.proj.push_back({ProjKind::Index, index_local}); return p; }
  Place deref() const { Place p = *this; p.proj.push_back({ProjKind::Deref, 0}); return p; }
  Place downcast(uint32_t variant) const { Place p = *this; p.proj.push_back({ProjKind::Downcast, variant}); return p; }
  bool is_local() const { return proj.empty(); }
};

// ---------- operands ----------

struct Constant {
  enum class Kind { Int, Float, Bool, Str, Unit, Fn, Char };
  Kind kind = Kind::Unit;
  uint64_t int_value = 0;
  double float_value = 0.0;
  std::string str_value;
  Function* fn = nullptr;
  const Type* type = nullptr;
};

struct Operand {
  enum class Kind { Copy, Move, Const };
  Kind kind = Kind::Const;
  Place place;
  Constant constant;

  static Operand copy(Place p) { Operand o; o.kind = Kind::Copy; o.place = std::move(p); return o; }
  static Operand move(Place p) { Operand o; o.kind = Kind::Move; o.place = std::move(p); return o; }
  static Operand constant_of(Constant c) { Operand o; o.kind = Kind::Const; o.constant = std::move(c); return o; }
};

// ---------- rvalues ----------

enum class RvalueKind {
  Use,            // a
  Ref,            // &place or &const place
  Binary,         // a op b
  Unary,          // op a
  Cast,           // a as type
  Aggregate,      // struct, tuple, array, or enum variant from elems
  Discriminant,   // variant number of the enum in place
  Len,            // length of the array or slice in place
  StrFromString,  // &const String -> &const str
  ArrayToSlice,   // &T[N] -> &T[]
  VecToSlice,     // &Vec<T> -> &T[]
  ToDyn          // &T -> &dyn Trait, preserving the borrow
};

enum class AggregateKind { Struct, Tuple, Array, Variant };

struct Rvalue {
  RvalueKind kind = RvalueKind::Use;
  Operand a, b;
  Place place;                  // Ref, Discriminant, Len
  bool is_const = false;        // Ref
  ast::BinaryOp bop = ast::BinaryOp::Add;
  ast::UnaryOp uop = ast::UnaryOp::Neg;
  const Type* type = nullptr;   // Cast target, Aggregate type, coercion result
  AggregateKind agg = AggregateKind::Struct;
  uint32_t variant = 0;         // Aggregate Variant
  std::vector<Operand> elems;   // Aggregate
  // Closure ownership transfers and loans. Payload bytes were stored in
  // the environment; these operands describe the completed capture.
  std::vector<Operand> captures;
  Vtable* vtable = nullptr;     // ToDyn
};

// ---------- statements and terminators ----------

struct Statement {
  enum class Kind { Assign, Drop, DropIf, StorageDead, Nop };
  Kind kind = Kind::Nop;
  Place place;
  Rvalue rvalue;
  uint32_t flag_local = 0;  // DropIf: drop only when this bool local is true
  Span span;
};

struct Callee {
  enum class Kind { Direct, Builtin, Indirect, Virtual };
  Kind kind = Kind::Direct;
  Function* fn = nullptr;       // Direct
  std::string builtin;          // Builtin: runtime function name
  std::vector<const Type*> type_args;  // Builtin: generic arguments (size_of<T>)
  Operand fn_operand;           // Indirect: a value of fn type
  uint32_t vtable_slot = 0;      // Virtual: method index; args[0] is &dyn Trait
};

struct Terminator {
  enum class Kind { Goto, Switch, Call, Assert, Return, Unreachable };
  Kind kind = Kind::Unreachable;
  uint32_t target = 0;                               // Goto; Call and Assert continuation
  Operand discr;                                     // Switch
  std::vector<std::pair<uint64_t, uint32_t>> cases;  // Switch: value -> block
  uint32_t otherwise = 0;                            // Switch
  Callee callee;                                     // Call
  std::vector<Operand> args;                         // Call
  Place dest;                                        // Call result
  bool diverges = false;                             // Call: callee never returns
  Operand cond;                                      // Assert: must be true
  std::string message;                               // Assert: panic text
  Span span;
};

struct BasicBlock {
  std::vector<Statement> stmts;
  Terminator term;
};

struct Local {
  const Type* type = nullptr;
  std::string name;   // user name, or empty for a temporary
  bool is_arg = false;
  Span span;
  bool escape_boundary = false;  // owned capture may contain external writable references
};

// One concrete function. locals[0] is the return slot, then the arguments.
struct Function {
  std::string name;
  const sema::FunctionDef* def = nullptr;
  std::vector<const Type*> owner_args;
  std::vector<const Type*> type_args;
  const Type* ret = nullptr;
  uint32_t arg_count = 0;
  std::vector<Local> locals;
  std::vector<BasicBlock> blocks;
  bool lowered = false;
  // Closure entry receives locals[1] in R10; other arguments use the stack.
  const ast::ClosureExpr* closure = nullptr;
  const Type* closure_type = nullptr;
  const Type* environment_type = nullptr;
  // Async bodies are borrow checked with ordinary locals, then transformed
  // into poll functions over an immovable heap frame.
  bool async_body = false;
  Function* async_poll = nullptr;    // constructor -> logical body
  Function* async_cancel = nullptr;
  uint32_t async_waker = UINT32_MAX;
  std::map<uint32_t, uint32_t> async_suspends;  // marker block -> resume block
  std::set<uint32_t> async_drop_locals;
  std::map<uint32_t, uint32_t> async_drop_flags;
  std::map<uint32_t, std::set<uint32_t>> async_live;
  std::map<uint32_t, std::set<std::pair<uint32_t, uint32_t>>> async_borrows;
};

struct Vtable {
  std::string name;
  const Type* concrete = nullptr;
  const Type* dyn_type = nullptr;
  std::vector<Function*> methods;
};

struct Module {
  std::vector<std::unique_ptr<Function>> functions;
  Function* entry = nullptr;  // main
  // User Drop implementations, instantiated for every type that is dropped.
  std::map<const Type*, Function*> drop_impls;
  // Complete cleanup requirements, including children of recursive owners.
  std::set<const Type*> drop_types;
  std::vector<std::unique_ptr<Vtable>> vtables;
};

std::string dump(const Function& f);
std::string dump(const Module& m);

}  // namespace synx::ir
