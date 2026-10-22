#include <sstream>

#include "synx/ast.hpp"

namespace synx::ast {

std::string Path::to_string() const {
  std::string out;
  for (size_t i = 0; i < segments.size(); ++i) {
    if (i) out += '.';
    out += segments[i].name;
  }
  return out;
}

const char* prim_kind_name(PrimKind k) {
  switch (k) {
    case PrimKind::Bool: return "bool";
    case PrimKind::Byte: return "byte";
    case PrimKind::Short: return "short";
    case PrimKind::Int: return "int";
    case PrimKind::Long: return "long";
    case PrimKind::Uint: return "uint";
    case PrimKind::Ulong: return "ulong";
    case PrimKind::Usize: return "usize";
    case PrimKind::Float: return "float";
    case PrimKind::Double: return "double";
    case PrimKind::Char: return "char";
    case PrimKind::Void: return "void";
  }
  return "?";
}

const char* unary_op_name(UnaryOp op) {
  switch (op) {
    case UnaryOp::Neg: return "-";
    case UnaryOp::Not: return "!";
    case UnaryOp::BitNot: return "~";
    case UnaryOp::Deref: return "*";
  }
  return "?";
}

const char* binary_op_name(BinaryOp op) {
  switch (op) {
    case BinaryOp::Add: return "+";
    case BinaryOp::Sub: return "-";
    case BinaryOp::Mul: return "*";
    case BinaryOp::Div: return "/";
    case BinaryOp::Rem: return "%";
    case BinaryOp::Eq: return "==";
    case BinaryOp::Ne: return "!=";
    case BinaryOp::Lt: return "<";
    case BinaryOp::Gt: return ">";
    case BinaryOp::Le: return "<=";
    case BinaryOp::Ge: return ">=";
    case BinaryOp::And: return "&&";
    case BinaryOp::Or: return "||";
    case BinaryOp::BitAnd: return "&";
    case BinaryOp::BitOr: return "|";
    case BinaryOp::BitXor: return "^";
    case BinaryOp::Shl: return "<<";
    case BinaryOp::Shr: return ">>";
  }
  return "?";
}

const char* assign_op_name(AssignOp op) {
  switch (op) {
    case AssignOp::Assign: return "=";
    case AssignOp::Add: return "+=";
    case AssignOp::Sub: return "-=";
    case AssignOp::Mul: return "*=";
    case AssignOp::Div: return "/=";
    case AssignOp::Rem: return "%=";
    case AssignOp::BitAnd: return "&=";
    case AssignOp::BitOr: return "|=";
    case AssignOp::BitXor: return "^=";
    case AssignOp::Shl: return "<<=";
    case AssignOp::Shr: return ">>=";
  }
  return "?";
}

namespace {

class Dumper {
 public:
  std::string out;

  void type(const Type& t) {
    switch (t.kind) {
      case TypeKind::Primitive:
        out += prim_kind_name(static_cast<const PrimitiveType&>(t).prim);
        break;
      case TypeKind::Path: {
        auto& p = static_cast<const PathType&>(t);
        out += p.path.to_string();
        if (!p.args.empty()) {
          out += '<';
          for (size_t i = 0; i < p.args.size(); ++i) {
            if (i) out += ", ";
            type(*p.args[i]);
          }
          out += '>';
        }
        break;
      }
      case TypeKind::Ref: {
        auto& r = static_cast<const RefType&>(t);
        out += r.is_const ? "&const " : "&";
        type(*r.inner);
        break;
      }
      case TypeKind::Ptr: {
        auto& r = static_cast<const PtrType&>(t);
        out += r.is_const ? "*const " : "*";
        type(*r.inner);
        break;
      }
      case TypeKind::Array: {
        auto& a = static_cast<const ArrayType&>(t);
        type(*a.elem);
        out += '[';
        expr(*a.size);
        out += ']';
        break;
      }
      case TypeKind::Slice:
        type(*static_cast<const SliceType&>(t).elem);
        out += "[]";
        break;
      case TypeKind::Optional:
        type(*static_cast<const OptionalType&>(t).inner);
        out += '?';
        break;
      case TypeKind::Tuple: {
        auto& tt = static_cast<const TupleType&>(t);
        out += '(';
        for (size_t i = 0; i < tt.elems.size(); ++i) {
          if (i) out += ", ";
          type(*tt.elems[i]);
        }
        out += ')';
        break;
      }
      case TypeKind::Fn: {
        auto& f = static_cast<const FnType&>(t);
        out += f.call_mode == CallMode::Shared ? "fn const" : f.call_mode == CallMode::Once ? "fn once" : "fn";
        if (f.is_send) out += " send";
        out += '(';
        for (size_t i = 0; i < f.params.size(); ++i) {
          if (i) out += ", ";
          type(*f.params[i]);
        }
        out += ')';
        if (f.ret) {
          out += " -> ";
          type(*f.ret);
        }
        break;
      }
      case TypeKind::Dyn: {
        auto& d = static_cast<const DynType&>(t);
        out += "dyn ";
        bound(d.bound);
        break;
      }
      case TypeKind::SelfType:
        out += "Self";
        break;
    }
  }

