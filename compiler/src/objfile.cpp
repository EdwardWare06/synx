#include "synx/objfile.hpp"

#include <cstring>
#include <map>

namespace synx::codegen {

namespace {

class Bytes {
 public:
  std::vector<uint8_t> data;
  size_t size() const { return data.size(); }
  void u8(uint8_t v) { data.push_back(v); }
  void u16(uint16_t v) { for (int i = 0; i < 2; ++i) data.push_back(static_cast<uint8_t>(v >> (8 * i))); }
  void u32(uint32_t v) { for (int i = 0; i < 4; ++i) data.push_back(static_cast<uint8_t>(v >> (8 * i))); }
  void u64(uint64_t v) { for (int i = 0; i < 8; ++i) data.push_back(static_cast<uint8_t>(v >> (8 * i))); }
  void bytes(const std::vector<uint8_t>& b) { data.insert(data.end(), b.begin(), b.end()); }
  void zeros(size_t n) { data.insert(data.end(), n, 0); }
  void align(size_t a) { while (data.size() % a) data.push_back(0); }
  void patch32(size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) data[at + i] = static_cast<uint8_t>(v >> (8 * i)); }
  void patch64(size_t at, uint64_t v) { for (int i = 0; i < 8; ++i) data[at + i] = static_cast<uint8_t>(v >> (8 * i)); }
  void fixed_name(const std::string& s, size_t n) {
    for (size_t i = 0; i < n; ++i) data.push_back(i < s.size() ? static_cast<uint8_t>(s[i]) : 0);
  }
};

// ---------- ELF64 ----------

std::vector<uint8_t> write_elf(const ObjectFile& obj) {
  // Sections: null, .text, .rodata, .rela.text, .symtab, .strtab, .shstrtab
  Bytes strtab;
  strtab.u8(0);
  std::map<std::string, uint32_t> str_off;
  auto add_str = [&](const std::string& s) {
    auto it = str_off.find(s);
    if (it != str_off.end()) return it->second;
    uint32_t off = static_cast<uint32_t>(strtab.size());
    for (char c : s) strtab.u8(static_cast<uint8_t>(c));
    strtab.u8(0);
    str_off[s] = off;
    return off;
  };

  // Symbol table: null, section symbols (.text=1, .rodata=2), then locals, then globals.
  struct Sym { std::string name; uint64_t value; uint16_t shndx; uint8_t info; };
  std::vector<Sym> syms;
  syms.push_back({"", 0, 0, 0});
  syms.push_back({"", 0, 1, 0x03});  // STT_SECTION .text
  syms.push_back({"", 0, 2, 0x03});  // STT_SECTION .rodata
  std::map<std::string, uint32_t> sym_index;
  for (auto& s : obj.symbols) {
    if (s.global) continue;
    sym_index[s.name] = static_cast<uint32_t>(syms.size());
    syms.push_back({s.name, s.offset, static_cast<uint16_t>(s.is_text ? 1 : 2), static_cast<uint8_t>(s.is_text ? 0x02 : 0x01)});
  }
  uint32_t first_global = static_cast<uint32_t>(syms.size());
  for (auto& s : obj.symbols) {
    if (!s.global) continue;
    sym_index[s.name] = static_cast<uint32_t>(syms.size());
    if (s.defined) syms.push_back({s.name, s.offset, static_cast<uint16_t>(s.is_text ? 1 : 2), static_cast<uint8_t>(0x10 | (s.is_text ? 0x02 : 0x01))});
    else syms.push_back({s.name, 0, 0, 0x10});
  }
  // Undefined symbols referenced only by relocations.
  for (auto& r : obj.relocs) {
    if (sym_index.count(r.symbol)) continue;
    sym_index[r.symbol] = static_cast<uint32_t>(syms.size());
    syms.push_back({r.symbol, 0, 0, 0x10});
  }

  Bytes symtab;
  for (auto& s : syms) {
    symtab.u32(s.name.empty() ? 0 : add_str(s.name));
    symtab.u8(s.info);
    symtab.u8(0);
    symtab.u16(s.shndx);
    symtab.u64(s.value);
    symtab.u64(0);
  }

  Bytes rela, rela_rodata;
  for (auto& r : obj.relocs) {
    Bytes& entries = r.is_text ? rela : rela_rodata;
    entries.u64(r.offset);
    uint32_t type = r.kind == RelocKind::Abs64 ? 1 /* R_X86_64_64 */ :
                    r.kind == RelocKind::Call32 ? 4 /* R_X86_64_PLT32 */ : 2 /* R_X86_64_PC32 */;
    entries.u64((static_cast<uint64_t>(sym_index[r.symbol]) << 32) | type);
    entries.u64(static_cast<uint64_t>(r.addend));
  }

  Bytes shstr;
  shstr.u8(0);
  std::map<std::string, uint32_t> sh_off;
  auto add_sh = [&](const std::string& s) {
    uint32_t off = static_cast<uint32_t>(shstr.size());
    for (char c : s) shstr.u8(static_cast<uint8_t>(c));
    shstr.u8(0);
    sh_off[s] = off;
  };
  for (const char* n : {".text", ".rodata", ".rela.text", ".symtab", ".strtab", ".shstrtab", ".rela.rodata"}) add_sh(n);

  Bytes out;
  // ELF header (64 bytes)
  out.bytes({0x7F, 'E', 'L', 'F', 2, 1, 1, 0});
  out.zeros(8);
  out.u16(1);        // ET_REL
  out.u16(0x3E);     // x86-64
  out.u32(1);
  out.u64(0);        // entry
  out.u64(0);        // phoff
  size_t shoff_at = out.size();
  out.u64(0);        // shoff, patched
  out.u32(0);        // flags
  out.u16(64);       // ehsize
  out.u16(0);
  out.u16(0);
  out.u16(64);       // shentsize
  out.u16(8);        // shnum
  out.u16(6);        // shstrndx

  struct SecInfo { uint64_t off, size; };
  auto place = [&](const Bytes& b, size_t align) {
    out.align(align);
    SecInfo i{out.size(), b.size()};
    out.bytes(b.data);
    return i;
  };
  Bytes text; text.data = obj.text;
  Bytes rodata; rodata.data = obj.rodata;
  SecInfo s_text = place(text, 16);
  SecInfo s_rodata = place(rodata, 16);
  SecInfo s_rela = place(rela, 8);
  SecInfo s_rela_rodata = place(rela_rodata, 8);
  SecInfo s_sym = place(symtab, 8);
  SecInfo s_str = place(strtab, 1);
  SecInfo s_shstr = place(shstr, 1);
  out.align(8);
  out.patch64(shoff_at, out.size());

  auto shdr = [&](uint32_t name, uint32_t type, uint64_t flags, SecInfo si, uint32_t link, uint32_t info, uint64_t align, uint64_t entsize) {
    out.u32(name); out.u32(type); out.u64(flags); out.u64(0); out.u64(si.off); out.u64(si.size);
    out.u32(link); out.u32(info); out.u64(align); out.u64(entsize);
  };
  shdr(0, 0, 0, {0, 0}, 0, 0, 0, 0);
  shdr(sh_off[".text"], 1, 0x6, s_text, 0, 0, 16, 0);
  shdr(sh_off[".rodata"], 1, 0x2, s_rodata, 0, 0, 16, 0);
  shdr(sh_off[".rela.text"], 4, 0x40, s_rela, 4, 1, 8, 24);
  shdr(sh_off[".symtab"], 2, 0, s_sym, 5, first_global, 8, 24);
  shdr(sh_off[".strtab"], 3, 0, s_str, 0, 0, 1, 0);
  shdr(sh_off[".shstrtab"], 3, 0, s_shstr, 0, 0, 1, 0);
  shdr(sh_off[".rela.rodata"], 4, 0x40, s_rela_rodata, 4, 2, 8, 24);
  return out.data;
}

// ---------- COFF (x64) ----------

std::vector<uint8_t> write_coff(const ObjectFile& obj) {
  // Sections: .text (1), .rdata (2). Symbols: section symbols then ours.
  Bytes strtab;
  strtab.u32(0);  // size placeholder, patched
  auto sym_name = [&](Bytes& b, const std::string& s) {
    if (s.size() <= 8) {
      b.fixed_name(s, 8);
    } else {
      uint32_t off = static_cast<uint32_t>(strtab.size());
      for (char c : s) strtab.u8(static_cast<uint8_t>(c));
      strtab.u8(0);
      b.u32(0);
      b.u32(off);
    }
  };

  struct Sym { std::string name; uint32_t value; int16_t section; uint8_t cls; };
  std::vector<Sym> syms;
  syms.push_back({".text", 0, 1, 3});
  syms.push_back({".rdata", 0, 2, 3});
  std::map<std::string, uint32_t> sym_index;
  for (auto& s : obj.symbols) {
    sym_index[s.name] = static_cast<uint32_t>(syms.size());
    if (s.defined) syms.push_back({s.name, static_cast<uint32_t>(s.offset), static_cast<int16_t>(s.is_text ? 1 : 2), static_cast<uint8_t>(s.global ? 2 : 3)});
    else syms.push_back({s.name, 0, 0, 2});
  }
  for (auto& r : obj.relocs) {
    if (sym_index.count(r.symbol)) continue;
    sym_index[r.symbol] = static_cast<uint32_t>(syms.size());
    syms.push_back({r.symbol, 0, 0, 2});
  }

  Bytes symtab;
  for (auto& s : syms) {
    sym_name(symtab, s.name);
    symtab.u32(s.value);
    symtab.u16(static_cast<uint16_t>(s.section));
    symtab.u16(s.cls == 3 && s.section > 0 && s.name[0] == '.' ? 0 : 0x20);  // function type for ours
    symtab.u8(s.cls);
    symtab.u8(0);
  }
  strtab.patch32(0, static_cast<uint32_t>(strtab.size()));

  Bytes relocs, rdata_relocs;
  for (auto& r : obj.relocs) {
    Bytes& entries = r.is_text ? relocs : rdata_relocs;
    // COFF REL32 is relative to the end of the 4 byte field, like our -4 addend.
    // Extra addend beyond -4 must be folded into the instruction field by the caller.
    entries.u32(static_cast<uint32_t>(r.offset));
    entries.u32(sym_index[r.symbol]);
    entries.u16(r.kind == RelocKind::Abs64 ? 1 : 4);  // ADDR64 or REL32
  }

  Bytes out;
  out.u16(0x8664);
  out.u16(2);
  out.u32(0);  // timestamp
  size_t symptr_at = out.size();
  out.u32(0);
  out.u32(static_cast<uint32_t>(syms.size()));
  out.u16(0);
  out.u16(0);
  size_t sh_text = out.size();
  out.zeros(40);
  size_t sh_rdata = out.size();
  out.zeros(40);

  size_t text_off = out.size();
  {
    std::vector<uint8_t> text = obj.text;
    for (auto& r : obj.relocs) {
      if (!r.is_text) continue;
      uint64_t v = static_cast<uint64_t>(r.addend + (r.kind == RelocKind::Abs64 ? 0 : 4));
      int width = r.kind == RelocKind::Abs64 ? 8 : 4;
      for (int i = 0; i < width; ++i) text[r.offset + i] = static_cast<uint8_t>(v >> (8 * i));
    }
    out.bytes(text);
  }
  out.align(16);
  size_t reloc_off = out.size();
  out.bytes(relocs.data);
  size_t rdata_off = out.size();
  {
    auto rodata = obj.rodata;
    for (auto& r : obj.relocs) {
      if (r.is_text) continue;
      uint64_t v = static_cast<uint64_t>(r.addend + (r.kind == RelocKind::Abs64 ? 0 : 4));
      int width = r.kind == RelocKind::Abs64 ? 8 : 4;
      for (int i = 0; i < width; ++i) rodata[r.offset + i] = static_cast<uint8_t>(v >> (8 * i));
    }
    out.bytes(rodata);
  }
  out.align(16);
  size_t rdata_reloc_off = out.size();
  out.bytes(rdata_relocs.data);
  size_t sym_off = out.size();
  out.bytes(symtab.data);
  out.bytes(strtab.data);
  out.patch32(symptr_at, static_cast<uint32_t>(sym_off));

  auto section = [&](size_t at, const char* name, uint32_t size, uint32_t data_off, uint32_t reloc_ptr, uint16_t nreloc, uint32_t flags) {
    Bytes h;
    h.fixed_name(name, 8);
    h.u32(0);          // virtual size
    h.u32(0);          // virtual address
    h.u32(size);
    h.u32(data_off);
    h.u32(reloc_ptr);
    h.u32(0);
    h.u16(nreloc);
    h.u16(0);
    h.u32(flags);
    std::memcpy(&out.data[at], h.data.data(), 40);
  };
  section(sh_text, ".text", static_cast<uint32_t>(obj.text.size()), static_cast<uint32_t>(text_off),
          static_cast<uint32_t>(reloc_off), static_cast<uint16_t>(relocs.size() / 10),
          0x60500020);  // code | execute | read | align 16
  section(sh_rdata, ".rdata", static_cast<uint32_t>(obj.rodata.size()), static_cast<uint32_t>(rdata_off), static_cast<uint32_t>(rdata_reloc_off),
          static_cast<uint16_t>(rdata_relocs.size() / 10), 0x40500040);  // initialized data | read | align 16
  return out.data;
}

// ---------- Mach-O (x86-64) ----------

std::vector<uint8_t> write_macho(const ObjectFile& obj) {
  // One segment with __text and __const; symtab with all symbols; relocs on __text.
  // Mach-O symbol names get a leading underscore.
  struct Sym { std::string name; uint64_t value; uint8_t sect; bool ext; bool defined; };
  std::vector<Sym> syms;
  std::map<std::string, uint32_t> sym_index;
  // Order: local defined, external defined, undefined.
  for (auto& s : obj.symbols) if (s.defined && !s.global) { sym_index[s.name] = static_cast<uint32_t>(syms.size()); syms.push_back({"_" + s.name, s.offset, static_cast<uint8_t>(s.is_text ? 1 : 2), false, true}); }
  uint32_t n_local = static_cast<uint32_t>(syms.size());
  for (auto& s : obj.symbols) if (s.defined && s.global) { sym_index[s.name] = static_cast<uint32_t>(syms.size()); syms.push_back({"_" + s.name, s.offset, static_cast<uint8_t>(s.is_text ? 1 : 2), true, true}); }
  uint32_t n_ext = static_cast<uint32_t>(syms.size()) - n_local;
  for (auto& s : obj.symbols) if (!s.defined) { sym_index[s.name] = static_cast<uint32_t>(syms.size()); syms.push_back({"_" + s.name, 0, 0, true, false}); }
  for (auto& r : obj.relocs) {
    if (sym_index.count(r.symbol)) continue;
    sym_index[r.symbol] = static_cast<uint32_t>(syms.size());
    syms.push_back({"_" + r.symbol, 0, 0, true, false});
  }
  uint32_t n_undef = static_cast<uint32_t>(syms.size()) - n_local - n_ext;

  Bytes strtab;
  strtab.u8(0);
  std::vector<uint32_t> name_off;
  for (auto& s : syms) {
    name_off.push_back(static_cast<uint32_t>(strtab.size()));
    for (char c : s.name) strtab.u8(static_cast<uint8_t>(c));
    strtab.u8(0);
  }
  strtab.align(8);

  uint64_t text_size = obj.text.size();
  uint64_t const_off_in_seg = (text_size + 15) / 16 * 16;
  uint64_t seg_size = const_off_in_seg + obj.rodata.size();

  // Relocation entries: all against symbols, pc-relative, length 2 (4 bytes).
  Bytes relocs, const_relocs;
  for (auto& r : obj.relocs) {
    Bytes& entries = r.is_text ? relocs : const_relocs;
    entries.u32(static_cast<uint32_t>(r.offset));
    uint32_t sym = sym_index[r.symbol];
    uint32_t type = r.kind == RelocKind::Abs64 ? 0 /* X86_64_RELOC_UNSIGNED */ :
                    r.kind == RelocKind::Call32 ? 2 /* X86_64_RELOC_BRANCH */ : 1 /* X86_64_RELOC_SIGNED */;
    // r_symbolnum:24, r_pcrel:1, r_length:2, r_extern:1, r_type:4
    bool absolute = r.kind == RelocKind::Abs64;
    uint32_t info = (sym & 0xFFFFFF) | (absolute ? 0 : 1u << 24) | ((absolute ? 3u : 2u) << 25) | (1u << 27) | (type << 28);
    entries.u32(info);
  }

  uint32_t ncmds = 3;  // LC_SEGMENT_64, LC_SYMTAB, LC_DYSYMTAB
  uint32_t seg_cmd_size = 72 + 2 * 80;
  uint32_t sizeofcmds = seg_cmd_size + 24 + 80;
  uint64_t header_total = 32 + sizeofcmds;
  uint64_t data_off = (header_total + 15) / 16 * 16;
  uint64_t reloc_off = data_off + ((seg_size + 7) / 8 * 8);
  uint64_t const_reloc_off = reloc_off + relocs.size();
  uint64_t sym_off = const_reloc_off + const_relocs.size();
  uint64_t str_off = sym_off + syms.size() * 16;

  Bytes out;
  out.u32(0xFEEDFACF);
  out.u32(0x01000007);  // CPU_TYPE_X86_64
  out.u32(0x00000003);  // CPU_SUBTYPE_X86_64_ALL
  out.u32(1);           // MH_OBJECT
  out.u32(ncmds);
  out.u32(sizeofcmds);
  out.u32(0x2000);      // MH_SUBSECTIONS_VIA_SYMBOLS
  out.u32(0);

  // LC_SEGMENT_64
  out.u32(0x19);
  out.u32(seg_cmd_size);
  out.fixed_name("", 16);
  out.u64(0);                 // vmaddr
  out.u64(seg_size);          // vmsize
  out.u64(data_off);          // fileoff
  out.u64(seg_size);          // filesize
  out.u32(7); out.u32(7);     // maxprot, initprot
  out.u32(2);                 // nsects
  out.u32(0);
  // __text
  out.fixed_name("__text", 16);
  out.fixed_name("__TEXT", 16);
  out.u64(0); out.u64(text_size);
  out.u32(static_cast<uint32_t>(data_off));
  out.u32(4);                 // align 2^4
  out.u32(static_cast<uint32_t>(reloc_off));
  out.u32(static_cast<uint32_t>(relocs.size() / 8));
  out.u32(0x80000400);        // S_REGULAR | ATTR_PURE_INSTRUCTIONS | ATTR_SOME_INSTRUCTIONS
  out.u32(0); out.u32(0); out.u32(0);
  // __const
  out.fixed_name("__const", 16);
  out.fixed_name("__DATA_CONST", 16);
  out.u64(const_off_in_seg); out.u64(obj.rodata.size());
  out.u32(static_cast<uint32_t>(data_off + const_off_in_seg));
  out.u32(4);
  out.u32(static_cast<uint32_t>(const_reloc_off)); out.u32(static_cast<uint32_t>(const_relocs.size() / 8));
  out.u32(0);
  out.u32(0); out.u32(0); out.u32(0);

  // LC_SYMTAB
  out.u32(0x2);
  out.u32(24);
  out.u32(static_cast<uint32_t>(sym_off));
  out.u32(static_cast<uint32_t>(syms.size()));
  out.u32(static_cast<uint32_t>(str_off));
  out.u32(static_cast<uint32_t>(strtab.size()));

  // LC_DYSYMTAB
  out.u32(0xB);
  out.u32(80);
  out.u32(0); out.u32(n_local);
  out.u32(n_local); out.u32(n_ext);
  out.u32(n_local + n_ext); out.u32(n_undef);
  for (int i = 0; i < 12; ++i) out.u32(0);

  out.zeros(data_off - out.size());
  {
    std::vector<uint8_t> text = obj.text;
    for (auto& r : obj.relocs) {
      if (!r.is_text) continue;
      uint64_t v = static_cast<uint64_t>(r.addend + (r.kind == RelocKind::Abs64 ? 0 : 4));
      int width = r.kind == RelocKind::Abs64 ? 8 : 4;
      for (int i = 0; i < width; ++i) text[r.offset + i] = static_cast<uint8_t>(v >> (8 * i));
    }
    out.bytes(text);
  }
  out.zeros(const_off_in_seg - text_size);
  {
    auto rodata = obj.rodata;
    for (auto& r : obj.relocs) {
      if (r.is_text) continue;
      uint64_t v = static_cast<uint64_t>(r.addend + (r.kind == RelocKind::Abs64 ? 0 : 4));
      int width = r.kind == RelocKind::Abs64 ? 8 : 4;
      for (int i = 0; i < width; ++i) rodata[r.offset + i] = static_cast<uint8_t>(v >> (8 * i));
    }
    out.bytes(rodata);
  }
  out.zeros(reloc_off - out.size());
  out.bytes(relocs.data);
  out.bytes(const_relocs.data);
  for (size_t i = 0; i < syms.size(); ++i) {
    out.u32(name_off[i]);
    uint8_t type = syms[i].defined ? 0x0E : 0x00;  // N_SECT or N_UNDF
    if (syms[i].ext) type |= 0x01;
    out.u8(type);
    out.u8(syms[i].sect);
    out.u16(0);
    out.u64(syms[i].defined ? (syms[i].sect == 1 ? syms[i].value : const_off_in_seg + syms[i].value) : 0);
  }
  out.bytes(strtab.data);
  return out.data;
}

}  // namespace

std::vector<uint8_t> write_object(const ObjectFile& obj, ObjectFormat format) {
  switch (format) {
    case ObjectFormat::Elf: return write_elf(obj);
    case ObjectFormat::Coff: return write_coff(obj);
    case ObjectFormat::MachO: return write_macho(obj);
  }
  return {};
}

}  // namespace synx::codegen
