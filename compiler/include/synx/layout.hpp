// Memory layout of Synx types on x86-64. Shared by Axon and the runtime.
//
// Rules:
//  - Primitives have their natural size and alignment. void is empty.
//  - References and raw pointers are 8 bytes, except references to str and
//    slices, which are 16: pointer then length.
//  - Structs and tuples lay fields out in declaration order with natural
//    alignment and tail padding to the struct alignment.
//  - Enums store a usize tag at offset 0 followed by the largest variant
//    payload, aligned to the payload alignment. Variant payload fields are
//    laid out like a struct starting at the payload offset.
//  - Arrays are element size times length, element alignment.
#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "synx/defs.hpp"
#include "synx/types.hpp"

namespace synx::codegen {

struct Layout {
  uint64_t size = 0;
  uint64_t align = 1;
  std::vector<uint64_t> field_offsets;  // struct, tuple, or variant payload fields
  uint64_t payload_offset = 0;          // enums
  bool is_fat_pointer = false;          // &str, &T[]
};

class LayoutTable {
 public:
  LayoutTable(sema::Program& program, sema::TypeContext& ctx) : prog_(program), ctx_(ctx) {}

  const Layout& of(const sema::Type* t);
  // Layout of one variant's payload fields as a struct.
  Layout variant_payload(const sema::Type* enum_type, uint32_t variant);
  uint64_t size_of(const sema::Type* t) { return of(t).size; }
  uint64_t align_of(const sema::Type* t) { return of(t).align; }
  // Byte offset of field i inside a struct or tuple type.
  uint64_t field_offset(const sema::Type* t, uint32_t i) { return of(t).field_offsets[i]; }
  // Field types of a struct type with its arguments applied.
  std::vector<const sema::Type*> struct_field_types(const sema::Type* t);
  std::vector<const sema::Type*> variant_field_types(const sema::Type* enum_type, uint32_t variant);

  static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

 private:
  Layout compute(const sema::Type* t);
  Layout sequential(const std::vector<const sema::Type*>& fields);

  sema::Program& prog_;
  sema::TypeContext& ctx_;
  std::map<const sema::Type*, Layout> cache_;
};

}  // namespace synx::codegen
