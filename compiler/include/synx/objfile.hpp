// Object file writers for Axon output: ELF64 (Linux), COFF (Windows), and
// Mach-O (macOS). All three take the same in-memory description.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "synx/x64.hpp"

namespace synx::codegen {

enum class ObjectFormat { Elf, Coff, MachO };

struct ObjSymbol {
  std::string name;
  uint64_t offset = 0;   // in its section
  bool is_text = true;   // false: rodata
  bool global = true;
  bool defined = true;   // false: external reference
};

struct ObjReloc {
  size_t offset;         // in text
  std::string symbol;
  RelocKind kind;
  int64_t addend;
  bool is_text = true;  // false: relocation in rodata
};

struct ObjectFile {
  std::vector<uint8_t> text;
  std::vector<uint8_t> rodata;
  std::vector<ObjSymbol> symbols;
  std::vector<ObjReloc> relocs;  // section selected by ObjReloc::is_text
};

// Serializes the object in the given format. Returns the file bytes.
std::vector<uint8_t> write_object(const ObjectFile& obj, ObjectFormat format);

}  // namespace synx::codegen