  void bound(const Bound& b) {
    out += b.path.to_string();
    if (!b.args.empty()) {
      out += '<';
      for (size_t i = 0; i < b.args.size(); ++i) {
        if (i) out += ", ";
        type(*b.args[i]);
      }
      out += '>';
    }
  }

  void generics(const std::vector<GenericParam>& gs) {
    if (gs.empty()) return;
    out += " (generics";
    for (auto& g : gs) {
      out += " (";
      out += g.name;
      for (auto& b : g.bounds) {
        out += ' ';
        bound(b);
      }
      out += ')';
    }
    out += ')';
  }

  void type_args(const std::vector<TypePtr>& args) {
    if (args.empty()) return;
    out += " (targs";
    for (auto& a : args) {
      out += ' ';
      type(*a);
    }
    out += ')';
  }

  void expr(const Expr& e) {
    switch (e.kind) {
      case ExprKind::IntLit: {
        auto& l = static_cast<const IntLitExpr&>(e);
        out += std::to_string(l.value);
        if (l.is_long) out += 'L';
        break;
      }
      case ExprKind::FloatLit: {
        auto& l = static_cast<const FloatLitExpr&>(e);
        std::ostringstream s;
        s << l.value;
        out += s.str();
        if (s.str().find('.') == std::string::npos && s.str().find('e') == std::string::npos) out += ".0";
        if (l.is_float) out += 'f';
        break;
      }
      case ExprKind::CharLit: {
        std::ostringstream s;
        s << "(char " << static_cast<const CharLitExpr&>(e).value << ')';
        out += s.str();
        break;
      }
      case ExprKind::StringLit:
        out += '"';
        out += static_cast<const StringLitExpr&>(e).value;
        out += '"';
        break;
      case ExprKind::BoolLit:
        out += static_cast<const BoolLitExpr&>(e).value ? "true" : "false";
        break;
      case ExprKind::Interp: {
        out += "(interp";
        for (auto& p : static_cast<const InterpExpr&>(e).parts) {
          out += ' ';
          if (p.expr) expr(*p.expr);
          else { out += '"'; out += p.text; out += '"'; }
        }
        out += ')';
        break;
      }
      case ExprKind::Name: {
        auto& n = static_cast<const NameExpr&>(e);
        if (n.type_args.empty()) {
          out += n.name;
        } else {
          out += "(name ";
          out += n.name;
          type_args(n.type_args);
          out += ')';
        }
        break;
      }
      case ExprKind::Field: {
        auto& f = static_cast<const FieldExpr&>(e);
        out += "(. ";
        expr(*f.base);
        out += ' ';
        out += f.name;
        type_args(f.type_args);
        out += ')';
        break;
      }
      case ExprKind::Call: {
        auto& c = static_cast<const CallExpr&>(e);
        out += "(call ";
        expr(*c.callee);
        for (auto& a : c.args) {
          out += ' ';
          expr(*a);
        }
        out += ')';
        break;
      }
      case ExprKind::Index: {
        auto& i = static_cast<const IndexExpr&>(e);
        out += "(index ";
        expr(*i.base);
        out += ' ';
        expr(*i.index);
        out += ')';
        break;
      }
      case ExprKind::Unary: {
        auto& u = static_cast<const UnaryExpr&>(e);
        out += "(unary ";
        out += unary_op_name(u.op);
        out += ' ';
        expr(*u.operand);
        out += ')';
        break;
      }
      case ExprKind::Binary: {
        auto& b = static_cast<const BinaryExpr&>(e);
        out += '(';
        out += binary_op_name(b.op);
        out += ' ';
        expr(*b.lhs);
        out += ' ';
        expr(*b.rhs);
        out += ')';
        break;
      }
      case ExprKind::Cast: {
        auto& c = static_cast<const CastExpr&>(e);
        out += "(as ";
        expr(*c.operand);
        out += ' ';
        type(*c.type);
        out += ')';
        break;
      }
      case ExprKind::Try:
        out += "(try ";
        expr(*static_cast<const TryExpr&>(e).operand);
        out += ')';
        break;
      case ExprKind::OptChain: {
        auto& o = static_cast<const OptChainExpr&>(e);
        out += "(?. ";
        expr(*o.base);
        out += ' ';
        out += o.name;
        out += ')';
        break;
      }
      case ExprKind::Coalesce: {
        auto& c = static_cast<const CoalesceExpr&>(e);
        out += "(?? ";
        expr(*c.value);
        out += ' ';
        expr(*c.fallback);
        out += ')';
        break;
      }
      case ExprKind::Range: {
        auto& r = static_cast<const RangeExpr&>(e);
        out += r.inclusive ? "(..= " : "(.. ";
        if (r.start) expr(*r.start); else out += '_';
        out += ' ';
        if (r.end) expr(*r.end); else out += '_';
        out += ')';
        break;
      }
      case ExprKind::Ref: {
        auto& r = static_cast<const RefExpr&>(e);
        out += r.is_const ? "(&const " : "(& ";
        expr(*r.operand);
        out += ')';
        break;
      }
      case ExprKind::StructLit: {
        auto& s = static_cast<const StructLitExpr&>(e);
        out += "(struct ";
        out += s.path.to_string();
        for (auto& f : s.fields) {
          out += " (";
          out += f.name;
          if (f.value) {
            out += ' ';
            expr(*f.value);
          }
          out += ')';
        }
        out += ')';
        break;
      }
      case ExprKind::Tuple: {
        out += "(tuple";
        for (auto& x : static_cast<const TupleExpr&>(e).elems) {
          out += ' ';
          expr(*x);
        }
        out += ')';
        break;
      }
      case ExprKind::Array: {
        out += "(array";
        for (auto& x : static_cast<const ArrayExpr&>(e).elems) {
          out += ' ';
          expr(*x);
        }
        out += ')';
        break;
      }
      case ExprKind::ArrayRepeat: {
        auto& a = static_cast<const ArrayRepeatExpr&>(e);
        out += "(array-repeat ";
        expr(*a.value);
        out += ' ';
        expr(*a.count);
        out += ')';
        break;
      }
      case ExprKind::Block:
        block(*static_cast<const BlockExpr&>(e).block);
        break;
      case ExprKind::If: {
        auto& i = static_cast<const IfExpr&>(e);
        out += "(if ";
        expr(*i.cond);
        out += ' ';
        block(*i.then_block);
        if (i.else_expr) {
          out += ' ';
          expr(*i.else_expr);
        }
        out += ')';
        break;
      }
      case ExprKind::Match: {
        auto& m = static_cast<const MatchExpr&>(e);
        out += "(match ";
        expr(*m.scrutinee);
        for (auto& arm : m.arms) {
          out += " (arm ";
          pattern(*arm.pattern);
          if (arm.guard) {
            out += " (if ";
            expr(*arm.guard);
            out += ')';
          }
          out += ' ';
          expr(*arm.body);
          out += ')';
        }
        out += ')';
        break;
      }
      case ExprKind::Closure: {
        auto& c = static_cast<const ClosureExpr&>(e);
        out += c.call_mode == CallMode::Shared ? "(closure const" : c.call_mode == CallMode::Once ? "(closure once" : "(closure";
        if (c.is_send) out += " send";
        if (c.explicit_captures) {
          out += " (captures";
          for (const auto& capture : c.captures) {
            out += " ";
            if (capture.mode != CaptureMode::Move) out += capture.mode == CaptureMode::Shared ? "&const " : "&";
            out += capture.name;
          }
          out += ")";
        }
        out += " (params";
        for (auto& p : c.params) {
          out += " (";
          if (p.type) {
            type(*p.type);
            out += ' ';
          }
          out += p.name;
          out += ')';
        }
        out += ')';
        if (c.ret) {
          out += " (ret ";
          type(*c.ret);
          out += ')';
        }
        out += ' ';
        expr(*c.body);
        out += ')';
        break;
      }
      case ExprKind::Unsafe:
        out += "(unsafe ";
        block(*static_cast<const UnsafeExpr&>(e).block);
        out += ')';
        break;
      case ExprKind::Await:
        out += "(await ";
        expr(*static_cast<const AwaitExpr&>(e).operand);
        out += ')';
        break;
      case ExprKind::Return: {
        auto& r = static_cast<const ReturnExpr&>(e);
        out += "(return";
        if (r.value) {
          out += ' ';
          expr(*r.value);
        }
        out += ')';
        break;
      }
      case ExprKind::Break:
        out += "(break)";
        break;
      case ExprKind::Continue:
        out += "(continue)";
        break;
    }
  }

