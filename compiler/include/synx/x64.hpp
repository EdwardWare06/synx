// x86-64 machine code encoder for Axon. Emits bytes directly, with labels
// for branches inside a function and relocations for symbols outside it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace synx::codegen {

enum Reg : uint8_t {
  RAX = 0, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15
};

enum Xmm : uint8_t { XMM0 = 0, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7 };

enum class Cond : uint8_t {
  O = 0, NO, B, AE, E, NE, BE, A, S, NS, P, NP, L, GE, LE, G
};

enum class Alu : uint8_t { Add = 0, Or = 1, Adc = 2, Sbb = 3, And = 4, Sub = 5, Xor = 6, Cmp = 7 };

enum class RelocKind { Call32, Pc32, Abs64 };  // call rel32 to a symbol; rip-relative data access

struct Reloc {
  size_t offset;       // position of the 4 byte field in code
  std::string symbol;
  RelocKind kind;
  int64_t addend;      // usually -4
};

class Assembler {
 public:
  std::vector<uint8_t> code;
  std::vector<Reloc> relocs;

  // ----- labels -----
  int new_label();
  void bind(int label);
  bool is_bound(int label) const { return labels_[label].pos >= 0; }
  size_t label_pos(int label) const { return static_cast<size_t>(labels_[label].pos); }

  // ----- moves -----
  void mov_rr(Reg dst, Reg src);                       // 64 bit
  void mov_ri(Reg dst, int64_t imm);                   // 64 bit immediate
  void load(Reg dst, Reg base, int32_t disp, uint8_t size, bool sign_extend);  // zero or sign extends to 64
  void store(Reg base, int32_t disp, Reg src, uint8_t size);
  void lea(Reg dst, Reg base, int32_t disp);
  void lea_rip(Reg dst, const std::string& symbol, int64_t addend = 0);
  void lea_label(Reg dst, int label);                  // address of a label in this function

  // ----- integer arithmetic (64 bit) -----
  void alu_rr(Alu op, Reg dst, Reg src);
  void alu_ri(Alu op, Reg dst, int32_t imm);
  void imul_rr(Reg dst, Reg src);
  void mul(Reg src);                                  // unsigned rdx:rax = rax * src
  void cqo();
  void idiv(Reg src);
  void div(Reg src);
  void neg(Reg r);
  void not_(Reg r);
  void shl_cl(Reg r);
  void shr_cl(Reg r);
  void sar_cl(Reg r);
  void test_rr(Reg a, Reg b);
  void setcc(Cond c, Reg dst);                         // sets low byte and zero extends
  void movsx32(Reg dst, Reg src);                      // sign extend low 32 bits
  void movzx8(Reg dst, Reg src);
  void movzx16(Reg dst, Reg src);
  void movsx8(Reg dst, Reg src);
  void movsx16(Reg dst, Reg src);
  void mov32(Reg dst, Reg src);                        // zero extend low 32 bits

  // ----- control flow -----
  void jmp(int label);
  void jcc(Cond c, int label);
  void call_sym(const std::string& symbol);
  void call_reg(Reg r);
  void ret();
  void push(Reg r);
  void pop(Reg r);
  void ud2();

  // ----- memory copy -----
  void rep_movsb();  // copies rcx bytes from [rsi] to [rdi]

  // ----- floating point (scalar double, SSE2) -----
  void movsd_load(Xmm dst, Reg base, int32_t disp);
  void movsd_store(Reg base, int32_t disp, Xmm src);
  void movss_load(Xmm dst, Reg base, int32_t disp);
  void movss_store(Reg base, int32_t disp, Xmm src);
  void movq_xr(Xmm dst, Reg src);
  void movq_rx(Reg dst, Xmm src);
  void addsd(Xmm dst, Xmm src);
  void subsd(Xmm dst, Xmm src);
  void mulsd(Xmm dst, Xmm src);
  void divsd(Xmm dst, Xmm src);
  void ucomisd(Xmm a, Xmm b);
  void cvtsi2sd(Xmm dst, Reg src);
  void cvttsd2si(Reg dst, Xmm src);
  void cvtss2sd(Xmm dst, Xmm src);
  void cvtsd2ss(Xmm dst, Xmm src);
  void xorpd(Xmm dst, Xmm src);
  void movsd_rr(Xmm dst, Xmm src);

  // Finishes label fixups. Call once per function after the last bind.
  void finish();

 private:
  struct Label {
    int32_t pos = -1;
    std::vector<size_t> fixups;  // rel32 fields to patch
  };
  void emit(uint8_t b) { code.push_back(b); }
  void emit32(uint32_t v);
  void emit64(uint64_t v);
  void rex(bool w, uint8_t reg, uint8_t index, uint8_t base, bool force = false);
  void modrm_mem(uint8_t reg, Reg base, int32_t disp);
  void modrm_rr(uint8_t reg, uint8_t rm);
  void rel32_to(int label);

  std::vector<Label> labels_;
};

}  // namespace synx::codegen
