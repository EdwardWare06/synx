// Strict AST rendering for parser parity tests.
//
// The readable dump in ast_dump.cpp hides spans, documentation text, and some
// flags. This format prints every field of every node, in declaration order,
// so two parsers agree only when their trees agree completely.
//
// Grammar of the output:
//   node   = "(" Name " " span { " " field } ")"
//   span   = "@" start "-" end ":" file
//   list   = "[" [ item { " " item } ] "]"
//   string = '"' { byte } '"'   bytes below 0x20, above 0x7e, '"' and '\\'
//                               are written as \xHH with lowercase digits
//   bool   = "t" | "f"          absent pointer = "nil"
//   double = "#" followed by the 16 hex digits of its IEEE bit pattern
// Enumerations print as lowercase words. The Synx port must match this file.

#include <bit>
#include <cstdint>
#include <string>

#include "synx/ast.hpp"

namespace synx::ast {
namespace {

class Strict {
 public:
  std::string out;

  void span(const Span& s) {
    out += '@' + std::to_string(s.start) + '-' + std::to_string(s.end) + ':' + std::to_string(s.file);
  }
  void begin(const char* name, const Span& s) {
    out += '(';
    out += name;
    out += ' ';
    span(s);
  }
  void end() { out += ')'; }
  void sp() { out += ' '; }
  void flag(bool value) { out += value ? 't' : 'f'; }
  void str(const std::string& text) {
    static const char* digits = "0123456789abcdef";
    out += '"';
    for (unsigned char c : text) {
      if (c < 0x20 || c > 0x7e || c == '"' || c == '\\') {
        out += "\\x";
        out += digits[c >> 4];
        out += digits[c & 15];
      } else {
        out += static_cast<char>(c);
      }
    }
    out += '"';
  }
  void number(uint64_t value) { out += std::to_string(value); }
  void bits(double value) {
    static const char* digits = "0123456789abcdef";
    uint64_t raw = std::bit_cast<uint64_t>(value);
    out += '#';
    for (int shift = 60; shift >= 0; shift -= 4) out += digits[(raw >> shift) & 15];
  }
  template <class V, class F>
  void list(const V& values, F emit) {
    out += '[';
    bool first = true;
    for (const auto& value : values) {
      if (!first) out += ' ';
      first = false;
      emit(value);
    }
    out += ']';
  }
  template <class P, class F>
  void opt(const P& pointer, F emit) {
    if (pointer) emit(*pointer);
    else out += "nil";
  }

  const char* mode_name(CallMode mode) {
    return mode == CallMode::Shared ? "shared" : mode == CallMode::Once ? "once" : "writable";
  }
  const char* capture_name(CaptureMode mode) {
    return mode == CaptureMode::Shared ? "shared" : mode == CaptureMode::Writable ? "writable" : "move";
  }
  const char* self_name(SelfKind kind) {
    switch (kind) {
      case SelfKind::None: return "none";
      case SelfKind::Value: return "value";
      case SelfKind::Ref: return "ref";
      case SelfKind::ConstRef: return "constref";
    }
    return "?";
  }

  // ----- shared pieces -----

  void path(const Path& p) {
    begin("Path", p.span);
    sp();
    list(p.segments, [&](const PathSegment& s) {
      begin("Segment", s.span);
      sp();
      str(s.name);
      end();
    });
    end();
  }
  void bound(const Bound& b) {
    begin("Bound", b.span);
    sp();
    path(b.path);
    sp();
    list(b.args, [&](const TypePtr& t) { opt(t, [&](const Type& v) { type(v); }); });
    end();
  }
  void generics(const std::vector<GenericParam>& gs) {
    list(gs, [&](const GenericParam& g) {
      begin("Generic", g.span);
      sp();
      str(g.name);
      sp();
      list(g.bounds, [&](const Bound& b) { bound(b); });
      end();
    });
  }
  void type_list(const std::vector<TypePtr>& types) {
    list(types, [&](const TypePtr& t) { opt(t, [&](const Type& v) { type(v); }); });
  }

  // ----- types -----

