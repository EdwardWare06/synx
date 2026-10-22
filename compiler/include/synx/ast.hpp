// Abstract syntax tree for Synx. Produced by the parser, consumed by name
// resolution and the type checker. Every node carries the span of the source
// it came from.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "synx/source.hpp"

namespace synx::ast {

struct Type;
struct Expr;
struct Stmt;
struct Pattern;
struct Item;
struct Block;
struct Function;

using TypePtr = std::unique_ptr<Type>;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;
using PatternPtr = std::unique_ptr<Pattern>;
using ItemPtr = std::unique_ptr<Item>;
using BlockPtr = std::unique_ptr<Block>;
using FunctionPtr = std::unique_ptr<Function>;

// ---------- shared pieces ----------
enum class CallMode { Writable, Shared, Once };
enum class CaptureMode { Move, Shared, Writable };

struct PathSegment {
  std::string name;
  Span span;
};

// A dotted name: a, a.b, a.b.c. Resolution decides what each segment is.
struct Path {
  std::vector<PathSegment> segments;
  Span span;
  std::string to_string() const;
};

// A trait bound such as Shape or Iterator<int>.
struct Bound {
  Path path;
  std::vector<TypePtr> args;
  Span span;
};

struct GenericParam {
  std::string name;
  Span span;
  std::vector<Bound> bounds;
};

enum class PrimKind {
  Bool, Byte, Short, Int, Long, Uint, Ulong, Usize, Float, Double, Char, Void
};
const char* prim_kind_name(PrimKind k);

// ---------- types ----------

enum class TypeKind {
  Primitive, Path, Ref, Ptr, Array, Slice, Optional, Tuple, Fn, Dyn, SelfType
};

struct Type {
  TypeKind kind;
  Span span;
  Type(TypeKind k, Span s) : kind(k), span(s) {}
  virtual ~Type() = default;
};

struct PrimitiveType : Type {
  PrimKind prim;
  PrimitiveType(Span s, PrimKind p) : Type(TypeKind::Primitive, s), prim(p) {}
};

struct PathType : Type {
  Path path;
  std::vector<TypePtr> args;
  explicit PathType(Span s) : Type(TypeKind::Path, s) {}
};

struct RefType : Type {
  bool is_const;
  TypePtr inner;
  RefType(Span s, bool c, TypePtr i) : Type(TypeKind::Ref, s), is_const(c), inner(std::move(i)) {}
};

struct PtrType : Type {
  bool is_const;
  TypePtr inner;
  PtrType(Span s, bool c, TypePtr i) : Type(TypeKind::Ptr, s), is_const(c), inner(std::move(i)) {}
};

struct ArrayType : Type {
  TypePtr elem;
  ExprPtr size;
  ArrayType(Span s, TypePtr e, ExprPtr n) : Type(TypeKind::Array, s), elem(std::move(e)), size(std::move(n)) {}
};

struct SliceType : Type {
  TypePtr elem;
  SliceType(Span s, TypePtr e) : Type(TypeKind::Slice, s), elem(std::move(e)) {}
};

struct OptionalType : Type {
  TypePtr inner;
  OptionalType(Span s, TypePtr i) : Type(TypeKind::Optional, s), inner(std::move(i)) {}
};

struct TupleType : Type {
  std::vector<TypePtr> elems;  // empty means the unit type ()
  explicit TupleType(Span s) : Type(TypeKind::Tuple, s) {}
};

struct FnType : Type {
  CallMode call_mode = CallMode::Writable;
  bool is_send = false;
  std::vector<TypePtr> params;
  TypePtr ret;  // null means void
  explicit FnType(Span s) : Type(TypeKind::Fn, s) {}
};

struct DynType : Type {
  Bound bound;
  explicit DynType(Span s) : Type(TypeKind::Dyn, s) {}
};

struct SelfTypeRef : Type {
  explicit SelfTypeRef(Span s) : Type(TypeKind::SelfType, s) {}
};

// ---------- expressions ----------

enum class UnaryOp { Neg, Not, BitNot, Deref };
enum class BinaryOp {
  Add, Sub, Mul, Div, Rem,
  Eq, Ne, Lt, Gt, Le, Ge,
  And, Or,
  BitAnd, BitOr, BitXor, Shl, Shr
};
enum class AssignOp {
  Assign, Add, Sub, Mul, Div, Rem, BitAnd, BitOr, BitXor, Shl, Shr
};
const char* unary_op_name(UnaryOp op);
const char* binary_op_name(BinaryOp op);
const char* assign_op_name(AssignOp op);

enum class ExprKind {
  IntLit, FloatLit, CharLit, StringLit, BoolLit, Interp,
  Name, Field, Call, Index,
  Unary, Binary, Cast, Try, OptChain, Coalesce, Range, Ref,
  StructLit, Tuple, Array, ArrayRepeat,
  Block, If, Match, Closure, Unsafe, Await,
  Return, Break, Continue
};

struct Expr {
  ExprKind kind;
  Span span;
  Expr(ExprKind k, Span s) : kind(k), span(s) {}
  virtual ~Expr() = default;
};

struct IntLitExpr : Expr {
  uint64_t value;
  bool is_long;
  IntLitExpr(Span s, uint64_t v, bool l) : Expr(ExprKind::IntLit, s), value(v), is_long(l) {}
};

struct FloatLitExpr : Expr {
  double value;
  bool is_float;  // f suffix: float, else double
  FloatLitExpr(Span s, double v, bool f) : Expr(ExprKind::FloatLit, s), value(v), is_float(f) {}
};

struct CharLitExpr : Expr {
  uint32_t value;
  CharLitExpr(Span s, uint32_t v) : Expr(ExprKind::CharLit, s), value(v) {}
};

struct StringLitExpr : Expr {
  std::string value;
  StringLitExpr(Span s, std::string v) : Expr(ExprKind::StringLit, s), value(std::move(v)) {}
};

struct BoolLitExpr : Expr {
  bool value;
  BoolLitExpr(Span s, bool v) : Expr(ExprKind::BoolLit, s), value(v) {}
};

// "text {expr} text". Parts alternate freely; a part is either text or expr.
struct InterpPart {
  std::string text;
  ExprPtr expr;  // null for a text part
};
struct InterpExpr : Expr {
  std::vector<InterpPart> parts;
  explicit InterpExpr(Span s) : Expr(ExprKind::Interp, s) {}
};

// A bare identifier, including self and Self. Optional explicit type args.
struct NameExpr : Expr {
  std::string name;
  std::vector<TypePtr> type_args;
  NameExpr(Span s, std::string n) : Expr(ExprKind::Name, s), name(std::move(n)) {}
};

// base.name  Also reaches variants, associated items, and module items.
struct FieldExpr : Expr {
  ExprPtr base;
  std::string name;
  Span name_span;
  std::vector<TypePtr> type_args;
  FieldExpr(Span s, ExprPtr b, std::string n, Span ns)
      : Expr(ExprKind::Field, s), base(std::move(b)), name(std::move(n)), name_span(ns) {}
};

struct CallExpr : Expr {
  ExprPtr callee;
  std::vector<ExprPtr> args;
  CallExpr(Span s, ExprPtr c) : Expr(ExprKind::Call, s), callee(std::move(c)) {}
};

struct IndexExpr : Expr {
  ExprPtr base;
  ExprPtr index;
  IndexExpr(Span s, ExprPtr b, ExprPtr i) : Expr(ExprKind::Index, s), base(std::move(b)), index(std::move(i)) {}
};

struct UnaryExpr : Expr {
  UnaryOp op;
  ExprPtr operand;
  UnaryExpr(Span s, UnaryOp o, ExprPtr e) : Expr(ExprKind::Unary, s), op(o), operand(std::move(e)) {}
};

struct BinaryExpr : Expr {
  BinaryOp op;
  ExprPtr lhs, rhs;
  BinaryExpr(Span s, BinaryOp o, ExprPtr l, ExprPtr r)
      : Expr(ExprKind::Binary, s), op(o), lhs(std::move(l)), rhs(std::move(r)) {}
};

struct CastExpr : Expr {
  ExprPtr operand;
  TypePtr type;
  CastExpr(Span s, ExprPtr e, TypePtr t) : Expr(ExprKind::Cast, s), operand(std::move(e)), type(std::move(t)) {}
};

struct TryExpr : Expr {  // expr?
  ExprPtr operand;
  TryExpr(Span s, ExprPtr e) : Expr(ExprKind::Try, s), operand(std::move(e)) {}
};

struct OptChainExpr : Expr {  // expr?.name
  ExprPtr base;
  std::string name;
  OptChainExpr(Span s, ExprPtr b, std::string n) : Expr(ExprKind::OptChain, s), base(std::move(b)), name(std::move(n)) {}
};

struct CoalesceExpr : Expr {  // expr ?? fallback
  ExprPtr value, fallback;
  CoalesceExpr(Span s, ExprPtr v, ExprPtr f) : Expr(ExprKind::Coalesce, s), value(std::move(v)), fallback(std::move(f)) {}
};

struct RangeExpr : Expr {
  ExprPtr start, end;  // either may be null
  bool inclusive;
  RangeExpr(Span s, ExprPtr a, ExprPtr b, bool inc)
      : Expr(ExprKind::Range, s), start(std::move(a)), end(std::move(b)), inclusive(inc) {}
};

struct RefExpr : Expr {  // &x or &const x
  bool is_const;
  ExprPtr operand;
  RefExpr(Span s, bool c, ExprPtr e) : Expr(ExprKind::Ref, s), is_const(c), operand(std::move(e)) {}
};

struct FieldInit {
  std::string name;
  Span span;
  ExprPtr value;  // null means shorthand: name alone
};
struct StructLitExpr : Expr {
  Path path;
  std::vector<FieldInit> fields;
  explicit StructLitExpr(Span s) : Expr(ExprKind::StructLit, s) {}
};

struct TupleExpr : Expr {
  std::vector<ExprPtr> elems;  // empty is the unit value ()
  explicit TupleExpr(Span s) : Expr(ExprKind::Tuple, s) {}
};

struct ArrayExpr : Expr {
  std::vector<ExprPtr> elems;
  explicit ArrayExpr(Span s) : Expr(ExprKind::Array, s) {}
};

struct ArrayRepeatExpr : Expr {  // [value; count]
  ExprPtr value, count;
  ArrayRepeatExpr(Span s, ExprPtr v, ExprPtr c) : Expr(ExprKind::ArrayRepeat, s), value(std::move(v)), count(std::move(c)) {}
};

struct BlockExpr : Expr {
  BlockPtr block;
  BlockExpr(Span s, BlockPtr b) : Expr(ExprKind::Block, s), block(std::move(b)) {}
};

struct IfExpr : Expr {
  ExprPtr cond;
  BlockPtr then_block;
  ExprPtr else_expr;  // null, a BlockExpr, or another IfExpr
  explicit IfExpr(Span s) : Expr(ExprKind::If, s) {}
};

struct MatchArm {
  PatternPtr pattern;
  ExprPtr guard;  // may be null
  ExprPtr body;
  Span span;
};
struct MatchExpr : Expr {
  ExprPtr scrutinee;
  std::vector<MatchArm> arms;
  explicit MatchExpr(Span s) : Expr(ExprKind::Match, s) {}
};

struct ClosureParam {
  TypePtr type;  // may be null: inferred
  std::string name;
  Span span;
};
struct ClosureExpr : Expr {
  CallMode call_mode = CallMode::Writable;
  bool is_send = false;
  struct Capture { std::string name; CaptureMode mode; Span span; };
  std::vector<Capture> captures;
  bool explicit_captures = false;
  std::vector<ClosureParam> params;
  TypePtr ret;  // may be null
  ExprPtr body;
  explicit ClosureExpr(Span s) : Expr(ExprKind::Closure, s) {}
};

struct UnsafeExpr : Expr {
  BlockPtr block;
  UnsafeExpr(Span s, BlockPtr b) : Expr(ExprKind::Unsafe, s), block(std::move(b)) {}
};

struct AwaitExpr : Expr {
  ExprPtr operand;
  AwaitExpr(Span s, ExprPtr e) : Expr(ExprKind::Await, s), operand(std::move(e)) {}
};

struct ReturnExpr : Expr {
  ExprPtr value;  // may be null
  ReturnExpr(Span s, ExprPtr v) : Expr(ExprKind::Return, s), value(std::move(v)) {}
};

struct BreakExpr : Expr {
  explicit BreakExpr(Span s) : Expr(ExprKind::Break, s) {}
};

struct ContinueExpr : Expr {
  explicit ContinueExpr(Span s) : Expr(ExprKind::Continue, s) {}
};

// ---------- patterns ----------

enum class PatternKind {
  Wildcard, Literal, Binding, Path, TupleStruct, Struct, Tuple, Or, Range
};

struct Pattern {
  PatternKind kind;
  Span span;
  Pattern(PatternKind k, Span s) : kind(k), span(s) {}
  virtual ~Pattern() = default;
};

struct WildcardPattern : Pattern {
  explicit WildcardPattern(Span s) : Pattern(PatternKind::Wildcard, s) {}
};

struct LiteralPattern : Pattern {
  ExprPtr value;
  LiteralPattern(Span s, ExprPtr v) : Pattern(PatternKind::Literal, s), value(std::move(v)) {}
};

// A single identifier. Resolution turns it into a unit variant like None
// when the name resolves to one; otherwise it binds a new variable.
struct BindingPattern : Pattern {
  std::string name;
  BindingPattern(Span s, std::string n) : Pattern(PatternKind::Binding, s), name(std::move(n)) {}
};

struct PathPattern : Pattern {  // Token.Plus
  Path path;
  explicit PathPattern(Span s) : Pattern(PatternKind::Path, s) {}
};

struct TupleStructPattern : Pattern {  // Some(x), Token.Number(n)
  Path path;
  std::vector<PatternPtr> elems;
  explicit TupleStructPattern(Span s) : Pattern(PatternKind::TupleStruct, s) {}
};

struct FieldPattern {
  std::string name;
  Span span;
  PatternPtr pattern;  // null means shorthand: bind the field name
};
struct StructPattern : Pattern {  // Shape.Rect { w, h: height }
  Path path;
  std::vector<FieldPattern> fields;
  explicit StructPattern(Span s) : Pattern(PatternKind::Struct, s) {}
};

struct TuplePattern : Pattern {
  std::vector<PatternPtr> elems;
  explicit TuplePattern(Span s) : Pattern(PatternKind::Tuple, s) {}
};

struct OrPattern : Pattern {
  std::vector<PatternPtr> alts;
  explicit OrPattern(Span s) : Pattern(PatternKind::Or, s) {}
};

struct RangePattern : Pattern {
  ExprPtr lo, hi;
  bool inclusive;
  RangePattern(Span s, ExprPtr l, ExprPtr h, bool inc)
      : Pattern(PatternKind::Range, s), lo(std::move(l)), hi(std::move(h)), inclusive(inc) {}
};

// ---------- statements and blocks ----------

enum class StmtKind { VarDecl, Expr, Assign, While, Loop, For };

struct Stmt {
  StmtKind kind;
  Span span;
  Stmt(StmtKind k, Span s) : kind(k), span(s) {}
  virtual ~Stmt() = default;
};

// int x = 5;  var y = 5;  const int z = 5;  const w = 5;  int x;
struct VarDeclStmt : Stmt {
  TypePtr type;  // null means inferred (var or const without type)
  bool is_const;
  std::string name;
  Span name_span;
  ExprPtr init;  // may be null
  explicit VarDeclStmt(Span s) : Stmt(StmtKind::VarDecl, s), is_const(false) {}
};

struct ExprStmt : Stmt {
  ExprPtr expr;
  bool has_semi;
  ExprStmt(Span s, ExprPtr e, bool semi) : Stmt(StmtKind::Expr, s), expr(std::move(e)), has_semi(semi) {}
};

struct AssignStmt : Stmt {
  AssignOp op;
  ExprPtr target, value;
  AssignStmt(Span s, AssignOp o, ExprPtr t, ExprPtr v)
      : Stmt(StmtKind::Assign, s), op(o), target(std::move(t)), value(std::move(v)) {}
};

struct WhileStmt : Stmt {
  ExprPtr cond;
  BlockPtr body;
  explicit WhileStmt(Span s) : Stmt(StmtKind::While, s) {}
};

struct LoopStmt : Stmt {
  BlockPtr body;
  explicit LoopStmt(Span s) : Stmt(StmtKind::Loop, s) {}
};

struct ForStmt : Stmt {
  PatternPtr pattern;
  ExprPtr iter;
  BlockPtr body;
  explicit ForStmt(Span s) : Stmt(StmtKind::For, s) {}
};

struct Block {
  Span span;
  std::vector<StmtPtr> stmts;
  ExprPtr tail;  // value of the block, may be null
};

// ---------- items ----------

enum class SelfKind { None, Value, Ref, ConstRef };

struct Param {
  SelfKind self_kind = SelfKind::None;
  TypePtr type;  // null for self params
  std::string name;
  Span span;
};

struct Function {
  Span span;
  std::string doc;
  bool is_pub = false;
  bool is_async = false;
  bool async_send = false;
  TypePtr ret;  // null means inferred from the body
  std::string name;
  Span name_span;
  std::vector<GenericParam> generics;
  std::vector<Param> params;
  BlockPtr body;  // null for a signature in a trait or extern block
};

struct Field {
  Span span;
  std::string doc;
  bool is_pub = false;
  TypePtr type;
  std::string name;
};

enum class ItemKind { Import, Const, Function, Struct, Enum, Trait, Impl, Extern };

struct Item {
  ItemKind kind;
  Span span;
  std::string doc;
  bool is_pub = false;
  Item(ItemKind k, Span s) : kind(k), span(s) {}
  virtual ~Item() = default;
};

// import a.b.c;  import a.b.{x, y};
struct ImportItem : Item {
  Path path;
  std::vector<PathSegment> names;  // the {x, y} list, empty if none
  explicit ImportItem(Span s) : Item(ItemKind::Import, s) {}
};

struct ConstItem : Item {
  TypePtr type;  // may be null
  std::string name;
  ExprPtr value;
  explicit ConstItem(Span s) : Item(ItemKind::Const, s) {}
};

struct FunctionItem : Item {
  FunctionPtr fn;
  FunctionItem(Span s, FunctionPtr f) : Item(ItemKind::Function, s), fn(std::move(f)) {}
};

struct StructItem : Item {
  std::string name;
  std::vector<GenericParam> generics;
  std::vector<Field> fields;
  std::vector<FunctionPtr> methods;
  explicit StructItem(Span s) : Item(ItemKind::Struct, s) {}
};

enum class VariantKind { Unit, Tuple, Struct };
struct Variant {
  Span span;
  std::string doc;
  std::string name;
  VariantKind kind = VariantKind::Unit;
  std::vector<TypePtr> tuple_types;
  std::vector<Field> fields;
};

struct EnumItem : Item {
  std::string name;
  std::vector<GenericParam> generics;
  std::vector<Variant> variants;
  std::vector<FunctionPtr> methods;
  explicit EnumItem(Span s) : Item(ItemKind::Enum, s) {}
};

struct TraitItem : Item {
  std::string name;
  std::vector<GenericParam> generics;
  std::vector<FunctionPtr> methods;  // body may be null
  explicit TraitItem(Span s) : Item(ItemKind::Trait, s) {}
};

struct ImplItem : Item {
  bool is_unsafe = false;
  std::vector<GenericParam> generics;
  Bound trait;
  TypePtr target;
  std::vector<FunctionPtr> methods;
  explicit ImplItem(Span s) : Item(ItemKind::Impl, s) {}
};

struct ExternItem : Item {
  std::string abi;
  std::vector<FunctionPtr> functions;  // signatures only
  explicit ExternItem(Span s) : Item(ItemKind::Extern, s) {}
};

struct Module {
  std::vector<ItemPtr> items;
};

// Renders a module as an S-expression. Deterministic, used by tests and the
// `synx parse` command.
std::string dump(const Module& module);
std::string dump(const Expr& expr);
std::string dump(const Type& type);
std::string dump(const Pattern& pattern);
std::string dump(const Stmt& stmt);
std::string dump(const Block& block);
std::string dump(const Item& item);

// Complete rendering of every field and span, used by parser parity tests.
// The format is described in compiler/src/ast_strict.cpp.
std::string dump_strict(const Module& module);
std::string dump_strict(const Expr& expr);
std::string dump_strict(const Type& type);
std::string dump_strict(const Pattern& pattern);
std::string dump_strict(const Stmt& stmt);
std::string dump_strict(const Block& block);
std::string dump_strict(const Item& item);

}  // namespace synx::ast