  void pattern(const Pattern& p) {
    switch (p.kind) {
      case PatternKind::Wildcard:
        out += '_';
        break;
      case PatternKind::Literal:
        expr(*static_cast<const LiteralPattern&>(p).value);
        break;
      case PatternKind::Binding:
        out += static_cast<const BindingPattern&>(p).name;
        break;
      case PatternKind::Path:
        out += static_cast<const PathPattern&>(p).path.to_string();
        break;
      case PatternKind::TupleStruct: {
        auto& t = static_cast<const TupleStructPattern&>(p);
        out += '(';
        out += t.path.to_string();
        for (auto& e : t.elems) {
          out += ' ';
          pattern(*e);
        }
        out += ')';
        break;
      }
      case PatternKind::Struct: {
        auto& s = static_cast<const StructPattern&>(p);
        out += "(struct ";
        out += s.path.to_string();
        for (auto& f : s.fields) {
          out += " (";
          out += f.name;
          if (f.pattern) {
            out += ' ';
            pattern(*f.pattern);
          }
          out += ')';
        }
        out += ')';
        break;
      }
      case PatternKind::Tuple: {
        out += "(tuple";
        for (auto& e : static_cast<const TuplePattern&>(p).elems) {
          out += ' ';
          pattern(*e);
        }
        out += ')';
        break;
      }
      case PatternKind::Or: {
        out += "(or";
        for (auto& a : static_cast<const OrPattern&>(p).alts) {
          out += ' ';
          pattern(*a);
        }
        out += ')';
        break;
      }
      case PatternKind::Range: {
        auto& r = static_cast<const RangePattern&>(p);
        out += r.inclusive ? "(..= " : "(.. ";
        expr(*r.lo);
        out += ' ';
        expr(*r.hi);
        out += ')';
        break;
      }
    }
  }

