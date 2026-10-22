// Stage 4a type parity. Includes predicates, formatting, and every identity pair.
#include <chrono>
#include <stdexcept>

#include "selfhost_support.hpp"
#include "synx/defs.hpp"
#include "test_support.hpp"

using namespace selfhost_test;
using namespace synx::sema;
using synx::ast::PrimKind;
using synx::ast::CallMode;

static std::string reference() {
  TypeContext ctx;
  std::vector<const Type*> samples;
  for (int i = 0; i < 12; ++i) samples.push_back(ctx.prim(static_cast<PrimKind>(i)));
  samples.insert(samples.end(), {ctx.str(), ctx.never(), ctx.error()});
  for (auto kind : {InferKind::Any, InferKind::Int, InferKind::Float}) {
    samples.push_back(ctx.fresh_infer(kind)); samples.push_back(ctx.fresh_infer(kind));
  }
  FunctionDef owner, other;
  const Type* p = ctx.param(&owner, 0, "T"), *q = ctx.param(&other, 0, "T");
  samples.insert(samples.end(), {p, q, ctx.param(&owner, 0, "ignored"), ctx.param(&owner, 1, "U")});
  auto* integer_type = ctx.int_type(); auto* void_type = ctx.void_type();
  ModuleDef module; module.name = "m";
  StructDef sd; sd.module = &module; sd.name = "Record";
  EnumDef ed; ed.module = &module; ed.name = "Choice";
  TraitDef td; td.module = &module; td.name = "View";
  StructDef sd2; sd2.module = &module; sd2.name = "Record";
  samples.push_back(ctx.struct_type(&sd, {})); samples.push_back(ctx.struct_type(&sd2, {}));
  samples.push_back(ctx.enum_type(&ed, {})); samples.push_back(ctx.dyn_type(&td, {}));
  samples.push_back(ctx.param(&sd, 0, "T")); samples.push_back(ctx.tuple({p}));
  samples.push_back(ctx.tuple({}));
  const size_t bases = samples.size();
  for (size_t i = 0; i < bases; ++i) {
    auto* base = samples[i];
    for (int repeat = 0; repeat < 2; ++repeat) {
      samples.push_back(ctx.ref(base, false)); samples.push_back(ctx.ref(base, true));
      samples.push_back(ctx.ptr(base, false)); samples.push_back(ctx.ptr(base, true));
      samples.push_back(ctx.array(base, 0)); samples.push_back(ctx.array(base, 1));
      samples.push_back(ctx.array(base, UINT64_MAX)); samples.push_back(ctx.slice(base));
      samples.push_back(ctx.tuple({base, integer_type}));
      for (auto mode : {CallMode::Writable, CallMode::Shared, CallMode::Once}) {
        for (bool send : {false, true}) {
          samples.push_back(ctx.fn({base, p}, base, mode, send));
          samples.push_back(ctx.fn({}, void_type, mode, send));
        }
      }
      samples.push_back(ctx.struct_type(&sd, {base})); samples.push_back(ctx.enum_type(&ed, {base}));
      samples.push_back(ctx.dyn_type(&td, {base}));
    }
  }
  const Type* deep = p;
  for (int i = 0; i < 32; ++i) deep = ctx.ref(deep, i % 2 == 0);
  samples.push_back(deep);
  Subst subst;
  subst.bind(p, integer_type); subst.bind(p, ctx.prim(PrimKind::Long)); subst.bind(q, void_type);
  const size_t before = samples.size();
  for (size_t i = 0; i < before; ++i) samples.push_back(subst.apply(ctx, samples[i]));
  samples.insert(samples.end(), {Subst().apply(ctx, deep), subst.lookup(p), subst.lookup(q)});
  std::string out;
  integer(out, samples.size(), 8); integer(out, ctx.infer_count(), 4);
  for (auto* t : samples) {
    blob(out, t->to_string());
    for (bool flag : {t->is_integer(), t->is_signed_integer(), t->is_float(), t->is_numeric(), t->is_void(), t->is_bool(),
                      t->is_ref(), t->is_error(), t->is_never(), t->is_infer(), t->is_unsized(), t->contains_infer(), t->contains_param()})
      integer(out, flag, 1);
  }
  for (auto* a : samples) for (auto* b : samples) integer(out, a == b, 1);
  return out;
}

int main(int argc, char** argv) {
  if (argc != 4) return 1;
  const unsigned mode = static_cast<unsigned>(std::stoi(argv[3]));
  const auto root = fs::temp_directory_path() / ("synx-types-" + std::to_string(mode) + '-' +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    fs::create_directories(root);
    const fs::path compiler = fs::absolute(argv[1]), source = fs::absolute(argv[2]);
    const auto exe = root / "types-probe.exe", report = root / "actual.bin", log = root / "run.log";
    std::vector<std::string> args = {"build", utf8(source / "selfhost/types_probe.snx"), "-o", utf8(exe)};
    if (mode & 1) args.push_back("--no-regalloc");
    if (mode & 2) args.push_back("--release");
    int rc = run(compiler, args, log); CHECK(rc == 0);
    if (!rc) { rc = run(exe, {utf8(report)}, log); CHECK(rc == 0); }
    if (rc) throw std::runtime_error(read(log));
    auto expected = reference(), actual = read(report);
    CHECK(actual == expected);
    if (actual != expected) {
      std::ofstream(root / "expected.bin", std::ios::binary).write(expected.data(), static_cast<std::streamsize>(expected.size()));
      size_t offset = 0;
      while (offset < expected.size() && offset < actual.size() && expected[offset] == actual[offset]) ++offset;
      throw std::runtime_error("type parity mismatch at byte " + std::to_string(offset));
    }
    std::printf("mode %u: predicates, strings, substitutions, identity matrix; %zu bytes\n", mode, actual.size());
    fs::remove_all(root);
    return synx_test::finish("selfhost_types");
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s\nartifacts: %s\n", e.what(), utf8(root).c_str()); return 1;
  }
}
