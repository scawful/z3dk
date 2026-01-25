#include "z3dk_core/lint.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <unordered_map>

#include "z3dk_core/opcode_table.h"

namespace z3dk {
namespace {

struct WidthState {
  int m_width = 1;
  int x_width = 1;
  bool m_known = true;
  bool x_known = true;
};

struct SourceIndex {
  std::unordered_map<int, std::string> files;
  std::vector<SourceMapEntry> entries;
};

SourceIndex BuildSourceIndex(const SourceMap& map) {
  SourceIndex index;
  for (const auto& file : map.files) {
    index.files[file.id] = file.path;
  }
  index.entries = map.entries;
  std::sort(index.entries.begin(), index.entries.end(),
            [](const SourceMapEntry& a, const SourceMapEntry& b) {
              if (a.address != b.address) {
                return a.address < b.address;
              }
              return a.line < b.line;
            });
  return index;
}

const SourceMapEntry* FindEntry(const SourceIndex& index, uint32_t address) {
  if (index.entries.empty()) {
    return nullptr;
  }
  auto it = std::upper_bound(index.entries.begin(), index.entries.end(), address,
                             [](uint32_t addr, const SourceMapEntry& entry) {
                               return addr < entry.address;
                             });
  if (it == index.entries.begin()) {
    return nullptr;
  }
  --it;
  return &(*it);
}

void AddDiagnostic(LintResult* out, DiagnosticSeverity severity,
                   const std::string& message, uint32_t address,
                   const SourceIndex& sources) {
  Diagnostic diag;
  diag.severity = severity;
  diag.message = message;
  const SourceMapEntry* entry = FindEntry(sources, address);
  if (entry) {
    auto it = sources.files.find(entry->file_id);
    if (it != sources.files.end()) {
      diag.filename = it->second;
    }
    diag.line = entry->line;
    diag.column = 1;
  }
  out->diagnostics.push_back(std::move(diag));
}

bool IsOrgCollisionEnabled(const LintOptions& options) {
  return options.warn_org_collision;
}

}  // namespace

LintResult RunLint(const AssembleResult& result, const LintOptions& options) {
  LintResult out;
  if (result.rom_data.empty()) {
    return out;
  }

  SourceIndex sources = BuildSourceIndex(result.source_map);

  if (IsOrgCollisionEnabled(options)) {
    struct Range {
      uint32_t start = 0;
      uint32_t end = 0;
    };
    std::vector<Range> ranges;
    ranges.reserve(result.written_blocks.size());
    for (const auto& block : result.written_blocks) {
      if (block.num_bytes <= 0) {
        continue;
      }
      Range range;
      range.start = static_cast<uint32_t>(block.snes_offset);
      range.end = static_cast<uint32_t>(block.snes_offset + block.num_bytes);
      ranges.push_back(range);
    }
    std::sort(ranges.begin(), ranges.end(),
              [](const Range& a, const Range& b) {
                if (a.start != b.start) {
                  return a.start < b.start;
                }
                return a.end < b.end;
              });
    for (size_t i = 1; i < ranges.size(); ++i) {
      const auto& prev = ranges[i - 1];
      const auto& curr = ranges[i];
      if (curr.start < prev.end) {
        std::string message = "ORG collision: overlap between $";
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%06X-$%06X and $%06X-$%06X",
                      prev.start, prev.end - 1, curr.start, curr.end - 1);
        message += buffer;
        AddDiagnostic(&out, DiagnosticSeverity::kError, message, curr.start,
                      sources);
      }
    }
  }

  for (const auto& block : result.written_blocks) {
    if (block.num_bytes <= 0) {
      continue;
    }
    int pc = block.pc_offset;
    int end = block.pc_offset + block.num_bytes;
    if (pc < 0 || end > static_cast<int>(result.rom_data.size())) {
      continue;
    }
    uint32_t snes = static_cast<uint32_t>(block.snes_offset);

    WidthState widths;
    widths.m_width = std::max(1, options.default_m_width_bytes);
    widths.x_width = std::max(1, options.default_x_width_bytes);
    widths.m_known = options.default_m_width_bytes > 0;
    widths.x_known = options.default_x_width_bytes > 0;

    while (pc < end) {
      uint8_t opcode = result.rom_data[pc];
      const OpcodeInfo& info = GetOpcodeInfo(opcode);

      int m_width = widths.m_known ? widths.m_width
                                   : std::max(1, options.default_m_width_bytes);
      int x_width = widths.x_known ? widths.x_width
                                   : std::max(1, options.default_x_width_bytes);
      int operand_size = OperandSizeBytes(info.mode, m_width, x_width);

      if (pc + 1 + operand_size > end) {
        break;
      }

      if (options.warn_unknown_width) {
        if (IsImmediateMMode(info.mode) && !widths.m_known) {
          AddDiagnostic(&out, DiagnosticSeverity::kWarning,
                        "Immediate size depends on M flag (unknown state)",
                        snes, sources);
        }
        if (IsImmediateXMode(info.mode) && !widths.x_known) {
          AddDiagnostic(&out, DiagnosticSeverity::kWarning,
                        "Immediate size depends on X flag (unknown state)",
                        snes, sources);
        }
      }

      if (options.warn_branch_outside_bank && IsRelativeMode(info.mode)) {
        int32_t offset = 0;
        if (info.mode == AddrMode::kRelative8) {
          offset = static_cast<int8_t>(result.rom_data[pc + 1]);
        } else {
          uint16_t lo = result.rom_data[pc + 1];
          uint16_t hi = result.rom_data[pc + 2];
          offset = static_cast<int16_t>((hi << 8) | lo);
        }
        uint32_t base = (snes & 0xFFFF);
        int32_t target = static_cast<int32_t>(base + 1 + operand_size + offset);
        if (target < 0x8000 || target > 0xFFFF) {
          std::string message = "Branch target leaves current bank (target $";
          char buffer[32];
          std::snprintf(buffer, sizeof(buffer), "%04X)", target & 0xFFFF);
          message += buffer;
          AddDiagnostic(&out, DiagnosticSeverity::kWarning, message, snes,
                        sources);
        }
      }

      if (info.mnemonic == std::string("REP") && operand_size == 1) {
        uint8_t mask = result.rom_data[pc + 1];
        if (mask & 0x20) {
          widths.m_width = 2;
          widths.m_known = true;
        }
        if (mask & 0x10) {
          widths.x_width = 2;
          widths.x_known = true;
        }
      } else if (info.mnemonic == std::string("SEP") && operand_size == 1) {
        uint8_t mask = result.rom_data[pc + 1];
        if (mask & 0x20) {
          widths.m_width = 1;
          widths.m_known = true;
        }
        if (mask & 0x10) {
          widths.x_width = 1;
          widths.x_known = true;
        }
      } else if (info.mnemonic == std::string("PLP") ||
                 info.mnemonic == std::string("RTI")) {
        widths.m_known = false;
        widths.x_known = false;
      } else if (info.mnemonic == std::string("XCE")) {
        widths.m_width = 1;
        widths.x_width = 1;
        widths.m_known = true;
        widths.x_known = true;
      }

      pc += 1 + operand_size;
      snes += static_cast<uint32_t>(1 + operand_size);
    }
  }

  return out;
}

}  // namespace z3dk
