#include "synx/x64.hpp"

namespace synx::codegen {

// ---------- primitives ----------

void Assembler::emit32(uint32_t v) {
  for (int i = 0; i < 4; ++i) emit(static_cast<uint8_t>(v >> (8 * i)));
}

void Assembler::emit64(uint64_t v) {
  for (int i = 0; i < 8; ++i) emit(static_cast<uint8_t>(v >> (8 * i)));
}

void Assembler::rex(bool w, uint8_t reg, uint8_t index, uint8_t base, bool force) {
  uint8_t b = 0x40;
  if (w) b |= 8;
  if (reg & 8) b |= 4;
  if (index & 8) b |= 2;
  if (base & 8) b |= 1;
  if (b != 0x40 || force) emit(b);
}

// ModRM for [base + disp32]. Always uses disp32 so encoding size is stable.
void Assembler::modrm_mem(uint8_t reg, Reg base, int32_t disp) {
  uint8_t rm = base & 7;
  emit(static_cast<uint8_t>(0x80 | ((reg & 7) << 3) | rm));
  if (rm == 4) emit(0x24);  // SIB: base rsp/r12, no index
  emit32(static_cast<uint32_t>(disp));
}

void Assembler::modrm_rr(uint8_t reg, uint8_t rm) {
  emit(static_cast<uint8_t>(0xC0 | ((reg & 7) << 3) | (rm & 7)));
}

// ---------- labels ----------

int Assembler::new_label() {
  labels_.emplace_back();
  return static_cast<int>(labels_.size() - 1);
}

void Assembler::bind(int label) { labels_[label].pos = static_cast<int32_t>(code.size()); }

void Assembler::rel32_to(int label) {
  size_t field = code.size();
  emit32(0);
  labels_[label].fixups.push_back(field);
}

void Assembler::finish() {
  for (auto& l : labels_) {
    for (size_t f : l.fixups) {
      int32_t rel = static_cast<int32_t>(l.pos - static_cast<int32_t>(f + 4));
      code[f] = static_cast<uint8_t>(rel);
      code[f + 1] = static_cast<uint8_t>(rel >> 8);
      code[f + 2] = static_cast<uint8_t>(rel >> 16);
      code[f + 3] = static_cast<uint8_t>(rel >> 24);
    }
    l.fixups.clear();
  }
}

// ---------- moves ----------

void Assembler::mov_rr(Reg dst, Reg src) {
  rex(true, src, 0, dst);
  emit(0x89);
  modrm_rr(src, dst);
}

void Assembler::mov_ri(Reg dst, int64_t imm) {
  if (imm >= 0 && imm <= 0xFFFFFFFFLL) {
    // mov r32, imm32 zero extends.
    rex(false, 0, 0, dst);
    emit(static_cast<uint8_t>(0xB8 | (dst & 7)));
    emit32(static_cast<uint32_t>(imm));
  } else if (imm >= INT32_MIN && imm <= INT32_MAX) {
    rex(true, 0, 0, dst);
    emit(0xC7);
    modrm_rr(0, dst);
    emit32(static_cast<uint32_t>(static_cast<int32_t>(imm)));
  } else {
    rex(true, 0, 0, dst);
    emit(static_cast<uint8_t>(0xB8 | (dst & 7)));
    emit64(static_cast<uint64_t>(imm));
  }
}

void Assembler::load(Reg dst, Reg base, int32_t disp, uint8_t size, bool sign_extend) {
  switch (size) {
    case 1:
      rex(sign_extend, dst, 0, base);
      emit(0x0F);
      emit(sign_extend ? 0xBE : 0xB6);
      modrm_mem(dst, base, disp);
      break;
    case 2:
      rex(sign_extend, dst, 0, base);
      emit(0x0F);
      emit(sign_extend ? 0xBF : 0xB7);
      modrm_mem(dst, base, disp);
      break;
    case 4:
      if (sign_extend) {
        rex(true, dst, 0, base);
        emit(0x63);  // movsxd
      } else {
        rex(false, dst, 0, base);
        emit(0x8B);
      }
      modrm_mem(dst, base, disp);
      break;
    default:
      rex(true, dst, 0, base);
      emit(0x8B);
      modrm_mem(dst, base, disp);
      break;
  }
}

void Assembler::store(Reg base, int32_t disp, Reg src, uint8_t size) {
  switch (size) {
    case 1:
      // Need REX to address sil/dil and r8b+ as byte registers.
      rex(false, src, 0, base, true);
      emit(0x88);
      modrm_mem(src, base, disp);
      break;
    case 2:
      emit(0x66);
      rex(false, src, 0, base);
      emit(0x89);
      modrm_mem(src, base, disp);
      break;
    case 4:
      rex(false, src, 0, base);
      emit(0x89);
      modrm_mem(src, base, disp);
      break;
    default:
      rex(true, src, 0, base);
      emit(0x89);
      modrm_mem(src, base, disp);
      break;
  }
}

void Assembler::lea(Reg dst, Reg base, int32_t disp) {
  rex(true, dst, 0, base);
  emit(0x8D);
  modrm_mem(dst, base, disp);
}

void Assembler::lea_rip(Reg dst, const std::string& symbol, int64_t addend) {
  rex(true, dst, 0, 0);
  emit(0x8D);
  emit(static_cast<uint8_t>(0x05 | ((dst & 7) << 3)));
  relocs.push_back({code.size(), symbol, RelocKind::Pc32, addend - 4});
  emit32(0);
}

void Assembler::lea_label(Reg dst, int label) {
  rex(true, dst, 0, 0);
  emit(0x8D);
  emit(static_cast<uint8_t>(0x05 | ((dst & 7) << 3)));
  rel32_to(label);
}

// ---------- integer arithmetic ----------

void Assembler::alu_rr(Alu op, Reg dst, Reg src) {
  rex(true, src, 0, dst);
  emit(static_cast<uint8_t>(static_cast<uint8_t>(op) * 8 + 1));
  modrm_rr(src, dst);
}

void Assembler::alu_ri(Alu op, Reg dst, int32_t imm) {
  rex(true, 0, 0, dst);
  if (imm >= -128 && imm <= 127) {
    emit(0x83);
    modrm_rr(static_cast<uint8_t>(op), dst);
    emit(static_cast<uint8_t>(imm));
  } else {
    emit(0x81);
    modrm_rr(static_cast<uint8_t>(op), dst);
    emit32(static_cast<uint32_t>(imm));
  }
}

void Assembler::imul_rr(Reg dst, Reg src) {
  rex(true, dst, 0, src);
  emit(0x0F);
  emit(0xAF);
  modrm_rr(dst, src);
}

void Assembler::cqo() { emit(0x48); emit(0x99); }
void Assembler::mul(Reg src) { rex(true, 0, 0, src); emit(0xF7); modrm_rr(4, src); }

void Assembler::idiv(Reg src) { rex(true, 0, 0, src); emit(0xF7); modrm_rr(7, src); }
void Assembler::div(Reg src) { rex(true, 0, 0, src); emit(0xF7); modrm_rr(6, src); }
void Assembler::neg(Reg r) { rex(true, 0, 0, r); emit(0xF7); modrm_rr(3, r); }
void Assembler::not_(Reg r) { rex(true, 0, 0, r); emit(0xF7); modrm_rr(2, r); }
void Assembler::shl_cl(Reg r) { rex(true, 0, 0, r); emit(0xD3); modrm_rr(4, r); }
void Assembler::shr_cl(Reg r) { rex(true, 0, 0, r); emit(0xD3); modrm_rr(5, r); }
void Assembler::sar_cl(Reg r) { rex(true, 0, 0, r); emit(0xD3); modrm_rr(7, r); }

void Assembler::test_rr(Reg a, Reg b) {
  rex(true, b, 0, a);
  emit(0x85);
  modrm_rr(b, a);
}

void Assembler::setcc(Cond c, Reg dst) {
  rex(false, 0, 0, dst, true);
  emit(0x0F);
  emit(static_cast<uint8_t>(0x90 | static_cast<uint8_t>(c)));
  modrm_rr(0, dst);
  movzx8(dst, dst);
}

void Assembler::movzx8(Reg dst, Reg src) {
  rex(true, dst, 0, src, true);
  emit(0x0F);
  emit(0xB6);
  modrm_rr(dst, src);
}

void Assembler::movzx16(Reg dst, Reg src) {
  rex(true, dst, 0, src);
  emit(0x0F);
  emit(0xB7);
  modrm_rr(dst, src);
}

void Assembler::movsx8(Reg dst, Reg src) {
  rex(true, dst, 0, src, true);
  emit(0x0F);
  emit(0xBE);
  modrm_rr(dst, src);
}

void Assembler::movsx16(Reg dst, Reg src) {
  rex(true, dst, 0, src);
  emit(0x0F);
  emit(0xBF);
  modrm_rr(dst, src);
}

void Assembler::movsx32(Reg dst, Reg src) {
  rex(true, dst, 0, src);
  emit(0x63);
  modrm_rr(dst, src);
}

void Assembler::mov32(Reg dst, Reg src) {
  rex(false, src, 0, dst);
  emit(0x89);
  modrm_rr(src, dst);
}

// ---------- control flow ----------

void Assembler::jmp(int label) {
  emit(0xE9);
  rel32_to(label);
}

void Assembler::jcc(Cond c, int label) {
  emit(0x0F);
  emit(static_cast<uint8_t>(0x80 | static_cast<uint8_t>(c)));
  rel32_to(label);
}

void Assembler::call_sym(const std::string& symbol) {
  emit(0xE8);
  relocs.push_back({code.size(), symbol, RelocKind::Call32, -4});
  emit32(0);
}

void Assembler::call_reg(Reg r) {
  rex(false, 0, 0, r);
  emit(0xFF);
  modrm_rr(2, r);
}

void Assembler::ret() { emit(0xC3); }

void Assembler::push(Reg r) {
  rex(false, 0, 0, r);
  emit(static_cast<uint8_t>(0x50 | (r & 7)));
}

void Assembler::pop(Reg r) {
  rex(false, 0, 0, r);
  emit(static_cast<uint8_t>(0x58 | (r & 7)));
}

void Assembler::ud2() { emit(0x0F); emit(0x0B); }

void Assembler::rep_movsb() { emit(0xF3); emit(0xA4); }

// ---------- SSE ----------

void Assembler::movsd_load(Xmm dst, Reg base, int32_t disp) {
  emit(0xF2);
  rex(false, dst, 0, base);
  emit(0x0F);
  emit(0x10);
  modrm_mem(dst, base, disp);
}

void Assembler::movsd_store(Reg base, int32_t disp, Xmm src) {
  emit(0xF2);
  rex(false, src, 0, base);
  emit(0x0F);
  emit(0x11);
  modrm_mem(src, base, disp);
}

void Assembler::movss_load(Xmm dst, Reg base, int32_t disp) {
  emit(0xF3);
  rex(false, dst, 0, base);
  emit(0x0F);
  emit(0x10);
  modrm_mem(dst, base, disp);
}

void Assembler::movss_store(Reg base, int32_t disp, Xmm src) {
  emit(0xF3);
  rex(false, src, 0, base);
  emit(0x0F);
  emit(0x11);
  modrm_mem(src, base, disp);
}

void Assembler::movq_xr(Xmm dst, Reg src) {
  emit(0x66);
  rex(true, dst, 0, src);
  emit(0x0F);
  emit(0x6E);
  modrm_rr(dst, src);
}

void Assembler::movq_rx(Reg dst, Xmm src) {
  emit(0x66);
  rex(true, src, 0, dst);
  emit(0x0F);
  emit(0x7E);
  modrm_rr(src, dst);
}

static void sse_rr(Assembler& a, std::vector<uint8_t>& code, uint8_t prefix, uint8_t op, uint8_t dst, uint8_t src) {
  (void)a;
  if (prefix) code.push_back(prefix);
  uint8_t r = 0x40;
  if (dst & 8) r |= 4;
  if (src & 8) r |= 1;
  if (r != 0x40) code.push_back(r);
  code.push_back(0x0F);
  code.push_back(op);
  code.push_back(static_cast<uint8_t>(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

void Assembler::addsd(Xmm dst, Xmm src) { sse_rr(*this, code, 0xF2, 0x58, dst, src); }
void Assembler::subsd(Xmm dst, Xmm src) { sse_rr(*this, code, 0xF2, 0x5C, dst, src); }
void Assembler::mulsd(Xmm dst, Xmm src) { sse_rr(*this, code, 0xF2, 0x59, dst, src); }
void Assembler::divsd(Xmm dst, Xmm src) { sse_rr(*this, code, 0xF2, 0x5E, dst, src); }
void Assembler::ucomisd(Xmm a, Xmm b) { sse_rr(*this, code, 0x66, 0x2E, a, b); }
void Assembler::xorpd(Xmm dst, Xmm src) { sse_rr(*this, code, 0x66, 0x57, dst, src); }
void Assembler::movsd_rr(Xmm dst, Xmm src) { sse_rr(*this, code, 0xF2, 0x10, dst, src); }
void Assembler::cvtss2sd(Xmm dst, Xmm src) { sse_rr(*this, code, 0xF3, 0x5A, dst, src); }
void Assembler::cvtsd2ss(Xmm dst, Xmm src) { sse_rr(*this, code, 0xF2, 0x5A, dst, src); }

void Assembler::cvtsi2sd(Xmm dst, Reg src) {
  emit(0xF2);
  rex(true, dst, 0, src);
  emit(0x0F);
  emit(0x2A);
  modrm_rr(dst, src);
}

void Assembler::cvttsd2si(Reg dst, Xmm src) {
  emit(0xF2);
  rex(true, dst, 0, src);
  emit(0x0F);
  emit(0x2C);
  modrm_rr(dst, src);
}

}  // namespace synx::codegen