  void type(const Type& t) {
    switch (t.kind) {
      case TypeKind::Primitive:
        begin("PrimitiveType", t.span);
        sp();
        out += prim_kind_name(static_cast<const PrimitiveType&>(t).prim);
        end();
        break;
      case TypeKind::Path: {
        auto& p = static_cast<const PathType&>(t);
        begin("PathType", t.span);
        sp();
        path(p.path);
        sp();
        type_list(p.args);
        end();
        break;
      }
      case TypeKind::Ref: {
        auto& r = static_cast<const RefType&>(t);
        begin("RefType", t.span);
        sp();
        flag(r.is_const);
        sp();
        opt(r.inner, [&](const Type& v) { type(v); });
        end();
        break;
      }
      case TypeKind::Ptr: {
        auto& r = static_cast<const PtrType&>(t);
        begin("PtrType", t.span);
        sp();
        flag(r.is_const);
        sp();
        opt(r.inner, [&](const Type& v) { type(v); });
        end();
        break;
      }
      case TypeKind::Array: {
        auto& a = static_cast<const ArrayType&>(t);
        begin("ArrayType", t.span);
        sp();
        opt(a.elem, [&](const Type& v) { type(v); });
        sp();
        opt(a.size, [&](const Expr& v) { expr(v); });
        end();
        break;
      }
      case TypeKind::Slice:
        begin("SliceType", t.span);
        sp();
        opt(static_cast<const SliceType&>(t).elem, [&](const Type& v) { type(v); });
        end();
        break;
      case TypeKind::Optional:
        begin("OptionalType", t.span);
        sp();
        opt(static_cast<const OptionalType&>(t).inner, [&](const Type& v) { type(v); });
        end();
        break;
      case TypeKind::Tuple:
        begin("TupleType", t.span);
        sp();
        type_list(static_cast<const TupleType&>(t).elems);
        end();
        break;
      case TypeKind::Fn: {
        auto& f = static_cast<const FnType&>(t);
        begin("FnType", t.span);
        sp();
        out += mode_name(f.call_mode);
        sp();
        flag(f.is_send);
        sp();
        type_list(f.params);
        sp();
        opt(f.ret, [&](const Type& r) { type(r); });
        end();
        break;
      }
      case TypeKind::Dyn:
        begin("DynType", t.span);
        sp();
        bound(static_cast<const DynType&>(t).bound);
        end();
        break;
      case TypeKind::SelfType:
        begin("SelfType", t.span);
        end();
        break;
    }
  }

  // ----- expressions -----

  void expr_list(const std::vector<ExprPtr>& values) {
    list(values, [&](const ExprPtr& e) { opt(e, [&](const Expr& v) { expr(v); }); });
  }

