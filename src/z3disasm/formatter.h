#ifndef Z3DISASM_FORMATTER_H_
#define Z3DISASM_FORMATTER_H_

#include <string>
#include <iostream>
#include "z3dk_core/opcode_table.h"
#include "symbols.h"
#include "hooks.h"

namespace z3disasm {

std::string FormatOperand(const z3dk::OpcodeInfo& info, const uint8_t* data,
                           uint32_t snes, int m_width, int x_width,
                           const LabelIndex& labels);

void EmitHookComment(std::ostream& out, const HookEntry& hook);

}  // namespace z3disasm

#endif  // Z3DISASM_FORMATTER_H_
