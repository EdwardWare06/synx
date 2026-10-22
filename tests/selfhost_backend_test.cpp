// Stage 6 backend parity. Replay instruction inputs, then serialize objects in
// all three formats. Compare every byte and relocation record against stage 0.
#include <array>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include "selfhost_support.hpp"
#include "synx/x64.hpp"
#include "synx/objfile.hpp"
#include "synx/axon.hpp"
#include "test_support.hpp"

using namespace selfhost_test;
using namespace synx::codegen;

enum Opcode : uint8_t {
  op_bind,
  op_finish,
  op_mov_rr,
  op_mov_ri,
  op_load,
  op_store,
  op_lea,
  op_lea_rip,
  op_lea_label,
  op_alu_rr,
  op_alu_ri,
  op_imul_rr,
  op_cqo,
  op_mul,
  op_idiv,
  op_div,
  op_neg,
  op_not_,
  op_shl_cl,
  op_shr_cl,
  op_sar_cl,
  op_test_rr,
  op_setcc,
  op_movzx8,
  op_movzx16,
  op_movsx8,
  op_movsx16,
  op_movsx32,
  op_mov32,
  op_jmp,
  op_jcc,
  op_call_sym,
  op_call_reg,
  op_ret,
  op_push,
  op_pop,
  op_ud2,
  op_rep_movsb,
  op_movsd_load,
  op_movsd_store,
  op_movss_load,
  op_movss_store,
  op_movq_xr,
  op_movq_rx,
  op_addsd,
  op_subsd,
  op_mulsd,
  op_divsd,
  op_ucomisd,
  op_xorpd,
  op_movsd_rr,
  op_cvtss2sd,
  op_cvtsd2ss,
  op_cvtsi2sd,
  op_cvttsd2si,
};
struct Operation {
  Opcode code;
  uint8_t a = 0, b = 0, c = 0, d = 0;
  int64_t imm = 0;
  std::string symbol;
};
struct Program { unsigned labels = 0; std::vector<Operation> operations; };