  void expr(const Expr& e) {
    switch (e.kind) {
      case ExprKind::IntLit: {
        auto& l = static_cast<const IntLitExpr&>(e);
        begin("IntLit", e.span);
        sp();
        number(l.value);
        sp();
        flag(l.is_long);
        end();
        break;
      }
      case ExprKind::FloatLit: {
        auto& l = static_cast<const FloatLitExpr&>(e);
        begin("FloatLit", e.span);
        sp();
        bits(l.value);
        sp();
        flag(l.is_float);
        end();
        break;
      }
      case ExprKind::CharLit:
        begin("CharLit", e.span);
        sp();
        number(static_cast<const CharLitExpr&>(e).value);
        end();
        break;
      case ExprKind::StringLit:
        begin("StringLit", e.span);
        sp();
        str(static_cast<const StringLitExpr&>(e).value);
        end();
        break;
      case ExprKind::BoolLit:
        begin("BoolLit", e.span);
        sp();
        flag(static_cast<const BoolLitExpr&>(e).value);
        end();
        break;
      case ExprKind::Interp:
        begin("Interp", e.span);
        sp();
        list(static_cast<const InterpExpr&>(e).parts, [&](const InterpPart& p) {
          out += "(Part ";
          str(p.text);
          sp();
          opt(p.expr, [&](const Expr& x) { expr(x); });
          out += ')';
        });
        end();
        break;
      case ExprKind::Name: {
        auto& n = static_cast<const NameExpr&>(e);
        begin("Name", e.span);
        sp();
        str(n.name);
        sp();
        type_list(n.type_args);
        end();
        break;
      }
      case ExprKind::Field: {
        auto& f = static_cast<const FieldExpr&>(e);
        begin("Field", e.span);
        sp();
        opt(f.base, [&](const Expr& v) { expr(v); });
        sp();
        str(f.name);
        sp();
        span(f.name_span);
        sp();
        type_list(f.type_args);
        end();
        break;
      }
      case ExprKind::Call: {
        auto& c = static_cast<const CallExpr&>(e);
        begin("Call", e.span);
        sp();
        opt(c.callee, [&](const Expr& v) { expr(v); });
        sp();
        expr_list(c.args);
        end();
        break;
      }
      case ExprKind::Index: {
        auto& i = static_cast<const IndexExpr&>(e);
        begin("Index", e.span);
        sp();
        opt(i.base, [&](const Expr& v) { expr(v); });
        sp();
        opt(i.index, [&](const Expr& v) { expr(v); });
        end();
        break;
      }
      case ExprKind::Unary: {
        auto& u = static_cast<const UnaryExpr&>(e);
        begin("Unary", e.span);
        sp();
        out += unary_op_name(u.op);
        sp();
        opt(u.operand, [&](const Expr& v) { expr(v); });
        end();
        break;
      }
      case ExprKind::Binary: {
        auto& b = static_cast<const BinaryExpr&>(e);
        begin("Binary", e.span);
        sp();
        out += binary_op_name(b.op);
        sp();
        opt(b.lhs, [&](const Expr& v) { expr(v); });
        sp();
        opt(b.rhs, [&](const Expr& v) { expr(v); });
        end();
        break;
      }
      case ExprKind::Cast: {
        auto& c = static_cast<const CastExpr&>(e);
        begin("Cast", e.span);
        sp();
        opt(c.operand, [&](const Expr& v) { expr(v); });
        sp();
        opt(c.type, [&](const Type& v) { type(v); });
        end();
        break;
      }
      case ExprKind::Try:
        begin("Try", e.span);
        sp();
        opt(static_cast<const TryExpr&>(e).operand, [&](const Expr& v) { expr(v); });
        end();
        break;
      case ExprKind::OptChain: {
        auto& o = static_cast<const OptChainExpr&>(e);
        begin("OptChain", e.span);
        sp();
        opt(o.base, [&](const Expr& v) { expr(v); });
        sp();
        str(o.name);
        end();
        break;
      }
      case ExprKind::Coalesce: {
        auto& c = static_cast<const CoalesceExpr&>(e);
        begin("Coalesce", e.span);
        sp();
        opt(c.value, [&](const Expr& v) { expr(v); });
        sp();
        opt(c.fallback, [&](const Expr& v) { expr(v); });
        end();
        break;
      }
      case ExprKind::Range: {
        auto& r = static_cast<const RangeExpr&>(e);
        begin("Range", e.span);
        sp();
        opt(r.start, [&](const Expr& x) { expr(x); });
        sp();
        opt(r.end, [&](const Expr& x) { expr(x); });
        sp();
        flag(r.inclusive);
        end();
        break;
      }
      case ExprKind::Ref: {
        auto& r = static_cast<const RefExpr&>(e);
        begin("Ref", e.span);
        sp();
        flag(r.is_const);
        sp();
        opt(r.operand, [&](const Expr& v) { expr(v); });
        end();
        break;
      }
      case ExprKind::StructLit: {
        auto& s = static_cast<const StructLitExpr&>(e);
        begin("StructLit", e.span);
        sp();
        path(s.path);
        sp();
        list(s.fields, [&](const FieldInit& f) {
          begin("Init", f.span);
          sp();
          str(f.name);
          sp();
          opt(f.value, [&](const Expr& x) { expr(x); });
          end();
        });
        end();
        break;
      }
      case ExprKind::Tuple:
        begin("Tuple", e.span);
        sp();
        expr_list(static_cast<const TupleExpr&>(e).elems);
        end();
        break;
      case ExprKind::Array:
        begin("Array", e.span);
        sp();
        expr_list(static_cast<const ArrayExpr&>(e).elems);
        end();
        break;
      case ExprKind::ArrayRepeat: {
        auto& a = static_cast<const ArrayRepeatExpr&>(e);
        begin("ArrayRepeat", e.span);
        sp();
        opt(a.value, [&](const Expr& v) { expr(v); });
        sp();
        opt(a.count, [&](const Expr& v) { expr(v); });
        end();
        break;
      }
      case ExprKind::Block:
        begin("BlockExpr", e.span);
        sp();
        opt(static_cast<const BlockExpr&>(e).block, [&](const Block& v) { block(v); });
        end();
        break;
      case ExprKind::If: {
        auto& i = static_cast<const IfExpr&>(e);
        begin("If", e.span);
        sp();
        opt(i.cond, [&](const Expr& v) { expr(v); });
        sp();
        opt(i.then_block, [&](const Block& v) { block(v); });
        sp();
        opt(i.else_expr, [&](const Expr& x) { expr(x); });
        end();
        break;
      }
      case ExprKind::Match: {
        auto& m = static_cast<const MatchExpr&>(e);
        begin("Match", e.span);
        sp();
        opt(m.scrutinee, [&](const Expr& v) { expr(v); });
        sp();
        list(m.arms, [&](const MatchArm& arm) {
          begin("Arm", arm.span);
          sp();
          opt(arm.pattern, [&](const Pattern& v) { pattern(v); });
          sp();
          opt(arm.guard, [&](const Expr& x) { expr(x); });
          sp();
          opt(arm.body, [&](const Expr& v) { expr(v); });
          end();
        });
        end();
        break;
      }
      case ExprKind::Closure: {
        auto& c = static_cast<const ClosureExpr&>(e);
        begin("Closure", e.span);
        sp();
        out += mode_name(c.call_mode);
        sp();
        flag(c.is_send);
        sp();
        flag(c.explicit_captures);
        sp();
        list(c.captures, [&](const ClosureExpr::Capture& capture) {
          begin("Capture", capture.span);
          sp();
          str(capture.name);
          sp();
          out += capture_name(capture.mode);
          end();
        });
        sp();
        list(c.params, [&](const ClosureParam& p) {
          begin("ClosureParam", p.span);
          sp();
          opt(p.type, [&](const Type& t) { type(t); });
          sp();
          str(p.name);
          end();
        });
        sp();
        opt(c.ret, [&](const Type& t) { type(t); });
        sp();
        opt(c.body, [&](const Expr& v) { expr(v); });
        end();
        break;
      }
      case ExprKind::Unsafe:
        begin("Unsafe", e.span);
        sp();
        opt(static_cast<const UnsafeExpr&>(e).block, [&](const Block& v) { block(v); });
        end();
        break;
      case ExprKind::Await:
        begin("Await", e.span);
        sp();
        opt(static_cast<const AwaitExpr&>(e).operand, [&](const Expr& v) { expr(v); });
        end();
        break;
      case ExprKind::Return:
        begin("Return", e.span);
        sp();
        opt(static_cast<const ReturnExpr&>(e).value, [&](const Expr& x) { expr(x); });
        end();
        break;
      case ExprKind::Break:
        begin("Break", e.span);
        end();
        break;
      case ExprKind::Continue:
        begin("Continue", e.span);
        end();
        break;
    }
  }

