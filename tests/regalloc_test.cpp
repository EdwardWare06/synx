#include <functional>

#include "synx/compile.hpp"
#include "synx/axon.hpp"
#include "synx/regalloc.hpp"
#include "test_support.hpp"

using namespace synx;
using namespace synx::codegen;

namespace {

void inspect(const std::string& source, const std::string& name,
             const std::function<void(const ir::Function&, const RegisterAllocation&)>& test) {
  auto c = check_source("registers.snx", source);
  CHECK(!c->sink.has_errors());
  if (c->sink.has_errors()) return;
  CHECK(c->run_lowering());
  if (c->sink.has_errors()) return;
  LayoutTable layouts(c->program, c->types);
  for (auto& f : c->ir_module.functions) {
    if (f->name != name) continue;
    test(*f, allocate_registers(*f, layouts));
    return;
  }
  CHECK(false);
}

int location(const ir::Function& f, const RegisterAllocation& a, const std::string& name) {
  for (size_t i = 0; i < f.locals.size(); ++i)
    if (f.locals[i].name == name) return a.registers[i];
  CHECK(false);
  return -1;
}

void test_loops_and_calls() {
  inspect("int inc(int x) { return x + 1; } int main() { int sum = 0; int i = 0; "
          "while (i < 4) { sum += inc(i); i += 1; } return sum; }", "main",
    [](auto& f, auto& a) {
      CHECK_EQ(a.registers[0], -1);
      int sum = location(f, a, "sum"), i = location(f, a, "i");
      CHECK(sum == RBX || sum >= R12);
      CHECK(i == RBX || i >= R12);
      CHECK(sum != i);
    });
}

void test_addresses() {
  inspect("int main() { int x = 1; &int r = &x; *r = 2; int[2] a = [1, 2]; "
          "usize i = 1; return a[i] + x; }", "main",
    [](auto& f, auto& a) {
      CHECK_EQ(location(f, a, "x"), -1);
      CHECK_EQ(location(f, a, "r"), -1);
      CHECK_EQ(location(f, a, "a"), -1);
      CHECK(location(f, a, "i") >= 0);
    });
}

void test_parameter_interference() {
  inspect("int f(int a, int b, int c) { int x = a + 1; return x + b + c; } "
          "main() { f(1, 2, 3); }", "f",
    [](auto&, auto& a) {
      CHECK(a.registers[1] >= 0);
      CHECK(a.registers[2] >= 0);
      CHECK(a.registers[3] >= 0);
      CHECK(a.registers[1] != a.registers[2]);
      CHECK(a.registers[1] != a.registers[3]);
      CHECK(a.registers[2] != a.registers[3]);
    });
}

void test_pressure_and_drop() {
  inspect("struct D { int x; } impl Drop for D { void drop(&self) { print(\"drop\"); } } "
          "int main() { int a=1; int b=2; int c=3; int d=4; int e=5; int f=6; int g=7; "
          "{ var owned = D { x: 1 }; } return a+b+c+d+e+f+g; }", "main",
    [](auto& f, auto& a) {
      int spilled = 0, saved = 0;
      for (const char* name : {"a", "b", "c", "d", "e", "f", "g"}) {
        int reg = location(f, a, name);
        if (reg < 0) ++spilled;
        else { CHECK(reg == RBX || reg >= R12); ++saved; }
      }
      CHECK(spilled >= 2);
      CHECK(saved > 0);
      CHECK(a.saved.size() <= 5);
    });
}

void test_c_call_scratch() {
  auto c = check_source("c_call.snx",
    "extern \"C\" { int many(int a, int b, int c, int d, int e, int f, "
    "double g, double h, double i, double j, double k, double l, double m, double n); } "
    "int main() { return unsafe { many(1,2,3,4,5,6,1.0,2.0,3.0,4.0,5.0,6.0,7.0,8.0) }; }");
  CHECK(!c->sink.has_errors());
  CHECK(c->run_lowering());
  if (c->sink.has_errors()) return;
  ObjectFile object;
  Axon axon(c->program, c->types, c->ir_module, c->sink, Target{TargetOs::Linux});
  CHECK(axon.generate(object));
  CHECK(!c->sink.has_errors());
}

}  // namespace

int main() {
  test_loops_and_calls();
  test_addresses();
  test_parameter_interference();
  test_pressure_and_drop();
  test_c_call_scratch();
  return synx_test::finish("regalloc_test");
}
