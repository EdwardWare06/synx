// Linear scan allocation for Axon's scalar locals. -1 keeps a stack slot.
#pragma once

#include <vector>

#include "synx/ir.hpp"
#include "synx/layout.hpp"
#include "synx/x64.hpp"

namespace synx::codegen {

struct RegisterAllocation {
  std::vector<int> registers;
  std::vector<Reg> saved;
};

RegisterAllocation allocate_registers(const ir::Function& function, LayoutTable& layouts);

}  // namespace synx::codegen