  // ----- patterns -----

  void pattern_list(const std::vector<PatternPtr>& values) {
    list(values, [&](const PatternPtr& p) { opt(p, [&](const Pattern& v) { pattern(v); }); });
  }

  void pattern(const Pattern& p) {
    switch (p.kind) {
      case PatternKind::Wildcard:
        begin("Wildcard", p.span);
        end();
        break;
      case PatternKind::Literal:
        begin("LiteralPattern", p.span);
        sp();
        opt(static_cast<const LiteralPattern&>(p).value, [&](const Expr& v) { expr(v); });
        end();
        break;
      case PatternKind::Binding:
        begin("Binding", p.span);
        sp();
        str(static_cast<const BindingPattern&>(p).name);
        end();
        break;
      case PatternKind::Path:
        begin("PathPattern", p.span);
        sp();
        path(static_cast<const PathPattern&>(p).path);
        end();
        break;
      case PatternKind::TupleStruct: {
        auto& t = static_cast<const TupleStructPattern&>(p);
        begin("TupleStructPattern", p.span);
        sp();
        path(t.path);
        sp();
        pattern_list(t.elems);
        end();
        break;
      }
      case PatternKind::Struct: {
        auto& s = static_cast<const StructPattern&>(p);
        begin("StructPattern", p.span);
        sp();
        path(s.path);
        sp();
        list(s.fields, [&](const FieldPattern& f) {
          begin("FieldPattern", f.span);
          sp();
          str(f.name);
          sp();
          opt(f.pattern, [&](const Pattern& x) { pattern(x); });
          end();
        });
        end();
        break;
      }
      case PatternKind::Tuple:
        begin("TuplePattern", p.span);
        sp();
        pattern_list(static_cast<const TuplePattern&>(p).elems);
        end();
        break;
      case PatternKind::Or:
        begin("OrPattern", p.span);
        sp();
        pattern_list(static_cast<const OrPattern&>(p).alts);
        end();
        break;
      case PatternKind::Range: {
        auto& r = static_cast<const RangePattern&>(p);
        begin("RangePattern", p.span);
        sp();
        opt(r.lo, [&](const Expr& v) { expr(v); });
        sp();
        opt(r.hi, [&](const Expr& v) { expr(v); });
        sp();
        flag(r.inclusive);
        end();
        break;
      }
    }
  }

