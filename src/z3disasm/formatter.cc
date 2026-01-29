#include "formatter.h"
#include "utils.h"
#include <sstream>
#include <algorithm>

namespace z3disasm {

std::string FormatOperand(const z3dk::OpcodeInfo& info, const uint8_t* data,
                          uint32_t snes, int m_width, int x_width,
                          const LabelIndex& labels) {
  auto label_for = [&](uint32_t address) -> std::optional<std::string> {
    auto it = labels.labels.find(address);
    if (it == labels.labels.end() || it->second.empty()) {
      uint32_t mirror = address ^ 0x800000;
      it = labels.labels.find(mirror);
      if (it == labels.labels.end() || it->second.empty()) {
        return std::nullopt;
      }
    }
    return it->second.front();
  };
  auto label_for_wram = [&](uint16_t value) -> std::optional<std::string> {
    uint32_t addr7e = 0x7E0000 | value;
    if (auto label = label_for(addr7e)) {
      return label;
    }
    uint32_t addr7f = 0x7F0000 | value;
    return label_for(addr7f);
  };

  switch (info.mode) {
    case z3dk::AddrMode::kImmediate8:
      return std::string("#") + Hex(data[0], 2);
    case z3dk::AddrMode::kImmediate16:
      return std::string("#") +
             Hex(static_cast<uint32_t>(data[0] | (data[1] << 8)), 4);
    case z3dk::AddrMode::kImmediateM: {
      int width = std::max(1, m_width);
      uint32_t value = data[0];
      if (width == 2) {
        value |= static_cast<uint32_t>(data[1] << 8);
      }
      return std::string("#") + Hex(value, width * 2);
    }
    case z3dk::AddrMode::kImmediateX: {
      int width = std::max(1, x_width);
      uint32_t value = data[0];
      if (width == 2) {
        value |= static_cast<uint32_t>(data[1] << 8);
      }
      return std::string("#") + Hex(value, width * 2);
    }
    case z3dk::AddrMode::kRelative8: {
      int8_t rel = static_cast<int8_t>(data[0]);
      uint32_t target = (snes & 0xFF0000) |
                        static_cast<uint16_t>((snes + 2 + rel) & 0xFFFF);
      if (auto label = label_for(target)) {
        return *label;
      }
      return Hex(target, 6);
    }
    case z3dk::AddrMode::kRelative16: {
      int16_t rel = static_cast<int16_t>(data[0] | (data[1] << 8));
      uint32_t target = (snes & 0xFF0000) |
                        static_cast<uint16_t>((snes + 3 + rel) & 0xFFFF);
      if (auto label = label_for(target)) {
        return *label;
      }
      return Hex(target, 6);
    }
    case z3dk::AddrMode::kDirectPage:
      return Hex(data[0], 2);
    case z3dk::AddrMode::kDirectPageX:
      return Hex(data[0], 2) + ",X";
    case z3dk::AddrMode::kDirectPageY:
      return Hex(data[0], 2) + ",Y";
    case z3dk::AddrMode::kDirectPageIndirect:
      return "(" + Hex(data[0], 2) + ")";
    case z3dk::AddrMode::kDirectPageIndexedIndirect:
      return "(" + Hex(data[0], 2) + ",X)";
    case z3dk::AddrMode::kDirectPageIndirectIndexedY:
      return "(" + Hex(data[0], 2) + "),Y";
    case z3dk::AddrMode::kDirectPageIndirectLong:
      return "[" + Hex(data[0], 2) + "]";
    case z3dk::AddrMode::kDirectPageIndirectLongY:
      return "[" + Hex(data[0], 2) + "],Y";
    case z3dk::AddrMode::kStackRelative:
      return Hex(data[0], 2) + ",S";
    case z3dk::AddrMode::kStackRelativeIndirectY:
      return "(" + Hex(data[0], 2) + ",S),Y";
    case z3dk::AddrMode::kAbsolute: {
      uint32_t value = data[0] | (data[1] << 8);
      uint32_t addr = (snes & 0xFF0000) | value;
      if (auto label = label_for(addr)) {
        return *label;
      }
      if (auto label = label_for_wram(static_cast<uint16_t>(value))) {
        return *label;
      }
      return Hex(value, 4);
    }
    case z3dk::AddrMode::kAbsoluteX: {
      uint32_t value = data[0] | (data[1] << 8);
      uint32_t addr = (snes & 0xFF0000) | value;
      if (auto label = label_for(addr)) {
        return *label + ",X";
      }
      if (auto label = label_for_wram(static_cast<uint16_t>(value))) {
        return *label + ",X";
      }
      return Hex(value, 4) + ",X";
    }
    case z3dk::AddrMode::kAbsoluteY: {
      uint32_t value = data[0] | (data[1] << 8);
      uint32_t addr = (snes & 0xFF0000) | value;
      if (auto label = label_for(addr)) {
        return *label + ",Y";
      }
      if (auto label = label_for_wram(static_cast<uint16_t>(value))) {
        return *label + ",Y";
      }
      return Hex(value, 4) + ",Y";
    }
    case z3dk::AddrMode::kAbsoluteLong: {
      uint32_t value = data[0] | (data[1] << 8) | (data[2] << 16);
      if (auto label = label_for(value)) {
        return *label;
      }
      return Hex(value, 6);
    }
    case z3dk::AddrMode::kAbsoluteLongX: {
      uint32_t value = data[0] | (data[1] << 8) | (data[2] << 16);
      if (auto label = label_for(value)) {
        return *label + ",X";
      }
      return Hex(value, 6) + ",X";
    }
    case z3dk::AddrMode::kAbsoluteIndirect: {
      uint32_t value = data[0] | (data[1] << 8);
      return "(" + Hex(value, 4) + ")";
    }
    case z3dk::AddrMode::kAbsoluteIndexedIndirect: {
      uint32_t value = data[0] | (data[1] << 8);
      return "(" + Hex(value, 4) + ",X)";
    }
    case z3dk::AddrMode::kAbsoluteIndirectLong: {
      uint32_t value = data[0] | (data[1] << 8);
      return "[" + Hex(value, 4) + "]";
    }
    case z3dk::AddrMode::kBlockMove: {
      std::string dest = Hex(data[0], 2);
      std::string src = Hex(data[1], 2);
      return dest + "," + src;
    }
    case z3dk::AddrMode::kImplied:
    default:
      return "";
  }
}

void EmitHookComment(std::ostream& out, const HookEntry& hook) {
  out << "; HOOK";
  if (!hook.name.empty()) {
    out << " " << hook.name;
  }
  if (!hook.kind.empty()) {
    out << " [" << hook.kind << "]";
  }
  if (!hook.target.empty()) {
    out << " -> " << hook.target;
  }
  if (!hook.source.empty()) {
    out << " (" << hook.source << ")";
  }
  if (!hook.module.empty()) {
    out << " module=" << hook.module;
  }
  if (!hook.abi_class.empty()) {
    out << " abi=" << hook.abi_class;
  }
  if (hook.expected_m == 8 || hook.expected_m == 16) {
    out << " m=" << hook.expected_m;
  }
  if (hook.expected_x == 8 || hook.expected_x == 16) {
    out << " x=" << hook.expected_x;
  }
  if (hook.skip_abi) {
    out << " skip_abi";
  }
  if (hook.size > 0) {
    out << " size=" << hook.size;
  }
  if (!hook.note.empty()) {
    out << " ; " << hook.note;
  }
  out << "\n";
}

}  // namespace z3disasm