static void execute(Assembler& assembler, const Operation& op) {
  switch (op.code) {
    case op_bind: assembler.bind(static_cast<int>(op.imm)); break;
    case op_finish: assembler.finish(); break;
    case op_mov_rr: assembler.mov_rr(static_cast<Reg>(op.a), static_cast<Reg>(op.b)); break;
    case op_mov_ri: assembler.mov_ri(static_cast<Reg>(op.a), op.imm); break;
    case op_load: assembler.load(static_cast<Reg>(op.a), static_cast<Reg>(op.b), static_cast<int32_t>(op.imm), op.c, op.d != 0); break;
    case op_store: assembler.store(static_cast<Reg>(op.a), static_cast<int32_t>(op.imm), static_cast<Reg>(op.b), op.c); break;
    case op_lea: assembler.lea(static_cast<Reg>(op.a), static_cast<Reg>(op.b), static_cast<int32_t>(op.imm)); break;
    case op_lea_rip: assembler.lea_rip(static_cast<Reg>(op.a), op.symbol, op.imm); break;
    case op_lea_label: assembler.lea_label(static_cast<Reg>(op.a), static_cast<int>(op.imm)); break;
    case op_alu_rr: assembler.alu_rr(static_cast<Alu>(op.a), static_cast<Reg>(op.b), static_cast<Reg>(op.c)); break;
    case op_alu_ri: assembler.alu_ri(static_cast<Alu>(op.a), static_cast<Reg>(op.b), static_cast<int32_t>(op.imm)); break;
    case op_imul_rr: assembler.imul_rr(static_cast<Reg>(op.a), static_cast<Reg>(op.b)); break;
    case op_cqo: assembler.cqo(); break;
    case op_mul: assembler.mul(static_cast<Reg>(op.a)); break;
    case op_idiv: assembler.idiv(static_cast<Reg>(op.a)); break;
    case op_div: assembler.div(static_cast<Reg>(op.a)); break;
    case op_neg: assembler.neg(static_cast<Reg>(op.a)); break;
    case op_not_: assembler.not_(static_cast<Reg>(op.a)); break;
    case op_shl_cl: assembler.shl_cl(static_cast<Reg>(op.a)); break;
    case op_shr_cl: assembler.shr_cl(static_cast<Reg>(op.a)); break;
    case op_sar_cl: assembler.sar_cl(static_cast<Reg>(op.a)); break;
    case op_test_rr: assembler.test_rr(static_cast<Reg>(op.a), static_cast<Reg>(op.b)); break;
    case op_setcc: assembler.setcc(static_cast<Cond>(op.a), static_cast<Reg>(op.b)); break;
    case op_movzx8: assembler.movzx8(static_cast<Reg>(op.a), static_cast<Reg>(op.b)); break;
    case op_movzx16: assembler.movzx16(static_cast<Reg>(op.a), static_cast<Reg>(op.b)); break;
    case op_movsx8: assembler.movsx8(static_cast<Reg>(op.a), static_cast<Reg>(op.b)); break;
    case op_movsx16: assembler.movsx16(static_cast<Reg>(op.a), static_cast<Reg>(op.b)); break;
    case op_movsx32: assembler.movsx32(static_cast<Reg>(op.a), static_cast<Reg>(op.b)); break;
    case op_mov32: assembler.mov32(static_cast<Reg>(op.a), static_cast<Reg>(op.b)); break;
    case op_jmp: assembler.jmp(static_cast<int>(op.imm)); break;
    case op_jcc: assembler.jcc(static_cast<Cond>(op.a), static_cast<int>(op.imm)); break;
    case op_call_sym: assembler.call_sym(op.symbol); break;
    case op_call_reg: assembler.call_reg(static_cast<Reg>(op.a)); break;
    case op_ret: assembler.ret(); break;
    case op_push: assembler.push(static_cast<Reg>(op.a)); break;
    case op_pop: assembler.pop(static_cast<Reg>(op.a)); break;
    case op_ud2: assembler.ud2(); break;
    case op_rep_movsb: assembler.rep_movsb(); break;
    case op_movsd_load: assembler.movsd_load(static_cast<Xmm>(op.a), static_cast<Reg>(op.b), static_cast<int32_t>(op.imm)); break;
    case op_movsd_store: assembler.movsd_store(static_cast<Reg>(op.a), static_cast<int32_t>(op.imm), static_cast<Xmm>(op.b)); break;
    case op_movss_load: assembler.movss_load(static_cast<Xmm>(op.a), static_cast<Reg>(op.b), static_cast<int32_t>(op.imm)); break;
    case op_movss_store: assembler.movss_store(static_cast<Reg>(op.a), static_cast<int32_t>(op.imm), static_cast<Xmm>(op.b)); break;
    case op_movq_xr: assembler.movq_xr(static_cast<Xmm>(op.a), static_cast<Reg>(op.b)); break;
    case op_movq_rx: assembler.movq_rx(static_cast<Reg>(op.a), static_cast<Xmm>(op.b)); break;
    case op_addsd: assembler.addsd(static_cast<Xmm>(op.a), static_cast<Xmm>(op.b)); break;
    case op_subsd: assembler.subsd(static_cast<Xmm>(op.a), static_cast<Xmm>(op.b)); break;
    case op_mulsd: assembler.mulsd(static_cast<Xmm>(op.a), static_cast<Xmm>(op.b)); break;
    case op_divsd: assembler.divsd(static_cast<Xmm>(op.a), static_cast<Xmm>(op.b)); break;
    case op_ucomisd: assembler.ucomisd(static_cast<Xmm>(op.a), static_cast<Xmm>(op.b)); break;
    case op_xorpd: assembler.xorpd(static_cast<Xmm>(op.a), static_cast<Xmm>(op.b)); break;
    case op_movsd_rr: assembler.movsd_rr(static_cast<Xmm>(op.a), static_cast<Xmm>(op.b)); break;
    case op_cvtss2sd: assembler.cvtss2sd(static_cast<Xmm>(op.a), static_cast<Xmm>(op.b)); break;
    case op_cvtsd2ss: assembler.cvtsd2ss(static_cast<Xmm>(op.a), static_cast<Xmm>(op.b)); break;
    case op_cvtsi2sd: assembler.cvtsi2sd(static_cast<Xmm>(op.a), static_cast<Reg>(op.b)); break;
    case op_cvttsd2si: assembler.cvttsd2si(static_cast<Reg>(op.a), static_cast<Xmm>(op.b)); break;
  }
}
static std::string byte_string(const std::vector<uint8_t>& value) {
  return std::string(value.begin(), value.end());
}
static void write_file(const fs::path& path, const std::string& data) {
  std::ofstream out(path, std::ios::binary);
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  if (!out) throw std::runtime_error("cannot write " + utf8(path));
}
static bool keep_artifacts() {
#if defined(_WIN32)
  char* value = nullptr; size_t length = 0;
  const bool keep = _dupenv_s(&value, &length, "SYNX_KEEP_BACKEND") == 0 && value != nullptr;
  std::free(value); return keep;
#else
  return std::getenv("SYNX_KEEP_BACKEND") != nullptr;
#endif
}
static std::vector<Program> instruction_cases() {
  std::vector<Program> programs(1); // Empty function.
  Program p;
  auto emit = [&](Opcode code, uint8_t a = 0, uint8_t b = 0, uint8_t c = 0,
                  uint8_t d = 0, int64_t imm = 0, const std::string& symbol = "") {
    p.operations.push_back({code, a, b, c, d, imm, symbol});
  };
  const std::array<int32_t, 11> disps = {INT32_MIN, -65536, -129, -128, -1, 0, 1, 127, 128, 65536, INT32_MAX};
  const std::array<int64_t, 17> immediates = {
      INT64_MIN, INT64_MIN + 4, INT32_MIN - INT64_C(1), INT32_MIN, -129, -128, -1,
      0, 1, 127, 128, INT32_MAX, INT64_C(2147483648), INT64_C(4294967295),
      INT64_C(4294967296), INT64_MAX - 4, INT64_MAX};
  // Every GPR pair, all ALU operations and condition encodings.
  for (uint8_t a = 0; a < 16; ++a) {
    for (auto imm : immediates) emit(op_mov_ri, a, 0, 0, 0, imm);
    for (auto op : {op_mul, op_idiv, op_div, op_neg, op_not_, op_shl_cl, op_shr_cl,
                    op_sar_cl, op_call_reg, op_push, op_pop}) emit(op, a);
    for (uint8_t b = 0; b < 16; ++b) {
      for (auto op : {op_mov_rr, op_imul_rr, op_test_rr, op_movzx8, op_movzx16,
                      op_movsx8, op_movsx16, op_movsx32, op_mov32}) emit(op, a, b);
      for (uint8_t alu = 0; alu < 8; ++alu) emit(op_alu_rr, alu, a, b);
      emit(op_setcc, a, b);
      for (auto disp : disps) {
        emit(op_lea, a, b, 0, 0, disp);
        for (int size : {0, 1, 2, 4, 8, 16}) {
          emit(op_load, a, b, static_cast<uint8_t>(size), 0, disp);
          emit(op_load, a, b, static_cast<uint8_t>(size), 1, disp);
          emit(op_store, a, b, static_cast<uint8_t>(size), 0, disp);
        }
      }
    }
    for (uint8_t alu = 0; alu < 8; ++alu)
      for (auto imm : disps) emit(op_alu_ri, alu, a, 0, 0, imm);
  }
  for (auto op : {op_cqo, op_ret, op_ud2, op_rep_movsb}) emit(op);
  programs.push_back(std::move(p)); p = Program{};
  // Every supported XMM/GPR combination, with RSP/R12 SIB addressing.
  for (uint8_t xmm = 0; xmm < 8; ++xmm) {
    for (uint8_t reg = 0; reg < 16; ++reg) {
      emit(op_movq_xr, xmm, reg); emit(op_movq_rx, reg, xmm);
      emit(op_cvtsi2sd, xmm, reg); emit(op_cvttsd2si, reg, xmm);
      for (auto disp : disps) {
        emit(op_movsd_load, xmm, reg, 0, 0, disp);
        emit(op_movsd_store, reg, xmm, 0, 0, disp);
        emit(op_movss_load, xmm, reg, 0, 0, disp);
        emit(op_movss_store, reg, xmm, 0, 0, disp);
      }
    }
    for (uint8_t src = 0; src < 8; ++src)
      for (auto op : {op_addsd, op_subsd, op_mulsd, op_divsd, op_ucomisd,
                      op_xorpd, op_movsd_rr, op_cvtss2sd, op_cvtsd2ss}) emit(op, xmm, src);
  }
  programs.push_back(std::move(p)); p = Program{};
  // Forward/backward labels, repeated finish, later fixups to bound labels,
  // unused labels, and displacements on both sides of short-branch limits.
  p.labels = 7;
  emit(op_bind, 0, 0, 0, 0, 0);
  emit(op_jmp, 0, 0, 0, 0, 1);
  for (int n = 0; n < 63; ++n) emit(op_ud2);
  emit(op_bind, 0, 0, 0, 0, 1);
  for (uint8_t condition = 0; condition < 16; ++condition) {
    emit(op_jcc, condition, 0, 0, 0, 0); emit(op_jcc, condition, 0, 0, 0, 2);
  }
  for (uint8_t reg = 0; reg < 16; ++reg) {
    emit(op_lea_label, reg, 0, 0, 0, 0); emit(op_lea_label, reg, 0, 0, 0, 3);
  }
  emit(op_bind, 0, 0, 0, 0, 2); emit(op_bind, 0, 0, 0, 0, 3);
  emit(op_finish); emit(op_finish);
  for (int n = 0; n < 65; ++n) emit(op_ud2);
  emit(op_jmp, 0, 0, 0, 0, 0); emit(op_jmp, 0, 0, 0, 0, 4);
  emit(op_bind, 0, 0, 0, 0, 4); emit(op_bind, 0, 0, 0, 0, 5);
  emit(op_finish);
  // Rebinding changes later fixups only; finish must have cleared earlier ones.
  emit(op_bind, 0, 0, 0, 0, 0); emit(op_jmp, 0, 0, 0, 0, 0);
  programs.push_back(std::move(p)); p = Program{};
  const std::vector<std::string> names = {"", "f", "12345678", "123456789",
      "external_symbol_longer_than_eight_bytes", "copper_\xCE\xBB", std::string("embedded\0name", 13)};
  for (uint8_t reg = 0; reg < 16; ++reg) {
    for (const auto& name : names) {
      emit(op_call_sym, 0, 0, 0, 0, 0, name);
      for (int64_t addend : {INT64_MIN + 4, INT64_C(-4096), INT64_C(-4), INT64_C(0), INT64_C(4), INT64_C(4096), INT64_MAX})
        emit(op_lea_rip, reg, 0, 0, 0, addend, name);
    }
  }
  programs.push_back(std::move(p));
  return programs;
}
static std::vector<ObjectFile> object_cases() {
  std::vector<ObjectFile> objects;
  // Cross every section-alignment boundary, including independently empty sections.
  const std::array<size_t, 10> sizes = {0, 1, 7, 8, 15, 16, 17, 31, 32, 33};
  for (size_t text : sizes) for (size_t rodata : sizes) {
    ObjectFile obj;
    for (size_t i = 0; i < text; ++i) obj.text.push_back(static_cast<uint8_t>(i * 17));
    for (size_t i = 0; i < rodata; ++i) obj.rodata.push_back(static_cast<uint8_t>(255 - i * 3));
    objects.push_back(std::move(obj));
  }
  ObjectFile full;
  full.text.resize(128, 0xCC); full.rodata.resize(128, 0xA5);
  full.symbols = {{"public_text", 16, true, true, true},
                  {"local_data", 8, false, false, true},
                  {"external", 0, false, true, false},
                  {"local_text", 1, true, false, true},
                  {"public_data", 32, false, true, true},
                  {"12345678", 4, true, true, true},
                  {"123456789", 5, true, true, true},
                  {"duplicate", 6, true, false, true},
                  {"duplicate", 7, false, true, true},
                  {"copper_\xCE\xBB", 9, false, false, true},
                  {std::string("embedded\0name", 13), 11, true, true, true}};
  for (bool text : {false, true}) {
    size_t offset = 0;
    for (auto kind : {RelocKind::Call32, RelocKind::Pc32, RelocKind::Abs64}) {
      for (int64_t addend : {INT64_C(-4096), INT64_C(-4), INT64_C(0), INT64_C(4096)}) {
        const std::array<std::string, 4> names = {"duplicate", "relocation_only", "public_data", "external"};
        full.relocs.push_back({offset, names[(offset / 8) % names.size()], kind, addend, text});
        offset += 8;
      }
    }
    full.relocs.push_back({112, "public_text", RelocKind::Abs64, INT64_MIN, text});
    full.relocs.push_back({120, "local_data", RelocKind::Abs64, INT64_MAX, text});
  }
  objects.push_back(full);
  // Fixed seed produces stable cases on every host. Symbols and relocations
  // vary independently; duplicate names retain stage-0 last-index behavior.
  uint32_t state = 0x6E657572;
  auto next = [&]() { state = state * UINT32_C(1664525) + UINT32_C(1013904223); return state; };
  for (unsigned fixture = 0; fixture < 128; ++fixture) {
    ObjectFile obj;
    obj.text.resize(16 + next() % 240); obj.rodata.resize(16 + next() % 240);
    for (auto& b : obj.text) b = static_cast<uint8_t>(next() >> 24);
    for (auto& b : obj.rodata) b = static_cast<uint8_t>(next() >> 24);
    for (unsigned j = 0; j < 12; ++j) {
      bool text = (next() >> 9) & 1, global = (next() >> 11) & 1, defined = (next() >> 13) & 1;
      obj.symbols.push_back({"symbol_" + std::to_string(next() % 8), next() % (text ? obj.text.size() : obj.rodata.size()), text, global, defined});
    }
    for (unsigned j = 0; j < 24; ++j) {
      bool text = (next() >> 9) & 1;
      auto kind = static_cast<RelocKind>(next() % 3);
      const size_t width = kind == RelocKind::Abs64 ? 8 : 4;
      const size_t size = text ? obj.text.size() : obj.rodata.size();
      const size_t offset = next() % (size - width + 1);
      obj.relocs.push_back({offset, "symbol_" + std::to_string(next() % 12), kind,
                           static_cast<int64_t>(next() % 8192) - 4096, text});
    }
    objects.push_back(std::move(obj));
  }
  return objects;
}
static void label_report(std::string& out, const Assembler& assembler, unsigned labels) {
  for (unsigned i = 0; i < labels; ++i) {
    integer(out, assembler.is_bound(static_cast<int>(i)), 1);
    integer(out, assembler.label_pos(static_cast<int>(i)), 8);
  }
}
static void reports(std::string& input, std::string& expected, size_t& operation_count, size_t& object_count) {
  const auto programs = instruction_cases();
  integer(input, programs.size(), 8); integer(expected, programs.size(), 8);
  for (const auto& p : programs) {
    Assembler assembler;
    integer(input, p.labels, 8);
    for (unsigned i = 0; i < p.labels; ++i) integer(expected, assembler.new_label(), 8);
    label_report(expected, assembler, p.labels);
    integer(input, p.operations.size(), 8); operation_count += p.operations.size();
    for (const auto& op : p.operations) {
      integer(input, op.code, 1); integer(input, op.a, 1); integer(input, op.b, 1);
      integer(input, op.c, 1); integer(input, op.d, 1); integer(input, static_cast<uint64_t>(op.imm), 8);
      blob(input, op.symbol); execute(assembler, op);
    }
    assembler.finish(); blob(expected, byte_string(assembler.code));
    integer(expected, assembler.relocs.size(), 8);
    for (const auto& reloc : assembler.relocs) {
      integer(expected, reloc.offset, 8); blob(expected, reloc.symbol);
      integer(expected, static_cast<unsigned>(reloc.kind), 1);
      integer(expected, static_cast<uint64_t>(reloc.addend), 8);
    }
    label_report(expected, assembler, p.labels);
  }
  const auto objects = object_cases(); object_count = objects.size();
  integer(input, objects.size(), 8); integer(expected, objects.size(), 8);
  for (const auto& obj : objects) {
    blob(input, byte_string(obj.text)); blob(input, byte_string(obj.rodata));
    integer(input, obj.symbols.size(), 8);
    for (const auto& symbol : obj.symbols) {
      blob(input, symbol.name); integer(input, symbol.offset, 8); integer(input, symbol.is_text, 1);
      integer(input, symbol.global, 1); integer(input, symbol.defined, 1);
    }
    integer(input, obj.relocs.size(), 8);
    for (const auto& reloc : obj.relocs) {
      integer(input, reloc.offset, 8); blob(input, reloc.symbol);
      integer(input, static_cast<unsigned>(reloc.kind), 1);
      integer(input, static_cast<uint64_t>(reloc.addend), 8); integer(input, reloc.is_text, 1);
    }
    for (auto format : {ObjectFormat::Elf, ObjectFormat::Coff, ObjectFormat::MachO})
      blob(expected, byte_string(write_object(obj, format)));
    blob(expected, byte_string(obj.text)); blob(expected, byte_string(obj.rodata));
  }
  for (auto os : {TargetOs::Linux, TargetOs::Windows, TargetOs::MacOs}) {
    for (unsigned flags = 0; flags < 4; ++flags) {
      Target target{os};
      integer(expected, target.register_allocation, 1); integer(expected, target.checked_arithmetic, 1);
      target.register_allocation = (flags & 1) != 0; target.checked_arithmetic = (flags & 2) != 0;
      blob(expected, target.name()); integer(expected, target.win64_abi(), 1);
      integer(expected, static_cast<unsigned>(target.format()), 1);
      integer(expected, target.register_allocation, 1); integer(expected, target.checked_arithmetic, 1);
    }
  }
  std::vector<std::string> names = {"", "main", "a.b", "a_b", "a-b", "A0z9", "___", "::[]<>&*()",
      "std.collections.HashMap<int,String>.insert", "neural_\xCE\xBB", std::string("zero\0byte", 9), std::string(8192, 'x')};
  for (unsigned i = 1; i < 128; ++i) names.push_back("name_" + std::string(1, static_cast<char>(i)));
  integer(input, names.size(), 8); integer(expected, names.size(), 8);
  for (const auto& name : names) { blob(input, name); blob(expected, Axon::symbol_for(name)); }
}
int main(int argc, char** argv) {
  if (argc != 4) return 1;
  const unsigned mode = static_cast<unsigned>(std::stoi(argv[3]));
  const auto root = fs::temp_directory_path() / ("synx-backend-" + std::to_string(mode) + '-' +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    fs::create_directories(root);
    const fs::path compiler = fs::absolute(argv[1]), source = fs::absolute(argv[2]);
    const auto exe = root / "backend-probe.exe", report = root / "actual.bin",
               corpus = root / "input.bin", log = root / "run.log";
    std::string input, expected; size_t operations = 0, objects = 0;
    reports(input, expected, operations, objects); write_file(corpus, input);
    std::vector<std::string> args = {"build", utf8(source / "selfhost/backend_probe.snx"), "-o", utf8(exe)};
    if (mode & 1) args.push_back("--no-regalloc");
    if (mode & 2) args.push_back("--release");
    int rc = run(compiler, args, log); CHECK(rc == 0);
    if (!rc) { rc = run(exe, {utf8(corpus), utf8(report)}, log); CHECK(rc == 0); }
    if (rc) throw std::runtime_error(read(log));
    auto actual = read(report); CHECK(actual == expected);
    if (actual != expected) {
      write_file(root / "expected.bin", expected);
      size_t offset = 0;
      while (offset < expected.size() && offset < actual.size() && expected[offset] == actual[offset]) ++offset;
      throw std::runtime_error("backend parity mismatch at byte " + std::to_string(offset) +
          " (expected " + std::to_string(expected.size()) + ", actual " + std::to_string(actual.size()) + ')');
    }
    std::printf("mode %u: %zu instruction operations, %zu objects x 3 formats; %zu bytes\n",
                mode, operations, objects, actual.size());
    if (keep_artifacts()) std::printf("artifacts: %s\n", utf8(root).c_str());
    else fs::remove_all(root);
    return synx_test::finish("selfhost_backend");
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s\nartifacts: %s\n", e.what(), utf8(root).c_str()); return 1;
  }
}