  void stmt(const Stmt& s) {
    switch (s.kind) {
      case StmtKind::VarDecl: {
        auto& v = static_cast<const VarDeclStmt&>(s);
        out += v.is_const ? "(const " : "(var ";
        if (v.type) {
          type(*v.type);
          out += ' ';
        }
        out += v.name;
        if (v.init) {
          out += ' ';
          expr(*v.init);
        }
        out += ')';
        break;
      }
      case StmtKind::Expr:
        expr(*static_cast<const ExprStmt&>(s).expr);
        break;
      case StmtKind::Assign: {
        auto& a = static_cast<const AssignStmt&>(s);
        out += '(';
        out += assign_op_name(a.op);
        out += ' ';
        expr(*a.target);
        out += ' ';
        expr(*a.value);
        out += ')';
        break;
      }
      case StmtKind::While: {
        auto& w = static_cast<const WhileStmt&>(s);
        out += "(while ";
        expr(*w.cond);
        out += ' ';
        block(*w.body);
        out += ')';
        break;
      }
      case StmtKind::Loop:
        out += "(loop ";
        block(*static_cast<const LoopStmt&>(s).body);
        out += ')';
        break;
      case StmtKind::For: {
        auto& f = static_cast<const ForStmt&>(s);
        out += "(for ";
        pattern(*f.pattern);
        out += ' ';
        expr(*f.iter);
        out += ' ';
        block(*f.body);
        out += ')';
        break;
      }
    }
  }

  void block(const Block& b) {
    out += "(block";
    for (auto& s : b.stmts) {
      out += ' ';
      stmt(*s);
    }
    if (b.tail) {
      out += " (tail ";
      expr(*b.tail);
      out += ')';
    }
    out += ')';
  }

