#ifndef Z3DK_CORE_OPCODE_DESCRIPTIONS_H
#define Z3DK_CORE_OPCODE_DESCRIPTIONS_H

#include <string>
#include <unordered_map>

namespace z3dk {

// 65816 opcode descriptions for LSP hover information
struct OpcodeDescription {
  const char* full_name;
  const char* description;
  const char* flags_affected;  // N, V, M, X, D, I, Z, C
  const char* cycles;          // Cycle counts (can be variable)
};

inline const std::unordered_map<std::string, OpcodeDescription>& GetOpcodeDescriptions() {
  static const std::unordered_map<std::string, OpcodeDescription> kDescriptions = {
    // Data Transfer
    {"LDA", {"Load Accumulator", "Load memory into accumulator", "N, Z", "2-6"}},
    {"LDX", {"Load X Register", "Load memory into X register", "N, Z", "2-5"}},
    {"LDY", {"Load Y Register", "Load memory into Y register", "N, Z", "2-5"}},
    {"STA", {"Store Accumulator", "Store accumulator to memory", "None", "3-7"}},
    {"STX", {"Store X Register", "Store X register to memory", "None", "3-5"}},
    {"STY", {"Store Y Register", "Store Y register to memory", "None", "3-5"}},
    {"STZ", {"Store Zero", "Store zero to memory", "None", "3-5"}},

    // Transfer
    {"TAX", {"Transfer A to X", "Copy accumulator to X register", "N, Z", "2"}},
    {"TAY", {"Transfer A to Y", "Copy accumulator to Y register", "N, Z", "2"}},
    {"TXA", {"Transfer X to A", "Copy X register to accumulator", "N, Z", "2"}},
    {"TYA", {"Transfer Y to A", "Copy Y register to accumulator", "N, Z", "2"}},
    {"TXS", {"Transfer X to S", "Copy X register to stack pointer", "None", "2"}},
    {"TSX", {"Transfer S to X", "Copy stack pointer to X register", "N, Z", "2"}},
    {"TXY", {"Transfer X to Y", "Copy X register to Y register", "N, Z", "2"}},
    {"TYX", {"Transfer Y to X", "Copy Y register to X register", "N, Z", "2"}},
    {"TCD", {"Transfer C to D", "Copy 16-bit accumulator to direct page", "N, Z", "2"}},
    {"TDC", {"Transfer D to C", "Copy direct page to 16-bit accumulator", "N, Z", "2"}},
    {"TCS", {"Transfer C to S", "Copy 16-bit accumulator to stack pointer", "None", "2"}},
    {"TSC", {"Transfer S to C", "Copy stack pointer to 16-bit accumulator", "N, Z", "2"}},

    // Stack
    {"PHA", {"Push Accumulator", "Push accumulator onto stack", "None", "3-4"}},
    {"PHX", {"Push X Register", "Push X register onto stack", "None", "3-4"}},
    {"PHY", {"Push Y Register", "Push Y register onto stack", "None", "3-4"}},
    {"PHP", {"Push Processor Status", "Push processor status onto stack", "None", "3"}},
    {"PHB", {"Push Data Bank", "Push data bank register onto stack", "None", "3"}},
    {"PHD", {"Push Direct Page", "Push direct page register onto stack", "None", "4"}},
    {"PHK", {"Push Program Bank", "Push program bank register onto stack", "None", "3"}},
    {"PLA", {"Pull Accumulator", "Pull accumulator from stack", "N, Z", "4-5"}},
    {"PLX", {"Pull X Register", "Pull X register from stack", "N, Z", "4-5"}},
    {"PLY", {"Pull Y Register", "Pull Y register from stack", "N, Z", "4-5"}},
    {"PLP", {"Pull Processor Status", "Pull processor status from stack", "All", "4"}},
    {"PLB", {"Pull Data Bank", "Pull data bank register from stack", "N, Z", "4"}},
    {"PLD", {"Pull Direct Page", "Pull direct page register from stack", "N, Z", "5"}},
    {"PEA", {"Push Effective Address", "Push 16-bit absolute address", "None", "5"}},
    {"PEI", {"Push Effective Indirect", "Push 16-bit indirect address", "None", "6"}},
    {"PER", {"Push Effective Relative", "Push 16-bit PC-relative address", "None", "6"}},

    // Arithmetic
    {"ADC", {"Add with Carry", "Add memory to accumulator with carry", "N, V, Z, C", "2-8"}},
    {"SBC", {"Subtract with Carry", "Subtract memory from accumulator with borrow", "N, V, Z, C", "2-8"}},
    {"INC", {"Increment", "Increment accumulator or memory by one", "N, Z", "2-7"}},
    {"INX", {"Increment X", "Increment X register by one", "N, Z", "2"}},
    {"INY", {"Increment Y", "Increment Y register by one", "N, Z", "2"}},
    {"DEC", {"Decrement", "Decrement accumulator or memory by one", "N, Z", "2-7"}},
    {"DEX", {"Decrement X", "Decrement X register by one", "N, Z", "2"}},
    {"DEY", {"Decrement Y", "Decrement Y register by one", "N, Z", "2"}},

    // Logical
    {"AND", {"Logical AND", "AND memory with accumulator", "N, Z", "2-7"}},
    {"ORA", {"Logical OR", "OR memory with accumulator", "N, Z", "2-7"}},
    {"EOR", {"Exclusive OR", "XOR memory with accumulator", "N, Z", "2-7"}},
    {"BIT", {"Bit Test", "Test bits in memory with accumulator", "N, V, Z (partial)", "2-5"}},
    {"TSB", {"Test and Set Bits", "Test and set bits in memory", "Z", "5-6"}},
    {"TRB", {"Test and Reset Bits", "Test and reset bits in memory", "Z", "5-6"}},

    // Shift/Rotate
    {"ASL", {"Arithmetic Shift Left", "Shift accumulator or memory left", "N, Z, C", "2-7"}},
    {"LSR", {"Logical Shift Right", "Shift accumulator or memory right", "N, Z, C", "2-7"}},
    {"ROL", {"Rotate Left", "Rotate accumulator or memory left through carry", "N, Z, C", "2-7"}},
    {"ROR", {"Rotate Right", "Rotate accumulator or memory right through carry", "N, Z, C", "2-7"}},

    // Compare
    {"CMP", {"Compare Accumulator", "Compare memory with accumulator", "N, Z, C", "2-8"}},
    {"CPX", {"Compare X Register", "Compare memory with X register", "N, Z, C", "2-5"}},
    {"CPY", {"Compare Y Register", "Compare memory with Y register", "N, Z, C", "2-5"}},

    // Branch
    {"BCC", {"Branch if Carry Clear", "Branch if C = 0", "None", "2+"}},
    {"BCS", {"Branch if Carry Set", "Branch if C = 1", "None", "2+"}},
    {"BEQ", {"Branch if Equal", "Branch if Z = 1 (result was zero)", "None", "2+"}},
    {"BNE", {"Branch if Not Equal", "Branch if Z = 0 (result was not zero)", "None", "2+"}},
    {"BMI", {"Branch if Minus", "Branch if N = 1 (result was negative)", "None", "2+"}},
    {"BPL", {"Branch if Plus", "Branch if N = 0 (result was positive)", "None", "2+"}},
    {"BVC", {"Branch if Overflow Clear", "Branch if V = 0", "None", "2+"}},
    {"BVS", {"Branch if Overflow Set", "Branch if V = 1", "None", "2+"}},
    {"BRA", {"Branch Always", "Unconditional relative branch", "None", "3"}},
    {"BRL", {"Branch Long", "Unconditional 16-bit relative branch", "None", "4"}},

    // Jump/Call
    {"JMP", {"Jump", "Jump to address", "None", "3-6"}},
    {"JML", {"Jump Long", "Jump to 24-bit address", "None", "4"}},
    {"JSR", {"Jump to Subroutine", "Call subroutine (push PC, jump)", "None", "6-8"}},
    {"JSL", {"Jump to Subroutine Long", "Call subroutine with 24-bit address", "None", "8"}},
    {"RTS", {"Return from Subroutine", "Return from JSR", "None", "6"}},
    {"RTL", {"Return from Subroutine Long", "Return from JSL", "None", "6"}},
    {"RTI", {"Return from Interrupt", "Return from interrupt handler", "All", "6-7"}},

    // Interrupt
    {"BRK", {"Break", "Software interrupt", "I = 1, D = 0", "7-8"}},
    {"COP", {"Coprocessor", "Coprocessor interrupt", "I = 1, D = 0", "7-8"}},
    {"WAI", {"Wait for Interrupt", "Halt CPU until interrupt", "None", "3+"}},
    {"STP", {"Stop Processor", "Stop the processor", "None", "3"}},

    // Status Flags
    {"CLC", {"Clear Carry", "Set C = 0", "C = 0", "2"}},
    {"SEC", {"Set Carry", "Set C = 1", "C = 1", "2"}},
    {"CLD", {"Clear Decimal", "Set D = 0 (binary mode)", "D = 0", "2"}},
    {"SED", {"Set Decimal", "Set D = 1 (BCD mode)", "D = 1", "2"}},
    {"CLI", {"Clear Interrupt Disable", "Set I = 0 (enable IRQ)", "I = 0", "2"}},
    {"SEI", {"Set Interrupt Disable", "Set I = 1 (disable IRQ)", "I = 1", "2"}},
    {"CLV", {"Clear Overflow", "Set V = 0", "V = 0", "2"}},
    {"REP", {"Reset Processor Status Bits", "Clear specified status bits", "Selected bits", "3"}},
    {"SEP", {"Set Processor Status Bits", "Set specified status bits", "Selected bits", "3"}},
    {"XCE", {"Exchange Carry and Emulation", "Swap C and E flags", "C, E", "2"}},

    // Block Move
    {"MVN", {"Block Move Negative", "Move block of memory (increment)", "None", "7/char"}},
    {"MVP", {"Block Move Positive", "Move block of memory (decrement)", "None", "7/char"}},

    // Misc
    {"NOP", {"No Operation", "Do nothing for one cycle", "None", "2"}},
    {"WDM", {"Reserved", "Reserved for future expansion", "None", "2"}},
    {"XBA", {"Exchange B and A", "Swap high and low bytes of accumulator", "N, Z", "3"}},
  };
  return kDescriptions;
}

inline const char* GetAddrModeDescription(const char* mode_name) {
  static const std::unordered_map<std::string, const char*> kModeDescs = {
    {"Implied", "No operand"},
    {"Immediate8", "#$nn (8-bit immediate)"},
    {"Immediate16", "#$nnnn (16-bit immediate)"},
    {"ImmediateM", "#$nn/#$nnnn (M-width immediate)"},
    {"ImmediateX", "#$nn/#$nnnn (X-width immediate)"},
    {"Relative8", "$nn (8-bit PC-relative)"},
    {"Relative16", "$nnnn (16-bit PC-relative)"},
    {"DirectPage", "$nn (direct page)"},
    {"DirectPageX", "$nn,X (direct page indexed X)"},
    {"DirectPageY", "$nn,Y (direct page indexed Y)"},
    {"DirectPageIndirect", "($nn) (direct page indirect)"},
    {"DirectPageIndexedIndirect", "($nn,X) (indexed indirect)"},
    {"DirectPageIndirectIndexedY", "($nn),Y (indirect indexed)"},
    {"DirectPageIndirectLong", "[$nn] (direct page indirect long)"},
    {"DirectPageIndirectLongY", "[$nn],Y (indirect long indexed)"},
    {"StackRelative", "$nn,S (stack relative)"},
    {"StackRelativeIndirectY", "($nn,S),Y (stack relative indirect indexed)"},
    {"Absolute", "$nnnn (absolute)"},
    {"AbsoluteX", "$nnnn,X (absolute indexed X)"},
    {"AbsoluteY", "$nnnn,Y (absolute indexed Y)"},
    {"AbsoluteLong", "$nnnnnn (24-bit absolute)"},
    {"AbsoluteLongX", "$nnnnnn,X (long indexed X)"},
    {"AbsoluteIndirect", "($nnnn) (absolute indirect)"},
    {"AbsoluteIndexedIndirect", "($nnnn,X) (indexed indirect)"},
    {"AbsoluteIndirectLong", "[$nnnn] (indirect long)"},
    {"BlockMove", "$ss,$dd (source, dest banks)"},
  };
  auto it = kModeDescs.find(mode_name);
  return it != kModeDescs.end() ? it->second : mode_name;
}

}  // namespace z3dk

#endif  // Z3DK_CORE_OPCODE_DESCRIPTIONS_H