  // ----- statements and blocks -----

  void stmt(const Stmt& s) {
    switch (s.kind) {
      case StmtKind::VarDecl: {
        auto& v = static_cast<const VarDeclStmt&>(s);
        begin("VarDecl", s.span);
        sp();
        opt(v.type, [&](const Type& t) { type(t); });
        sp();
        flag(v.is_const);
        sp();
        str(v.name);
        sp();
        span(v.name_span);
        sp();
        opt(v.init, [&](const Expr& x) { expr(x); });
        end();
        break;
      }
      case StmtKind::Expr: {
        auto& x = static_cast<const ExprStmt&>(s);
        begin("ExprStmt", s.span);
        sp();
        opt(x.expr, [&](const Expr& v) { expr(v); });
        sp();
        flag(x.has_semi);
        end();
        break;
      }
      case StmtKind::Assign: {
        auto& a = static_cast<const AssignStmt&>(s);
        begin("Assign", s.span);
        sp();
        out += assign_op_name(a.op);
        sp();
        opt(a.target, [&](const Expr& v) { expr(v); });
        sp();
        opt(a.value, [&](const Expr& v) { expr(v); });
        end();
        break;
      }
      case StmtKind::While: {
        auto& w = static_cast<const WhileStmt&>(s);
        begin("While", s.span);
        sp();
        opt(w.cond, [&](const Expr& v) { expr(v); });
        sp();
        opt(w.body, [&](const Block& v) { block(v); });
        end();
        break;
      }
      case StmtKind::Loop:
        begin("Loop", s.span);
        sp();
        opt(static_cast<const LoopStmt&>(s).body, [&](const Block& v) { block(v); });
        end();
        break;
      case StmtKind::For: {
        auto& f = static_cast<const ForStmt&>(s);
        begin("For", s.span);
        sp();
        opt(f.pattern, [&](const Pattern& v) { pattern(v); });
        sp();
        opt(f.iter, [&](const Expr& v) { expr(v); });
        sp();
        opt(f.body, [&](const Block& v) { block(v); });
        end();
        break;
      }
    }
  }

  void block(const Block& b) {
    begin("Block", b.span);
    sp();
    list(b.stmts, [&](const StmtPtr& s) { opt(s, [&](const Stmt& v) { stmt(v); }); });
    sp();
    opt(b.tail, [&](const Expr& x) { expr(x); });
    end();
  }

  // ----- items -----

  void function(const Function& f) {
    begin("Function", f.span);
    sp();
    str(f.doc);
    sp();
    flag(f.is_pub);
    sp();
    flag(f.is_async);
    sp();
    flag(f.async_send);
    sp();
    opt(f.ret, [&](const Type& t) { type(t); });
    sp();
    str(f.name);
    sp();
    span(f.name_span);
    sp();
    generics(f.generics);
    sp();
    list(f.params, [&](const Param& p) {
      begin("Param", p.span);
      sp();
      out += self_name(p.self_kind);
      sp();
      opt(p.type, [&](const Type& t) { type(t); });
      sp();
      str(p.name);
      end();
    });
    sp();
    opt(f.body, [&](const Block& b) { block(b); });
    end();
  }

