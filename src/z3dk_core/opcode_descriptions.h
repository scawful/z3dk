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
};

inline const std::unordered_map<std::string, OpcodeDescription>& GetOpcodeDescriptions() {
  static const std::unordered_map<std::string, OpcodeDescription> kDescriptions = {
    // Data Transfer
    {"LDA", {"Load Accumulator", "Load memory into accumulator", "N, Z"}},
    {"LDX", {"Load X Register", "Load memory into X register", "N, Z"}},
    {"LDY", {"Load Y Register", "Load memory into Y register", "N, Z"}},
    {"STA", {"Store Accumulator", "Store accumulator to memory", "None"}},
    {"STX", {"Store X Register", "Store X register to memory", "None"}},
    {"STY", {"Store Y Register", "Store Y register to memory", "None"}},
    {"STZ", {"Store Zero", "Store zero to memory", "None"}},

    // Transfer
    {"TAX", {"Transfer A to X", "Copy accumulator to X register", "N, Z"}},
    {"TAY", {"Transfer A to Y", "Copy accumulator to Y register", "N, Z"}},
    {"TXA", {"Transfer X to A", "Copy X register to accumulator", "N, Z"}},
    {"TYA", {"Transfer Y to A", "Copy Y register to accumulator", "N, Z"}},
    {"TXS", {"Transfer X to S", "Copy X register to stack pointer", "None"}},
    {"TSX", {"Transfer S to X", "Copy stack pointer to X register", "N, Z"}},
    {"TXY", {"Transfer X to Y", "Copy X register to Y register", "N, Z"}},
    {"TYX", {"Transfer Y to X", "Copy Y register to X register", "N, Z"}},
    {"TCD", {"Transfer C to D", "Copy 16-bit accumulator to direct page", "N, Z"}},
    {"TDC", {"Transfer D to C", "Copy direct page to 16-bit accumulator", "N, Z"}},
    {"TCS", {"Transfer C to S", "Copy 16-bit accumulator to stack pointer", "None"}},
    {"TSC", {"Transfer S to C", "Copy stack pointer to 16-bit accumulator", "N, Z"}},

    // Stack
    {"PHA", {"Push Accumulator", "Push accumulator onto stack", "None"}},
    {"PHX", {"Push X Register", "Push X register onto stack", "None"}},
    {"PHY", {"Push Y Register", "Push Y register onto stack", "None"}},
    {"PHP", {"Push Processor Status", "Push processor status onto stack", "None"}},
    {"PHB", {"Push Data Bank", "Push data bank register onto stack", "None"}},
    {"PHD", {"Push Direct Page", "Push direct page register onto stack", "None"}},
    {"PHK", {"Push Program Bank", "Push program bank register onto stack", "None"}},
    {"PLA", {"Pull Accumulator", "Pull accumulator from stack", "N, Z"}},
    {"PLX", {"Pull X Register", "Pull X register from stack", "N, Z"}},
    {"PLY", {"Pull Y Register", "Pull Y register from stack", "N, Z"}},
    {"PLP", {"Pull Processor Status", "Pull processor status from stack", "All"}},
    {"PLB", {"Pull Data Bank", "Pull data bank register from stack", "N, Z"}},
    {"PLD", {"Pull Direct Page", "Pull direct page register from stack", "N, Z"}},
    {"PEA", {"Push Effective Address", "Push 16-bit absolute address", "None"}},
    {"PEI", {"Push Effective Indirect", "Push 16-bit indirect address", "None"}},
    {"PER", {"Push Effective Relative", "Push 16-bit PC-relative address", "None"}},

    // Arithmetic
    {"ADC", {"Add with Carry", "Add memory to accumulator with carry", "N, V, Z, C"}},
    {"SBC", {"Subtract with Carry", "Subtract memory from accumulator with borrow", "N, V, Z, C"}},
    {"INC", {"Increment", "Increment accumulator or memory by one", "N, Z"}},
    {"INX", {"Increment X", "Increment X register by one", "N, Z"}},
    {"INY", {"Increment Y", "Increment Y register by one", "N, Z"}},
    {"DEC", {"Decrement", "Decrement accumulator or memory by one", "N, Z"}},
    {"DEX", {"Decrement X", "Decrement X register by one", "N, Z"}},
    {"DEY", {"Decrement Y", "Decrement Y register by one", "N, Z"}},

    // Logical
    {"AND", {"Logical AND", "AND memory with accumulator", "N, Z"}},
    {"ORA", {"Logical OR", "OR memory with accumulator", "N, Z"}},
    {"EOR", {"Exclusive OR", "XOR memory with accumulator", "N, Z"}},
    {"BIT", {"Bit Test", "Test bits in memory with accumulator", "N, V, Z (partial)"}},
    {"TSB", {"Test and Set Bits", "Test and set bits in memory", "Z"}},
    {"TRB", {"Test and Reset Bits", "Test and reset bits in memory", "Z"}},

    // Shift/Rotate
    {"ASL", {"Arithmetic Shift Left", "Shift accumulator or memory left", "N, Z, C"}},
    {"LSR", {"Logical Shift Right", "Shift accumulator or memory right", "N, Z, C"}},
    {"ROL", {"Rotate Left", "Rotate accumulator or memory left through carry", "N, Z, C"}},
    {"ROR", {"Rotate Right", "Rotate accumulator or memory right through carry", "N, Z, C"}},

    // Compare
    {"CMP", {"Compare Accumulator", "Compare memory with accumulator", "N, Z, C"}},
    {"CPX", {"Compare X Register", "Compare memory with X register", "N, Z, C"}},
    {"CPY", {"Compare Y Register", "Compare memory with Y register", "N, Z, C"}},

    // Branch
    {"BCC", {"Branch if Carry Clear", "Branch if C = 0", "None"}},
    {"BCS", {"Branch if Carry Set", "Branch if C = 1", "None"}},
    {"BEQ", {"Branch if Equal", "Branch if Z = 1 (result was zero)", "None"}},
    {"BNE", {"Branch if Not Equal", "Branch if Z = 0 (result was not zero)", "None"}},
    {"BMI", {"Branch if Minus", "Branch if N = 1 (result was negative)", "None"}},
    {"BPL", {"Branch if Plus", "Branch if N = 0 (result was positive)", "None"}},
    {"BVC", {"Branch if Overflow Clear", "Branch if V = 0", "None"}},
    {"BVS", {"Branch if Overflow Set", "Branch if V = 1", "None"}},
    {"BRA", {"Branch Always", "Unconditional relative branch", "None"}},
    {"BRL", {"Branch Long", "Unconditional 16-bit relative branch", "None"}},

    // Jump/Call
    {"JMP", {"Jump", "Jump to address", "None"}},
    {"JML", {"Jump Long", "Jump to 24-bit address", "None"}},
    {"JSR", {"Jump to Subroutine", "Call subroutine (push PC, jump)", "None"}},
    {"JSL", {"Jump to Subroutine Long", "Call subroutine with 24-bit address", "None"}},
    {"RTS", {"Return from Subroutine", "Return from JSR", "None"}},
    {"RTL", {"Return from Subroutine Long", "Return from JSL", "None"}},
    {"RTI", {"Return from Interrupt", "Return from interrupt handler", "All"}},

    // Interrupt
    {"BRK", {"Break", "Software interrupt", "I = 1, D = 0"}},
    {"COP", {"Coprocessor", "Coprocessor interrupt", "I = 1, D = 0"}},
    {"WAI", {"Wait for Interrupt", "Halt CPU until interrupt", "None"}},
    {"STP", {"Stop Processor", "Stop the processor", "None"}},

    // Status Flags
    {"CLC", {"Clear Carry", "Set C = 0", "C = 0"}},
    {"SEC", {"Set Carry", "Set C = 1", "C = 1"}},
    {"CLD", {"Clear Decimal", "Set D = 0 (binary mode)", "D = 0"}},
    {"SED", {"Set Decimal", "Set D = 1 (BCD mode)", "D = 1"}},
    {"CLI", {"Clear Interrupt Disable", "Set I = 0 (enable IRQ)", "I = 0"}},
    {"SEI", {"Set Interrupt Disable", "Set I = 1 (disable IRQ)", "I = 1"}},
    {"CLV", {"Clear Overflow", "Set V = 0", "V = 0"}},
    {"REP", {"Reset Processor Status Bits", "Clear specified status bits", "Selected bits"}},
    {"SEP", {"Set Processor Status Bits", "Set specified status bits", "Selected bits"}},
    {"XCE", {"Exchange Carry and Emulation", "Swap C and E flags", "C, E"}},

    // Block Move
    {"MVN", {"Block Move Negative", "Move block of memory (increment)", "None"}},
    {"MVP", {"Block Move Positive", "Move block of memory (decrement)", "None"}},

    // Misc
    {"NOP", {"No Operation", "Do nothing for one cycle", "None"}},
    {"WDM", {"Reserved", "Reserved for future expansion", "None"}},
    {"XBA", {"Exchange B and A", "Swap high and low bytes of accumulator", "N, Z"}},
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
