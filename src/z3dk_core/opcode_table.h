#ifndef Z3DK_CORE_OPCODE_TABLE_H
#define Z3DK_CORE_OPCODE_TABLE_H

#include <cstdint>

namespace z3dk {

enum class AddrMode {
  kImplied,
  kImmediate8,
  kImmediate16,
  kImmediateM,
  kImmediateX,
  kRelative8,
  kRelative16,
  kDirectPage,
  kDirectPageX,
  kDirectPageY,
  kDirectPageIndirect,
  kDirectPageIndexedIndirect,
  kDirectPageIndirectIndexedY,
  kDirectPageIndirectLong,
  kDirectPageIndirectLongY,
  kStackRelative,
  kStackRelativeIndirectY,
  kAbsolute,
  kAbsoluteX,
  kAbsoluteY,
  kAbsoluteLong,
  kAbsoluteLongX,
  kAbsoluteIndirect,
  kAbsoluteIndexedIndirect,
  kAbsoluteIndirectLong,
  kBlockMove,
};

struct OpcodeInfo {
  const char* mnemonic;
  AddrMode mode;
};

const OpcodeInfo& GetOpcodeInfo(uint8_t opcode);

int OperandSizeBytes(AddrMode mode, int m_width_bytes, int x_width_bytes);

bool IsRelativeMode(AddrMode mode);
bool IsImmediateMMode(AddrMode mode);
bool IsImmediateXMode(AddrMode mode);

}  // namespace z3dk

#endif  // Z3DK_CORE_OPCODE_TABLE_H
