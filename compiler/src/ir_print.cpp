#include <sstream>

#include "synx/ir.hpp"

namespace synx::ir {

namespace {

std::string place_str(const Place& p) {
  std::string s = "_" + std::to_string(p.local);
  for (auto& pr : p.proj) {
    switch (pr.kind) {
      case ProjKind::Field: s += "." + std::to_string(pr.index); break;
      case ProjKind::Index: s += "[_" + std::to_string(pr.index) + "]"; break;
      case ProjKind::Deref: s = "(*" + s + ")"; break;
      case ProjKind::Downcast: s = "(" + s + " as variant " + std::to_string(pr.index) + ")"; break;
    }
  }
  return s;
}

std::string const_str(const Constant& c) {
  std::ostringstream s;
  switch (c.kind) {
    case Constant::Kind::Int: s << c.int_value; if (c.type) s << "_" << c.type->to_string(); break;
    case Constant::Kind::Float: s << c.float_value; if (c.type) s << "_" << c.type->to_string(); break;
    case Constant::Kind::Bool: s << (c.int_value ? "true" : "false"); break;
    case Constant::Kind::Char: s << "'\\u{" << std::hex << c.int_value << std::dec << "}'"; break;
    case Constant::Kind::Str: s << '"' << c.str_value << '"'; break;
    case Constant::Kind::Unit: s << "()"; break;
    case Constant::Kind::Fn: s << "fn " << (c.fn ? c.fn->name : "?"); break;
  }
  return s.str();
}

std::string operand_str(const Operand& o) {
  switch (o.kind) {
    case Operand::Kind::Copy: return "copy " + place_str(o.place);
    case Operand::Kind::Move: return "move " + place_str(o.place);
    case Operand::Kind::Const: return "const " + const_str(o.constant);
  }
  return "?";
}

std::string rvalue_str(const Rvalue& r) {
  switch (r.kind) {
    case RvalueKind::Use: return operand_str(r.a);
    case RvalueKind::Ref: return std::string(r.is_const ? "&const " : "&") + place_str(r.place);
    case RvalueKind::Binary:
      return std::string(ast::binary_op_name(r.bop)) + "(" + operand_str(r.a) + ", " + operand_str(r.b) + ")";
    case RvalueKind::Unary: return std::string(ast::unary_op_name(r.uop)) + "(" + operand_str(r.a) + ")";
    case RvalueKind::Cast: return operand_str(r.a) + " as " + r.type->to_string();
    case RvalueKind::Aggregate: {
      std::string s;
      switch (r.agg) {
        case AggregateKind::Struct: s = r.type->to_string() + " {"; break;
        case AggregateKind::Tuple: s = "("; break;
        case AggregateKind::Array: s = "["; break;
        case AggregateKind::Variant: s = r.type->to_string() + "::" + std::to_string(r.variant) + "("; break;
      }
      for (size_t i = 0; i < r.elems.size(); ++i) {
        if (i) s += ", ";
        s += operand_str(r.elems[i]);
      }
      switch (r.agg) {
        case AggregateKind::Struct: s += "}"; break;
        case AggregateKind::Tuple: s += ")"; break;
        case AggregateKind::Array: s += "]"; break;
        case AggregateKind::Variant: s += ")"; break;
      }
      if (!r.captures.empty()) {
        s += " captures [";
        for (size_t i = 0; i < r.captures.size(); ++i) { if (i) s += ", "; s += operand_str(r.captures[i]); }
        s += "]";
      }
      return s;
    }
    case RvalueKind::Discriminant: return "discriminant(" + place_str(r.place) + ")";
    case RvalueKind::Len: return "len(" + place_str(r.place) + ")";
    case RvalueKind::StrFromString: return "str_from_string(" + operand_str(r.a) + ")";
    case RvalueKind::ArrayToSlice: return "array_to_slice(" + operand_str(r.a) + ")";
    case RvalueKind::ToDyn: return "to_dyn(" + operand_str(r.a) + ", " + r.type->to_string() + ")";
    case RvalueKind::VecToSlice: return "vec_to_slice(" + operand_str(r.a) + ")";
  }
  return "?";
}

std::string term_str(const Terminator& t) {
  std::ostringstream s;
  switch (t.kind) {
    case Terminator::Kind::Goto: s << "goto bb" << t.target; break;
    case Terminator::Kind::Switch:
      s << "switch " << operand_str(t.discr) << " [";
      for (size_t i = 0; i < t.cases.size(); ++i) {
        if (i) s << ", ";
        s << t.cases[i].first << ": bb" << t.cases[i].second;
      }
      s << ", otherwise: bb" << t.otherwise << "]";
      break;
    case Terminator::Kind::Call: {
      if (!t.diverges && !t.dest.is_none()) s << place_str(t.dest) << " = ";
      switch (t.callee.kind) {
        case Callee::Kind::Direct: s << "call " << t.callee.fn->name; break;
        case Callee::Kind::Builtin: s << "call @" << t.callee.builtin; break;
        case Callee::Kind::Virtual: s << "call virtual[" << t.callee.vtable_slot << "]"; break;
        case Callee::Kind::Indirect: s << "call (" << operand_str(t.callee.fn_operand) << ")"; break;
      }
      s << "(";
      for (size_t i = 0; i < t.args.size(); ++i) {
        if (i) s << ", ";
        s << operand_str(t.args[i]);
      }
      s << ")";
      if (t.diverges) s << " -> diverges";
      else s << " -> bb" << t.target;
      break;
    }
    case Terminator::Kind::Assert:
      s << "assert " << operand_str(t.cond) << " \"" << t.message << "\" -> bb" << t.target;
      break;
    case Terminator::Kind::Return: s << "return"; break;
    case Terminator::Kind::Unreachable: s << "unreachable"; break;
  }
  return s.str();
}

}  // namespace

std::string dump(const Function& f) {
  std::ostringstream s;
  s << "fn " << f.name << "(";
  for (uint32_t i = 1; i <= f.arg_count; ++i) {
    if (i > 1) s << ", ";
    s << "_" << i << ": " << f.locals[i].type->to_string();
  }
  s << ") -> " << (f.ret ? f.ret->to_string() : "?") << " {\n";
  for (size_t i = 0; i < f.locals.size(); ++i) {
    s << "  let _" << i << ": " << f.locals[i].type->to_string();
    if (!f.locals[i].name.empty()) s << "  // " << f.locals[i].name;
    s << "\n";
  }
  for (size_t b = 0; b < f.blocks.size(); ++b) {
    s << "  bb" << b << ":\n";
    for (auto& st : f.blocks[b].stmts) {
      switch (st.kind) {
        case Statement::Kind::Assign: s << "    " << place_str(st.place) << " = " << rvalue_str(st.rvalue) << "\n"; break;
        case Statement::Kind::Drop: s << "    drop " << place_str(st.place) << "\n"; break;
        case Statement::Kind::DropIf: s << "    drop " << place_str(st.place) << " if _" << st.flag_local << "\n"; break;
        case Statement::Kind::StorageDead: s << "    storage_dead " << place_str(st.place) << "\n"; break;
        case Statement::Kind::Nop: s << "    nop\n"; break;
      }
    }
    s << "    " << term_str(f.blocks[b].term) << "\n";
  }
  s << "}\n";
  return s.str();
}

std::string dump(const Module& m) {
  std::string out;
  for (auto& f : m.functions) {
    if (!f->lowered) continue;
    if (!out.empty()) out += "\n";
    out += dump(*f);
  }
  return out;
}

}  // namespace synx::ir
