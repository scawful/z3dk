#include "z3dk_core/opcode_table.h"

#include <array>

namespace z3dk {
namespace {

constexpr std::array<OpcodeInfo, 256> kOpcodeTable = {{
  {"BRK", AddrMode::kImmediate8}, // 0x00
  {"ORA", AddrMode::kDirectPageIndexedIndirect}, // 0x01
  {"COP", AddrMode::kImmediate8}, // 0x02
  {"ORA", AddrMode::kStackRelative}, // 0x03
  {"TSB", AddrMode::kDirectPage}, // 0x04
  {"ORA", AddrMode::kDirectPage}, // 0x05
  {"ASL", AddrMode::kDirectPage}, // 0x06
  {"ORA", AddrMode::kDirectPageIndirectLong}, // 0x07
  {"PHP", AddrMode::kImplied}, // 0x08
  {"ORA", AddrMode::kImmediateM}, // 0x09
  {"ASL", AddrMode::kImplied}, // 0x0A
  {"PHD", AddrMode::kImplied}, // 0x0B
  {"TSB", AddrMode::kAbsolute}, // 0x0C
  {"ORA", AddrMode::kAbsolute}, // 0x0D
  {"ASL", AddrMode::kAbsolute}, // 0x0E
  {"ORA", AddrMode::kAbsoluteLong}, // 0x0F
  {"BPL", AddrMode::kRelative8}, // 0x10
  {"ORA", AddrMode::kDirectPageIndirectIndexedY}, // 0x11
  {"ORA", AddrMode::kDirectPageIndirect}, // 0x12
  {"ORA", AddrMode::kStackRelativeIndirectY}, // 0x13
  {"TRB", AddrMode::kDirectPage}, // 0x14
  {"ORA", AddrMode::kDirectPageX}, // 0x15
  {"ASL", AddrMode::kDirectPageX}, // 0x16
  {"ORA", AddrMode::kDirectPageIndirectLongY}, // 0x17
  {"CLC", AddrMode::kImplied}, // 0x18
  {"ORA", AddrMode::kAbsoluteY}, // 0x19
  {"INC", AddrMode::kImplied}, // 0x1A
  {"TCS", AddrMode::kImplied}, // 0x1B
  {"TRB", AddrMode::kAbsolute}, // 0x1C
  {"ORA", AddrMode::kAbsoluteX}, // 0x1D
  {"ASL", AddrMode::kAbsoluteX}, // 0x1E
  {"ORA", AddrMode::kAbsoluteLongX}, // 0x1F
  {"JSR", AddrMode::kAbsolute}, // 0x20
  {"AND", AddrMode::kDirectPageIndexedIndirect}, // 0x21
  {"JSL", AddrMode::kAbsoluteLong}, // 0x22
  {"AND", AddrMode::kStackRelative}, // 0x23
  {"BIT", AddrMode::kDirectPage}, // 0x24
  {"AND", AddrMode::kDirectPage}, // 0x25
  {"ROL", AddrMode::kDirectPage}, // 0x26
  {"AND", AddrMode::kDirectPageIndirectLong}, // 0x27
  {"PLP", AddrMode::kImplied}, // 0x28
  {"AND", AddrMode::kImmediateM}, // 0x29
  {"ROL", AddrMode::kImplied}, // 0x2A
  {"PLD", AddrMode::kImplied}, // 0x2B
  {"BIT", AddrMode::kAbsolute}, // 0x2C
  {"AND", AddrMode::kAbsolute}, // 0x2D
  {"ROL", AddrMode::kAbsolute}, // 0x2E
  {"AND", AddrMode::kAbsoluteLong}, // 0x2F
  {"BMI", AddrMode::kRelative8}, // 0x30
  {"AND", AddrMode::kDirectPageIndirectIndexedY}, // 0x31
  {"AND", AddrMode::kDirectPageIndirect}, // 0x32
  {"AND", AddrMode::kStackRelativeIndirectY}, // 0x33
  {"BIT", AddrMode::kDirectPageX}, // 0x34
  {"AND", AddrMode::kDirectPageX}, // 0x35
  {"ROL", AddrMode::kDirectPageX}, // 0x36
  {"AND", AddrMode::kDirectPageIndirectLongY}, // 0x37
  {"SEC", AddrMode::kImplied}, // 0x38
  {"AND", AddrMode::kAbsoluteY}, // 0x39
  {"DEC", AddrMode::kImplied}, // 0x3A
  {"TSC", AddrMode::kImplied}, // 0x3B
  {"BIT", AddrMode::kAbsoluteX}, // 0x3C
  {"AND", AddrMode::kAbsoluteX}, // 0x3D
  {"ROL", AddrMode::kAbsoluteX}, // 0x3E
  {"AND", AddrMode::kAbsoluteLongX}, // 0x3F
  {"RTI", AddrMode::kImplied}, // 0x40
  {"EOR", AddrMode::kDirectPageIndexedIndirect}, // 0x41
  {"WDM", AddrMode::kImmediate8}, // 0x42
  {"EOR", AddrMode::kStackRelative}, // 0x43
  {"MVP", AddrMode::kBlockMove}, // 0x44
  {"EOR", AddrMode::kDirectPage}, // 0x45
  {"LSR", AddrMode::kDirectPage}, // 0x46
  {"EOR", AddrMode::kDirectPageIndirectLong}, // 0x47
  {"PHA", AddrMode::kImplied}, // 0x48
  {"EOR", AddrMode::kImmediateM}, // 0x49
  {"LSR", AddrMode::kImplied}, // 0x4A
  {"PHK", AddrMode::kImplied}, // 0x4B
  {"JMP", AddrMode::kAbsolute}, // 0x4C
  {"EOR", AddrMode::kAbsolute}, // 0x4D
  {"LSR", AddrMode::kAbsolute}, // 0x4E
  {"EOR", AddrMode::kAbsoluteLong}, // 0x4F
  {"BVC", AddrMode::kRelative8}, // 0x50
  {"EOR", AddrMode::kDirectPageIndirectIndexedY}, // 0x51
  {"EOR", AddrMode::kDirectPageIndirect}, // 0x52
  {"EOR", AddrMode::kStackRelativeIndirectY}, // 0x53
  {"MVN", AddrMode::kBlockMove}, // 0x54
  {"EOR", AddrMode::kDirectPageX}, // 0x55
  {"LSR", AddrMode::kDirectPageX}, // 0x56
  {"EOR", AddrMode::kDirectPageIndirectLongY}, // 0x57
  {"CLI", AddrMode::kImplied}, // 0x58
  {"EOR", AddrMode::kAbsoluteY}, // 0x59
  {"PHY", AddrMode::kImplied}, // 0x5A
  {"TCD", AddrMode::kImplied}, // 0x5B
  {"JML", AddrMode::kAbsoluteLong}, // 0x5C
  {"EOR", AddrMode::kAbsoluteX}, // 0x5D
  {"LSR", AddrMode::kAbsoluteX}, // 0x5E
  {"EOR", AddrMode::kAbsoluteLongX}, // 0x5F
  {"RTS", AddrMode::kImplied}, // 0x60
  {"ADC", AddrMode::kDirectPageIndexedIndirect}, // 0x61
  {"PER", AddrMode::kRelative16}, // 0x62
  {"ADC", AddrMode::kStackRelative}, // 0x63
  {"STZ", AddrMode::kDirectPage}, // 0x64
  {"ADC", AddrMode::kDirectPage}, // 0x65
  {"ROR", AddrMode::kDirectPage}, // 0x66
  {"ADC", AddrMode::kDirectPageIndirectLong}, // 0x67
  {"PLA", AddrMode::kImplied}, // 0x68
  {"ADC", AddrMode::kImmediateM}, // 0x69
  {"ROR", AddrMode::kImplied}, // 0x6A
  {"RTL", AddrMode::kImplied}, // 0x6B
  {"JMP", AddrMode::kAbsoluteIndirect}, // 0x6C
  {"ADC", AddrMode::kAbsolute}, // 0x6D
  {"ROR", AddrMode::kAbsolute}, // 0x6E
  {"ADC", AddrMode::kAbsoluteLong}, // 0x6F
  {"BVS", AddrMode::kRelative8}, // 0x70
  {"ADC", AddrMode::kDirectPageIndirectIndexedY}, // 0x71
  {"ADC", AddrMode::kDirectPageIndirect}, // 0x72
  {"ADC", AddrMode::kStackRelativeIndirectY}, // 0x73
  {"STZ", AddrMode::kDirectPageX}, // 0x74
  {"ADC", AddrMode::kDirectPageX}, // 0x75
  {"ROR", AddrMode::kDirectPageX}, // 0x76
  {"ADC", AddrMode::kDirectPageIndirectLongY}, // 0x77
  {"SEI", AddrMode::kImplied}, // 0x78
  {"ADC", AddrMode::kAbsoluteY}, // 0x79
  {"PLY", AddrMode::kImplied}, // 0x7A
  {"TDC", AddrMode::kImplied}, // 0x7B
  {"JMP", AddrMode::kAbsoluteIndexedIndirect}, // 0x7C
  {"ADC", AddrMode::kAbsoluteX}, // 0x7D
  {"ROR", AddrMode::kAbsoluteX}, // 0x7E
  {"ADC", AddrMode::kAbsoluteLongX}, // 0x7F
  {"BRA", AddrMode::kRelative8}, // 0x80
  {"STA", AddrMode::kDirectPageIndexedIndirect}, // 0x81
  {"BRL", AddrMode::kRelative16}, // 0x82
  {"STA", AddrMode::kStackRelative}, // 0x83
  {"STY", AddrMode::kDirectPage}, // 0x84
  {"STA", AddrMode::kDirectPage}, // 0x85
  {"STX", AddrMode::kDirectPage}, // 0x86
  {"STA", AddrMode::kDirectPageIndirectLong}, // 0x87
  {"DEY", AddrMode::kImplied}, // 0x88
  {"BIT", AddrMode::kImmediateM}, // 0x89
  {"TXA", AddrMode::kImplied}, // 0x8A
  {"PHB", AddrMode::kImplied}, // 0x8B
  {"STY", AddrMode::kAbsolute}, // 0x8C
  {"STA", AddrMode::kAbsolute}, // 0x8D
  {"STX", AddrMode::kAbsolute}, // 0x8E
  {"STA", AddrMode::kAbsoluteLong}, // 0x8F
  {"BCC", AddrMode::kRelative8}, // 0x90
  {"STA", AddrMode::kDirectPageIndirectIndexedY}, // 0x91
  {"STA", AddrMode::kDirectPageIndirect}, // 0x92
  {"STA", AddrMode::kStackRelativeIndirectY}, // 0x93
  {"STY", AddrMode::kDirectPageX}, // 0x94
  {"STA", AddrMode::kDirectPageX}, // 0x95
  {"STX", AddrMode::kDirectPageY}, // 0x96
  {"STA", AddrMode::kDirectPageIndirectLongY}, // 0x97
  {"TYA", AddrMode::kImplied}, // 0x98
  {"STA", AddrMode::kAbsoluteY}, // 0x99
  {"TXS", AddrMode::kImplied}, // 0x9A
  {"TXY", AddrMode::kImplied}, // 0x9B
  {"STZ", AddrMode::kAbsolute}, // 0x9C
  {"STA", AddrMode::kAbsoluteX}, // 0x9D
  {"STZ", AddrMode::kAbsoluteX}, // 0x9E
  {"STA", AddrMode::kAbsoluteLongX}, // 0x9F
  {"LDY", AddrMode::kImmediateX}, // 0xA0
  {"LDA", AddrMode::kDirectPageIndexedIndirect}, // 0xA1
  {"LDX", AddrMode::kImmediateX}, // 0xA2
  {"LDA", AddrMode::kStackRelative}, // 0xA3
  {"LDY", AddrMode::kDirectPage}, // 0xA4
  {"LDA", AddrMode::kDirectPage}, // 0xA5
  {"LDX", AddrMode::kDirectPage}, // 0xA6
  {"LDA", AddrMode::kDirectPageIndirectLong}, // 0xA7
  {"TAY", AddrMode::kImplied}, // 0xA8
  {"LDA", AddrMode::kImmediateM}, // 0xA9
  {"TAX", AddrMode::kImplied}, // 0xAA
  {"PLB", AddrMode::kImplied}, // 0xAB
  {"LDY", AddrMode::kAbsolute}, // 0xAC
  {"LDA", AddrMode::kAbsolute}, // 0xAD
  {"LDX", AddrMode::kAbsolute}, // 0xAE
  {"LDA", AddrMode::kAbsoluteLong}, // 0xAF
  {"BCS", AddrMode::kRelative8}, // 0xB0
  {"LDA", AddrMode::kDirectPageIndirectIndexedY}, // 0xB1
  {"LDA", AddrMode::kDirectPageIndirect}, // 0xB2
  {"LDA", AddrMode::kStackRelativeIndirectY}, // 0xB3
  {"LDY", AddrMode::kDirectPageX}, // 0xB4
  {"LDA", AddrMode::kDirectPageX}, // 0xB5
  {"LDX", AddrMode::kDirectPageY}, // 0xB6
  {"LDA", AddrMode::kDirectPageIndirectLongY}, // 0xB7
  {"CLV", AddrMode::kImplied}, // 0xB8
  {"LDA", AddrMode::kAbsoluteY}, // 0xB9
  {"TSX", AddrMode::kImplied}, // 0xBA
  {"TYX", AddrMode::kImplied}, // 0xBB
  {"LDY", AddrMode::kAbsoluteX}, // 0xBC
  {"LDA", AddrMode::kAbsoluteX}, // 0xBD
  {"LDX", AddrMode::kAbsoluteY}, // 0xBE
  {"LDA", AddrMode::kAbsoluteLongX}, // 0xBF
  {"CPY", AddrMode::kImmediateX}, // 0xC0
  {"CMP", AddrMode::kDirectPageIndexedIndirect}, // 0xC1
  {"REP", AddrMode::kImmediate8}, // 0xC2
  {"CMP", AddrMode::kStackRelative}, // 0xC3
  {"CPY", AddrMode::kDirectPage}, // 0xC4
  {"CMP", AddrMode::kDirectPage}, // 0xC5
  {"DEC", AddrMode::kDirectPage}, // 0xC6
  {"CMP", AddrMode::kDirectPageIndirectLong}, // 0xC7
  {"INY", AddrMode::kImplied}, // 0xC8
  {"CMP", AddrMode::kImmediateM}, // 0xC9
  {"DEX", AddrMode::kImplied}, // 0xCA
  {"WAI", AddrMode::kImplied}, // 0xCB
  {"CPY", AddrMode::kAbsolute}, // 0xCC
  {"CMP", AddrMode::kAbsolute}, // 0xCD
  {"DEC", AddrMode::kAbsolute}, // 0xCE
  {"CMP", AddrMode::kAbsoluteLong}, // 0xCF
  {"BNE", AddrMode::kRelative8}, // 0xD0
  {"CMP", AddrMode::kDirectPageIndirectIndexedY}, // 0xD1
  {"CMP", AddrMode::kDirectPageIndirect}, // 0xD2
  {"CMP", AddrMode::kStackRelativeIndirectY}, // 0xD3
  {"PEI", AddrMode::kDirectPage}, // 0xD4
  {"CMP", AddrMode::kDirectPageX}, // 0xD5
  {"DEC", AddrMode::kDirectPageX}, // 0xD6
  {"CMP", AddrMode::kDirectPageIndirectLongY}, // 0xD7
  {"CLD", AddrMode::kImplied}, // 0xD8
  {"CMP", AddrMode::kAbsoluteY}, // 0xD9
  {"PHX", AddrMode::kImplied}, // 0xDA
  {"STP", AddrMode::kImplied}, // 0xDB
  {"JML", AddrMode::kAbsoluteIndirectLong}, // 0xDC
  {"CMP", AddrMode::kAbsoluteX}, // 0xDD
  {"DEC", AddrMode::kAbsoluteX}, // 0xDE
  {"CMP", AddrMode::kAbsoluteLongX}, // 0xDF
  {"CPX", AddrMode::kImmediateX}, // 0xE0
  {"SBC", AddrMode::kDirectPageIndexedIndirect}, // 0xE1
  {"SEP", AddrMode::kImmediate8}, // 0xE2
  {"SBC", AddrMode::kStackRelative}, // 0xE3
  {"CPX", AddrMode::kDirectPage}, // 0xE4
  {"SBC", AddrMode::kDirectPage}, // 0xE5
  {"INC", AddrMode::kDirectPage}, // 0xE6
  {"SBC", AddrMode::kDirectPageIndirectLong}, // 0xE7
  {"INX", AddrMode::kImplied}, // 0xE8
  {"SBC", AddrMode::kImmediateM}, // 0xE9
  {"NOP", AddrMode::kImplied}, // 0xEA
  {"XBA", AddrMode::kImplied}, // 0xEB
  {"CPX", AddrMode::kAbsolute}, // 0xEC
  {"SBC", AddrMode::kAbsolute}, // 0xED
  {"INC", AddrMode::kAbsolute}, // 0xEE
  {"SBC", AddrMode::kAbsoluteLong}, // 0xEF
  {"BEQ", AddrMode::kRelative8}, // 0xF0
  {"SBC", AddrMode::kDirectPageIndirectIndexedY}, // 0xF1
  {"SBC", AddrMode::kDirectPageIndirect}, // 0xF2
  {"SBC", AddrMode::kStackRelativeIndirectY}, // 0xF3
  {"PEA", AddrMode::kImmediate16}, // 0xF4
  {"SBC", AddrMode::kDirectPageX}, // 0xF5
  {"INC", AddrMode::kDirectPageX}, // 0xF6
  {"SBC", AddrMode::kDirectPageIndirectLongY}, // 0xF7
  {"SED", AddrMode::kImplied}, // 0xF8
  {"SBC", AddrMode::kAbsoluteY}, // 0xF9
  {"PLX", AddrMode::kImplied}, // 0xFA
  {"XCE", AddrMode::kImplied}, // 0xFB
  {"JSR", AddrMode::kAbsoluteIndexedIndirect}, // 0xFC
  {"SBC", AddrMode::kAbsoluteX}, // 0xFD
  {"INC", AddrMode::kAbsoluteX}, // 0xFE
  {"SBC", AddrMode::kAbsoluteLongX}, // 0xFF
}};

}  // namespace

const OpcodeInfo& GetOpcodeInfo(uint8_t opcode) {
  return kOpcodeTable[opcode];
}

int OperandSizeBytes(AddrMode mode, int m_width_bytes, int x_width_bytes) {
  switch (mode) {
    case AddrMode::kImmediate8:
      return 1;
    case AddrMode::kImmediate16:
      return 2;
    case AddrMode::kImmediateM:
      return m_width_bytes;
    case AddrMode::kImmediateX:
      return x_width_bytes;
    case AddrMode::kRelative8:
      return 1;
    case AddrMode::kRelative16:
      return 2;
    case AddrMode::kDirectPage:
    case AddrMode::kDirectPageX:
    case AddrMode::kDirectPageY:
    case AddrMode::kDirectPageIndirect:
    case AddrMode::kDirectPageIndexedIndirect:
    case AddrMode::kDirectPageIndirectIndexedY:
    case AddrMode::kDirectPageIndirectLong:
    case AddrMode::kDirectPageIndirectLongY:
    case AddrMode::kStackRelative:
    case AddrMode::kStackRelativeIndirectY:
      return 1;
    case AddrMode::kAbsolute:
    case AddrMode::kAbsoluteX:
    case AddrMode::kAbsoluteY:
    case AddrMode::kAbsoluteIndirect:
    case AddrMode::kAbsoluteIndexedIndirect:
    case AddrMode::kAbsoluteIndirectLong:
      return 2;
    case AddrMode::kAbsoluteLong:
    case AddrMode::kAbsoluteLongX:
      return 3;
    case AddrMode::kBlockMove:
      return 2;
    case AddrMode::kImplied:
    default:
      return 0;
  }
}

bool IsRelativeMode(AddrMode mode) {
  return mode == AddrMode::kRelative8 || mode == AddrMode::kRelative16;
}

bool IsImmediateMMode(AddrMode mode) {
  return mode == AddrMode::kImmediateM;
}

bool IsImmediateXMode(AddrMode mode) {
  return mode == AddrMode::kImmediateX;
}

}  // namespace z3dk