  void function_list(const std::vector<FunctionPtr>& values) {
    list(values, [&](const FunctionPtr& f) { opt(f, [&](const Function& v) { function(v); }); });
  }

  void field(const Field& f) {
    begin("FieldDecl", f.span);
    sp();
    str(f.doc);
    sp();
    flag(f.is_pub);
    sp();
    opt(f.type, [&](const Type& v) { type(v); });
    sp();
    str(f.name);
    end();
  }

  void item_head(const char* name, const Item& it) {
    begin(name, it.span);
    sp();
    str(it.doc);
    sp();
    flag(it.is_pub);
  }

  void item(const Item& it) {
    switch (it.kind) {
      case ItemKind::Import: {
        auto& i = static_cast<const ImportItem&>(it);
        item_head("Import", it);
        sp();
        path(i.path);
        sp();
        list(i.names, [&](const PathSegment& s) {
          begin("Segment", s.span);
          sp();
          str(s.name);
          end();
        });
        end();
        break;
      }
      case ItemKind::Const: {
        auto& c = static_cast<const ConstItem&>(it);
        item_head("Const", it);
        sp();
        opt(c.type, [&](const Type& t) { type(t); });
        sp();
        str(c.name);
        sp();
        opt(c.value, [&](const Expr& v) { expr(v); });
        end();
        break;
      }
      case ItemKind::Function:
        item_head("FunctionItem", it);
        sp();
        opt(static_cast<const FunctionItem&>(it).fn, [&](const Function& v) { function(v); });
        end();
        break;
      case ItemKind::Struct: {
        auto& s = static_cast<const StructItem&>(it);
        item_head("Struct", it);
        sp();
        str(s.name);
        sp();
        generics(s.generics);
        sp();
        list(s.fields, [&](const Field& f) { field(f); });
        sp();
        function_list(s.methods);
        end();
        break;
      }
      case ItemKind::Enum: {
        auto& e = static_cast<const EnumItem&>(it);
        item_head("Enum", it);
        sp();
        str(e.name);
        sp();
        generics(e.generics);
        sp();
        list(e.variants, [&](const Variant& v) {
          begin("Variant", v.span);
          sp();
          str(v.doc);
          sp();
          str(v.name);
          sp();
          out += v.kind == VariantKind::Unit ? "unit" : v.kind == VariantKind::Tuple ? "tuple" : "struct";
          sp();
          type_list(v.tuple_types);
          sp();
          list(v.fields, [&](const Field& f) { field(f); });
          end();
        });
        sp();
        function_list(e.methods);
        end();
        break;
      }
      case ItemKind::Trait: {
        auto& t = static_cast<const TraitItem&>(it);
        item_head("Trait", it);
        sp();
        str(t.name);
        sp();
        generics(t.generics);
        sp();
        function_list(t.methods);
        end();
        break;
      }
      case ItemKind::Impl: {
        auto& i = static_cast<const ImplItem&>(it);
        item_head("Impl", it);
        sp();
        flag(i.is_unsafe);
        sp();
        generics(i.generics);
        sp();
        bound(i.trait);
        sp();
        opt(i.target, [&](const Type& v) { type(v); });
        sp();
        function_list(i.methods);
        end();
        break;
      }
      case ItemKind::Extern: {
        auto& e = static_cast<const ExternItem&>(it);
        item_head("Extern", it);
        sp();
        str(e.abi);
        sp();
        function_list(e.functions);
        end();
        break;
      }
    }
  }
};

}  // namespace

std::string dump_strict(const Module& module) {
  Strict d;
  d.out += "(Module ";
  d.list(module.items, [&](const ItemPtr& i) { d.opt(i, [&](const Item& v) { d.item(v); }); });
  d.out += ')';
  return d.out;
}
std::string dump_strict(const Expr& e) { Strict d; d.expr(e); return d.out; }
std::string dump_strict(const Type& t) { Strict d; d.type(t); return d.out; }
std::string dump_strict(const Pattern& p) { Strict d; d.pattern(p); return d.out; }
std::string dump_strict(const Stmt& s) { Strict d; d.stmt(s); return d.out; }
std::string dump_strict(const Block& b) { Strict d; d.block(b); return d.out; }
std::string dump_strict(const Item& i) { Strict d; d.item(i); return d.out; }

}  // namespace synx::ast