  void function(const Function& f) {
    out += "(fn ";
    out += f.name;
    if (f.is_pub) out += " pub";
    if (f.is_async) out += f.async_send ? " async send" : " async";
    generics(f.generics);
    if (f.ret) {
      out += " (ret ";
      type(*f.ret);
      out += ')';
    }
    out += " (params";
    for (auto& p : f.params) {
      out += " (";
      switch (p.self_kind) {
        case SelfKind::Value: out += "self"; break;
        case SelfKind::Ref: out += "&self"; break;
        case SelfKind::ConstRef: out += "&const self"; break;
        case SelfKind::None:
          type(*p.type);
          out += ' ';
          out += p.name;
          break;
      }
      out += ')';
    }
    out += ')';
    if (f.body) {
      out += ' ';
      block(*f.body);
    }
    out += ')';
  }

  void field(const Field& f) {
    out += " (field ";
    if (f.is_pub) out += "pub ";
    type(*f.type);
    out += ' ';
    out += f.name;
    out += ')';
  }

  void item(const Item& it) {
    switch (it.kind) {
      case ItemKind::Import: {
        auto& i = static_cast<const ImportItem&>(it);
        out += "(import ";
        out += i.path.to_string();
        if (!i.names.empty()) {
          out += " {";
          for (size_t k = 0; k < i.names.size(); ++k) {
            if (k) out += ", ";
            out += i.names[k].name;
          }
          out += '}';
        }
        out += ')';
        break;
      }
      case ItemKind::Const: {
        auto& c = static_cast<const ConstItem&>(it);
        out += "(const ";
        if (c.is_pub) out += "pub ";
        if (c.type) {
          type(*c.type);
          out += ' ';
        }
        out += c.name;
        out += ' ';
        expr(*c.value);
        out += ')';
        break;
      }
      case ItemKind::Function:
        function(*static_cast<const FunctionItem&>(it).fn);
        break;
      case ItemKind::Struct: {
        auto& s = static_cast<const StructItem&>(it);
        out += "(struct ";
        out += s.name;
        if (s.is_pub) out += " pub";
        generics(s.generics);
        for (auto& f : s.fields) field(f);
        for (auto& m : s.methods) {
          out += ' ';
          function(*m);
        }
        out += ')';
        break;
      }
      case ItemKind::Enum: {
        auto& e = static_cast<const EnumItem&>(it);
        out += "(enum ";
        out += e.name;
        if (e.is_pub) out += " pub";
        generics(e.generics);
        for (auto& v : e.variants) {
          out += " (variant ";
          out += v.name;
          for (auto& t : v.tuple_types) {
            out += ' ';
            type(*t);
          }
          for (auto& f : v.fields) field(f);
          out += ')';
        }
        for (auto& m : e.methods) {
          out += ' ';
          function(*m);
        }
        out += ')';
        break;
      }
      case ItemKind::Trait: {
        auto& t = static_cast<const TraitItem&>(it);
        out += "(trait ";
        out += t.name;
        if (t.is_pub) out += " pub";
        generics(t.generics);
        for (auto& m : t.methods) {
          out += ' ';
          function(*m);
        }
        out += ')';
        break;
      }
      case ItemKind::Impl: {
        auto& i = static_cast<const ImplItem&>(it);
        out += i.is_unsafe ? "(unsafe impl" : "(impl";
        generics(i.generics);
        out += ' ';
        bound(i.trait);
        out += " for ";
        type(*i.target);
        for (auto& m : i.methods) {
          out += ' ';
          function(*m);
        }
        out += ')';
        break;
      }
      case ItemKind::Extern: {
        auto& e = static_cast<const ExternItem&>(it);
        out += "(extern \"";
        out += e.abi;
        out += '"';
        for (auto& f : e.functions) {
          out += ' ';
          function(*f);
        }
        out += ')';
        break;
      }
    }
  }
};

}  // namespace

std::string dump(const Module& module) {
  Dumper d;
  for (size_t i = 0; i < module.items.size(); ++i) {
    if (i) d.out += '\n';
    d.item(*module.items[i]);
  }
  return d.out;
}
std::string dump(const Expr& e) { Dumper d; d.expr(e); return d.out; }
std::string dump(const Type& t) { Dumper d; d.type(t); return d.out; }
std::string dump(const Pattern& p) { Dumper d; d.pattern(p); return d.out; }
std::string dump(const Stmt& s) { Dumper d; d.stmt(s); return d.out; }
std::string dump(const Block& b) { Dumper d; d.block(b); return d.out; }
std::string dump(const Item& i) { Dumper d; d.item(i); return d.out; }

}  // namespace synx::ast
