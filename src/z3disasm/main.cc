#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <algorithm>
#include <filesystem>

#include "utils.h"
#include "options.h"
#include "symbols.h"
#include "hooks.h"
#include "formatter.h"
#include "z3dk_core/opcode_table.h"
#include "z3dk_core/snes_knowledge_base.h"

namespace fs = std::filesystem;
using namespace z3disasm;

int main(int argc, const char* argv[]) {
  Options options;
  if (!ParseArgs(argc, argv, &options)) {
    PrintUsage(argv[0]);
    return 0;
  }
  if (options.rom_path.empty() || options.out_dir.empty()) {
    PrintUsage(argv[0]);
    return 1;
  }
  if (!options.lorom) {
    std::cerr << "Only lorom mapper is supported right now\n";
    return 1;
  }

  if (options.hooks_path.empty()) {
    fs::path candidate = options.rom_path.parent_path() / "hooks.json";
    if (options.hooks_auto || fs::exists(candidate)) {
      options.hooks_path = candidate;
    }
  }

  std::vector<uint8_t> rom;
  if (!ReadFile(options.rom_path, &rom)) {
    std::cerr << "Failed to read ROM: " << options.rom_path << "\n";
    return 1;
  }
  if (rom.size() % 0x8000 == 512) {
    rom.erase(rom.begin(), rom.begin() + 512);
  }
  if (rom.empty()) {
    std::cerr << "ROM is empty after header stripping\n";
    return 1;
  }

  LabelIndex labels;
  if (!options.labels_path.empty() &&
      !LoadSymbols(options.labels_path, &labels)) {
    std::cerr << "Failed to load labels: " << options.labels_path << "\n";
    return 1;
  }
  if (!options.symbols_path.empty() &&
      !LoadSymbols(options.symbols_path, &labels)) {
    std::cerr << "Failed to load symbols: " << options.symbols_path << "\n";
    return 1;
  }

  HookMap hooks;
  std::string hook_error;
  if (!LoadHooks(options.hooks_path, &hooks, &hook_error)) {
    std::cerr << hook_error << "\n";
    return 1;
  }

  fs::create_directories(options.out_dir);

  int total_banks = static_cast<int>((rom.size() + 0x7FFF) / 0x8000);
  int bank_start = std::max(0, options.bank_start);
  int bank_end = options.bank_end >= 0 ? options.bank_end : (total_banks - 1);
  bank_end = std::min(bank_end, total_banks - 1);

  for (int bank = bank_start; bank <= bank_end; ++bank) {
    fs::path out_path = options.out_dir / ("bank_" + Hex(bank, 2).substr(1) + ".asm");
    std::ofstream out(out_path);
    if (!out.is_open()) {
      std::cerr << "Failed to write " << out_path << "\n";
      return 1;
    }

    uint32_t bank_pc = static_cast<uint32_t>(bank * 0x8000);
    uint32_t bank_end_pc = bank_pc + 0x8000;
    if (bank_end_pc > rom.size()) {
      bank_end_pc = static_cast<uint32_t>(rom.size());
    }

    uint32_t snes_base = PcToSnesLoRom(bank_pc);
    out << "; bank " << Hex(bank, 2) << "\n";
    out << "org " << Hex(snes_base, 6) << "\n\n";

    int m_width = std::max(1, options.m_width_bytes);
    int x_width = std::max(1, options.x_width_bytes);

    for (uint32_t pc = bank_pc; pc < bank_end_pc;) {
      uint32_t snes = PcToSnesLoRom(pc);

      auto label_it = labels.labels.find(snes);
      if (label_it == labels.labels.end()) {
        label_it = labels.labels.find(snes ^ 0x800000);
      }
      if (label_it != labels.labels.end()) {
        for (const auto& label : label_it->second) {
          out << label << ":\n";
        }
      }

      auto hook_it = hooks.find(snes);
      if (hook_it == hooks.end()) {
        hook_it = hooks.find(snes ^ 0x800000);
      }
      if (hook_it != hooks.end()) {
        for (const auto& hook : hook_it->second) {
          EmitHookComment(out, hook);
        }
      }

      uint8_t opcode = rom[pc];
      const auto& info = z3dk::GetOpcodeInfo(opcode);
      int operand_size = z3dk::OperandSizeBytes(info.mode, m_width, x_width);
      if (pc + 1 + operand_size > bank_end_pc) {
        out << "  db " << Hex(opcode, 2) << "\n";
        ++pc;
        continue;
      }

      std::string operand;
      if (operand_size > 0) {
        operand = FormatOperand(info, &rom[pc + 1], snes, m_width, x_width,
                                labels);
      }

      out << "  " << info.mnemonic;
      if (!operand.empty()) {
        out << " " << operand;
      }

      // Hardware Register Annotation
      uint32_t target_addr = 0;
      bool has_target = false;
      if (info.mode == z3dk::AddrMode::kAbsolute || 
          info.mode == z3dk::AddrMode::kAbsoluteX || 
          info.mode == z3dk::AddrMode::kAbsoluteY) {
          target_addr = (snes & 0xFF0000) | (rom[pc+1] | (rom[pc+2] << 8));
          has_target = true;
      } else if (info.mode == z3dk::AddrMode::kAbsoluteLong ||
                 info.mode == z3dk::AddrMode::kAbsoluteLongX) {
          target_addr = rom[pc+1] | (rom[pc+2] << 8) | (rom[pc+3] << 16);
          has_target = true;
      } else if (info.mode == z3dk::AddrMode::kDirectPage ||
                 info.mode == z3dk::AddrMode::kDirectPageX ||
                 info.mode == z3dk::AddrMode::kDirectPageY) {
          target_addr = rom[pc+1]; // Assume DP=0 for simple annotation
          has_target = true;
      }

      if (has_target) {
          std::string hw_note = z3dk::SnesKnowledgeBase::GetHardwareAnnotation(target_addr);
          if (!hw_note.empty()) {
              out << " " << hw_note;
          }
      }

      out << "\n";

      if (std::string(info.mnemonic) == "REP" && operand_size == 1) {
        uint8_t mask = rom[pc + 1];
        if (mask & 0x20) {
          m_width = 2;
        }
        if (mask & 0x10) {
          x_width = 2;
        }
      } else if (std::string(info.mnemonic) == "SEP" && operand_size == 1) {
        uint8_t mask = rom[pc + 1];
        if (mask & 0x20) {
          m_width = 1;
        }
        if (mask & 0x10) {
          x_width = 1;
        }
      } else if (std::string(info.mnemonic) == "XCE") {
        m_width = 1;
        x_width = 1;
      }

      pc += 1 + operand_size;
    }
  }

  return 0;
}
