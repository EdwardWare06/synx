#include "synx/layout.hpp"

namespace synx::codegen {

using sema::Type;
using sema::TypeKind;

const Layout& LayoutTable::of(const Type* t) {
  auto it = cache_.find(t);
  if (it != cache_.end()) return it->second;
  Layout l = compute(t);
  return cache_[t] = l;
}

Layout LayoutTable::sequential(const std::vector<const Type*>& fields) {
  Layout l;
  uint64_t offset = 0;
  for (auto* f : fields) {
    const Layout& fl = of(f);
    offset = align_up(offset, fl.align);
    l.field_offsets.push_back(offset);
    offset += fl.size;
    if (fl.align > l.align) l.align = fl.align;
  }
  l.size = align_up(offset, l.align);
  return l;
}

std::vector<const Type*> LayoutTable::struct_field_types(const Type* t) {
  std::vector<const Type*> out;
  sema::Subst s;
  for (size_t i = 0; i < t->struct_def->generics.size() && i < t->elems.size(); ++i) s.bind(t->struct_def->generics[i].type, t->elems[i]);
  for (auto& f : t->struct_def->fields) out.push_back(s.apply(ctx_, f.type));
  return out;
}

std::vector<const Type*> LayoutTable::variant_field_types(const Type* t, uint32_t variant) {
  std::vector<const Type*> out;
  sema::Subst s;
  for (size_t i = 0; i < t->enum_def->generics.size() && i < t->elems.size(); ++i) s.bind(t->enum_def->generics[i].type, t->elems[i]);
  const sema::VariantDef& v = t->enum_def->variants[variant];
  for (auto* tt : v.tuple_types) out.push_back(s.apply(ctx_, tt));
  for (auto& f : v.fields) out.push_back(s.apply(ctx_, f.type));
  return out;
}

Layout LayoutTable::variant_payload(const Type* t, uint32_t variant) {
  return sequential(variant_field_types(t, variant));
}

Layout LayoutTable::compute(const Type* t) {
  Layout l;
  switch (t->kind) {
    case TypeKind::Prim:
      switch (t->prim) {
        case ast::PrimKind::Void: l.size = 0; l.align = 1; break;
        case ast::PrimKind::Bool: case ast::PrimKind::Byte: l.size = 1; l.align = 1; break;
        case ast::PrimKind::Short: l.size = 2; l.align = 2; break;
        case ast::PrimKind::Int: case ast::PrimKind::Uint: case ast::PrimKind::Float: case ast::PrimKind::Char:
          l.size = 4; l.align = 4; break;
        default: l.size = 8; l.align = 8; break;
      }
      return l;
    case TypeKind::Never: case TypeKind::Error:
      l.size = 0; l.align = 1;
      return l;
    case TypeKind::Dyn: case TypeKind::Str: case TypeKind::Slice:
      // Unsized: only reachable through a fat pointer. Element size is what
      // matters; report 0 here.
      l.size = 0; l.align = t->kind == TypeKind::Slice ? of(t->inner).align : 1;
      return l;
    case TypeKind::Ref: case TypeKind::Ptr:
      if (t->inner->is_unsized()) { l.size = 16; l.align = 8; l.is_fat_pointer = true; }
      else { l.size = 8; l.align = 8; }
      return l;
    case TypeKind::Fn:
      l.size = 16; l.align = 8;
      l.field_offsets = {0, 8};  // code pointer, owned environment pointer
      return l;
    case TypeKind::Array: {
      const Layout& e = of(t->inner);
      l.size = e.size * t->length;
      l.align = e.align;
      return l;
    }
    case TypeKind::Tuple:
      return sequential(t->elems);
    case TypeKind::Struct:
      return sequential(struct_field_types(t));
    case TypeKind::Enum: {
      uint64_t payload_size = 0, payload_align = 1;
      for (uint32_t i = 0; i < t->enum_def->variants.size(); ++i) {
        Layout p = sequential(variant_field_types(t, i));
        if (p.size > payload_size) payload_size = p.size;
        if (p.align > payload_align) payload_align = p.align;
      }
      l.payload_offset = align_up(8, payload_align);
      l.align = payload_align > 8 ? payload_align : 8;
      l.size = align_up(l.payload_offset + payload_size, l.align);
      return l;
    }
    default:
      l.size = 8; l.align = 8;
      return l;
  }
}

}  // namespace synx::codegen
