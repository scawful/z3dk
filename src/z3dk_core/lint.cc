#include "z3dk_core/lint.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
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

const SourceMapEntry* FindEntryRaw(const SourceIndex& index, uint32_t address) {
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

// Written blocks can report the $80-$FF mirror of a bank while the source map
// holds the $00-$7F address (or the reverse). Prefer an entry in the same bank
// so diagnostics land on the instruction's own line.
const SourceMapEntry* FindEntry(const SourceIndex& index, uint32_t address) {
  const SourceMapEntry* entry = FindEntryRaw(index, address);
  if (entry && (entry->address >> 16) == (address >> 16)) {
    return entry;
  }
  uint32_t mirror = address ^ 0x800000;
  const SourceMapEntry* mirrored = FindEntryRaw(index, mirror);
  if (mirrored && (mirrored->address >> 16) == (mirror >> 16)) {
    return mirrored;
  }
  return entry;
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

bool IsPushMnemonic(std::string_view mn) {
  return mn == "PHA" || mn == "PHX" || mn == "PHY" ||
         mn == "PHB" || mn == "PHK" || mn == "PHP" || mn == "PHD";
}

bool IsPullMnemonic(std::string_view mn) {
  return mn == "PLA" || mn == "PLX" || mn == "PLY" ||
         mn == "PLB" || mn == "PLP" || mn == "PLD";
}

bool IsReturnMnemonic(std::string_view mn) {
  return mn == "RTS" || mn == "RTL" || mn == "RTI";
}

bool IsHookAddress(const std::vector<Hook>& hooks, uint32_t address) {
  for (const auto& hook : hooks) {
    if (address >= hook.address &&
        address < hook.address + std::max(1, hook.size)) {
      return true;
    }
  }
  return false;
}


// ---------------------------------------------------------------------------
// Peephole-rule helpers (unindexed-slot-table, forced-width-truncation,
// spawn-result-unchecked). These rules decode bytes for the operand and
// addressing mode, then read the source line to confirm the instruction is
// code (not data or a mis-decoded width) and to honour suppressions.
// ---------------------------------------------------------------------------

constexpr const char* kRuleUnindexedSlotTable = "unindexed-slot-table";
constexpr const char* kRuleForcedWidth = "forced-width-truncation";
constexpr const char* kRuleSpawnUnchecked = "spawn-result-unchecked";

class SourceText {
 public:
  explicit SourceText(const std::vector<MemoryFile>& overrides) {
    for (const auto& file : overrides) {
      Store(file.path, file.contents);
    }
  }

  // 1-based line; returns nullptr when the file or line is unavailable.
  const std::string* Line(const std::string& path, int line) {
    if (path.empty() || line <= 0) {
      return nullptr;
    }
    auto it = cache_.find(path);
    if (it == cache_.end()) {
      std::ifstream in(path, std::ios::binary);
      std::string contents;
      if (in.is_open()) {
        std::ostringstream ss;
        ss << in.rdbuf();
        contents = ss.str();
      }
      it = Store(path, contents);
    }
    const auto& lines = it->second;
    if (static_cast<size_t>(line) > lines.size()) {
      return nullptr;
    }
    return &lines[static_cast<size_t>(line) - 1];
  }

 private:
  std::unordered_map<std::string, std::vector<std::string>>::iterator Store(
      const std::string& path, const std::string& contents) {
    std::vector<std::string> lines;
    std::string current;
    for (char c : contents) {
      if (c == '\n') {
        if (!current.empty() && current.back() == '\r') {
          current.pop_back();
        }
        lines.push_back(std::move(current));
        current.clear();
      } else {
        current.push_back(c);
      }
    }
    if (!current.empty()) {
      lines.push_back(std::move(current));
    }
    return cache_.insert_or_assign(path, std::move(lines)).first;
  }

  std::unordered_map<std::string, std::vector<std::string>> cache_;
};

std::string ToUpper(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return out;
}

// Splits a source line into code and comment at the first ';' outside quotes.
void SplitComment(const std::string& line, std::string* code,
                  std::string* comment) {
  bool in_double = false;
  bool in_single = false;
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (c == '"' && !in_single) {
      in_double = !in_double;
    } else if (c == '\'' && !in_double) {
      in_single = !in_single;
    } else if (c == ';' && !in_double && !in_single) {
      if (code) *code = line.substr(0, i);
      if (comment) *comment = line.substr(i + 1);
      return;
    }
  }
  if (code) *code = line;
  if (comment) comment->clear();
}

// `; lint: allow <rule>` on the line suppresses that rule. The K3 spec name
// `spawn-unchecked` is accepted as an alias for spawn-result-unchecked.
bool IsSuppressed(const std::string& comment, std::string_view rule) {
  auto pos = comment.find("lint:");
  if (pos == std::string::npos) {
    return false;
  }
  std::string rest = comment.substr(pos + 5);
  auto allow = rest.find("allow");
  if (allow == std::string::npos) {
    return false;
  }
  rest = rest.substr(allow + 5);
  if (rest.find(rule) != std::string::npos) {
    return true;
  }
  if (rule == kRuleSpawnUnchecked &&
      rest.find("spawn-unchecked") != std::string::npos) {
    return true;
  }
  return false;
}

bool IsIdentChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// True when `code` contains `mnemonic` as an instruction token (e.g. "STA",
// "sta.w", "STA.l"), which tells us the decoded bytes come from code.
bool LineHasMnemonic(const std::string& code, std::string_view mnemonic) {
  std::string upper = ToUpper(code);
  size_t pos = 0;
  while ((pos = upper.find(mnemonic, pos)) != std::string::npos) {
    bool left_ok = pos == 0 || !IsIdentChar(upper[pos - 1]);
    size_t after = pos + mnemonic.size();
    bool right_ok = after >= upper.size() || upper[after] == '.' ||
                    std::isspace(static_cast<unsigned char>(upper[after])) ||
                    upper[after] == ':';
    if (left_ok && right_ok) {
      return true;
    }
    pos = after;
  }
  return false;
}

bool InRanges(const std::vector<MemoryRange>& ranges, uint32_t address) {
  for (const auto& range : ranges) {
    if (address >= range.start && address < range.end) {
      return true;
    }
  }
  return false;
}

bool IsTableAccessMnemonic(std::string_view mn) {
  return mn != "JMP" && mn != "JML" && mn != "JSR" && mn != "JSL" &&
         mn != "PEA" && mn != "PEI";
}

bool IsYIndexedMode(AddrMode mode) {
  return mode == AddrMode::kAbsoluteY || mode == AddrMode::kDirectPageY ||
         mode == AddrMode::kDirectPageIndirectIndexedY ||
         mode == AddrMode::kDirectPageIndirectLongY ||
         mode == AddrMode::kStackRelativeIndirectY;
}

bool IsStoreMnemonic(std::string_view mn) {
  return mn == "STA" || mn == "STX" || mn == "STY" || mn == "STZ";
}

bool IsBranchMnemonic(std::string_view mn) {
  return mn == "BRA" || mn == "BRL" || mn == "BEQ" || mn == "BNE" ||
         mn == "BCC" || mn == "BCS" || mn == "BMI" || mn == "BPL" ||
         mn == "BVC" || mn == "BVS" || mn == "PER";
}

bool IsKnownMnemonic(std::string_view upper) {
  static const std::set<std::string, std::less<>> kMnemonics = [] {
    std::set<std::string, std::less<>> out;
    for (int op = 0; op < 256; ++op) {
      out.insert(GetOpcodeInfo(static_cast<uint8_t>(op)).mnemonic);
    }
    return out;
  }();
  return kMnemonics.find(upper) != kMnemonics.end();
}

// Splits code on ':' statement separators outside quotes ("A : B").
std::vector<std::string> SplitStatements(const std::string& code) {
  std::vector<std::string> out;
  std::string current;
  bool in_double = false;
  bool in_single = false;
  for (char c : code) {
    if (c == '"' && !in_single) in_double = !in_double;
    if (c == '\'' && !in_double) in_single = !in_single;
    if (c == ':' && !in_double && !in_single) {
      out.push_back(current);
      current.clear();
      continue;
    }
    current.push_back(c);
  }
  out.push_back(current);
  return out;
}

std::string TrimView(std::string_view text) {
  size_t start = 0;
  while (start < text.size() &&
         std::isspace(static_cast<unsigned char>(text[start]))) {
    ++start;
  }
  size_t end = text.size();
  while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return std::string(text.substr(start, end - start));
}

struct ForcedByteOperand {
  std::string mnemonic;
  std::string operand_text;  // symbol name or hex literal as written
  uint32_t value = 0;
};

// Finds `XXX.b <operand>` statements whose operand resolves above $FF.
std::vector<ForcedByteOperand> FindForcedByteTruncations(
    const std::string& code,
    const std::unordered_map<std::string, uint32_t>& label_values) {
  std::vector<ForcedByteOperand> out;
  for (const auto& raw : SplitStatements(code)) {
    std::string stmt = TrimView(raw);
    if (stmt.size() < 6 || stmt[3] != '.') {
      continue;
    }
    std::string mn = ToUpper(stmt.substr(0, 3));
    if (!IsKnownMnemonic(mn) || IsBranchMnemonic(mn)) {
      continue;
    }
    if (stmt[4] != 'b' && stmt[4] != 'B') {
      continue;
    }
    if (stmt.size() > 5 && !std::isspace(static_cast<unsigned char>(stmt[5]))) {
      continue;
    }
    std::string operand = TrimView(std::string_view(stmt).substr(5));
    if (operand.empty() || operand[0] == '#') {
      continue;
    }
    size_t i = 0;
    while (i < operand.size() && (operand[i] == '(' || operand[i] == '[')) {
      ++i;
    }
    if (i >= operand.size()) {
      continue;
    }
    ForcedByteOperand hit;
    hit.mnemonic = mn;
    if (operand[i] == '$') {
      size_t j = i + 1;
      while (j < operand.size() &&
             std::isxdigit(static_cast<unsigned char>(operand[j]))) {
        ++j;
      }
      if (j == i + 1) {
        continue;
      }
      hit.operand_text = operand.substr(i, j - i);
      hit.value = static_cast<uint32_t>(
          std::strtoul(operand.substr(i + 1, j - i - 1).c_str(), nullptr, 16));
    } else if (std::isalpha(static_cast<unsigned char>(operand[i])) ||
               operand[i] == '_') {
      size_t j = i;
      while (j < operand.size() &&
             (IsIdentChar(operand[j]) || operand[j] == '.')) {
        ++j;
      }
      hit.operand_text = operand.substr(i, j - i);
      auto it = label_values.find(hit.operand_text);
      if (it == label_values.end()) {
        continue;
      }
      hit.value = it->second;
    } else {
      continue;  // defines (!foo), anonymous labels, expressions
    }
    // Low 16 bits above $FF cannot be a direct-page address (D = $0000);
    // $7E00xx-style long addresses stay valid.
    if ((hit.value & 0xFFFF) > 0xFF) {
      out.push_back(std::move(hit));
    }
  }
  return out;
}

struct SourceLineRef {
  std::string path;
  int line = 0;
};

std::optional<SourceLineRef> LineForAddress(const SourceIndex& index,
                                            uint32_t address) {
  const SourceMapEntry* entry = FindEntry(index, address);
  if (!entry) {
    return std::nullopt;
  }
  auto it = index.files.find(entry->file_id);
  if (it == index.files.end()) {
    return std::nullopt;
  }
  return SourceLineRef{it->second, entry->line};
}

// Source line text (code part / comment part) for the instruction at
// `address`. Returns false when the source is unavailable.
bool CodeAndCommentAt(const SourceIndex& index, SourceText* text,
                      uint32_t address, std::string* code,
                      std::string* comment) {
  auto ref = LineForAddress(index, address);
  if (!ref) {
    return false;
  }
  const std::string* line = text->Line(ref->path, ref->line);
  if (!line) {
    return false;
  }
  SplitComment(*line, code, comment);
  return true;
}

void AddRuleDiagnostic(LintResult* out, const char* rule,
                       const std::string& message, uint32_t address,
                       const SourceIndex& sources) {
  AddDiagnostic(out, DiagnosticSeverity::kWarning,
                std::string("[") + rule + "] " + message, address, sources);
}

}  // namespace

std::vector<MemoryRange> AlttpSpriteSlotTableRanges() {
  // 16-slot sprite tables from the vanilla ALTTP RAM map. $0FA0-$0FFF holds
  // singletons (current slot, cached coordinates, freeze flags) and is
  // deliberately outside these ranges.
  return {
      {0x0B58, 0x0B68, "stun timer"},
      {0x0B6B, 0x0B7B, "tile interaction / die"},
      {0x0B89, 0x0B99, "OAM priority / flash"},
      {0x0BA0, 0x0BC0, "bulletproof / special"},
      {0x0BE0, 0x0BF0, "prize"},
      {0x0C9A, 0x0CCA, "room / deflection / drop"},
      {0x0CD2, 0x0CF2, "bump damage / damage taken"},
      {0x0D00, 0x0FA0, "core sprite tables"},
  };
}

void ApplyPeepholeLintConfig(const Config& config, LintOptions* options) {
  if (!options) {
    return;
  }
  if (config.warn_unindexed_slot_table.has_value()) {
    options->warn_unindexed_slot_table = *config.warn_unindexed_slot_table;
  }
  if (config.warn_forced_width_truncation.has_value()) {
    options->warn_forced_width_truncation =
        *config.warn_forced_width_truncation;
  }
  if (config.warn_spawn_unchecked.has_value()) {
    options->warn_spawn_unchecked = *config.warn_spawn_unchecked;
  }
  if (!config.slot_table_ranges.empty()) {
    options->slot_table_ranges = config.slot_table_ranges;
  } else if (config.preset.has_value() && *config.preset == "alttp") {
    options->slot_table_ranges = AlttpSpriteSlotTableRanges();
  }
  if (!config.slot_table_exempt.empty()) {
    options->slot_table_exempt = config.slot_table_exempt;
  }
  if (!config.spawn_routines.empty()) {
    options->spawn_routines = config.spawn_routines;
  }
}

LintResult RunLint(const AssembleResult& result, const LintOptions& options) {
  LintResult out;
  if (result.rom_data.empty()) {
    return out;
  }

  SourceIndex sources = BuildSourceIndex(result.source_map);

  bool check_org_collision = IsOrgCollisionEnabled(options);
  bool check_hooks = options.warn_unauthorized_hook;
  bool check_bank_capacity = options.warn_bank_full_percent > 0;
  if (check_org_collision || check_hooks || check_bank_capacity) {
    struct Range {
      uint32_t start = 0;
      uint32_t end = 0;
    };
    std::vector<Range> ranges;
    if (check_org_collision) {
      ranges.reserve(result.written_blocks.size());
    }
    std::unordered_map<uint8_t, size_t> bank_usage;
    for (const auto& block : result.written_blocks) {
      if (block.num_bytes <= 0) {
        continue;
      }
      Range range;
      range.start = static_cast<uint32_t>(block.snes_offset);
      range.end = static_cast<uint32_t>(block.snes_offset + block.num_bytes);
      if (check_org_collision) {
        ranges.push_back(range);
      }
      if (check_bank_capacity) {
        uint8_t bank = (block.snes_offset >> 16) & 0xFF;
        bank_usage[bank] += block.num_bytes;
      }
      
      // Hook Validation
      if (check_hooks) {
        // Simple heuristic: if it's in a sensitive region (e.g. Bank 0-3 non-freespace)
        // For OoS, sensitive regions are usually fixed code banks.
        // Let's check against known_hooks.
        bool is_known = false;
        // Address is SNES
        uint32_t addr = range.start;
        // Basic check for freespace (e.g. $F00000+ or ends of banks $7000-$7FFF in LoROM)
        bool is_freespace = (addr >= 0xF00000) || ((addr & 0xFFFF) >= 0x8000 && (addr & 0x7FFF) >= 0x7000);
        
        if (!is_freespace) {
          for (const auto& hook : options.known_hooks) {
            if (addr >= hook.address && addr < hook.address + std::max(1, hook.size)) {
              is_known = true;
              break;
            }
          }
          if (!is_known) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "Unauthorized hook at $%06X (not in hooks.json)", addr);
            AddDiagnostic(&out, DiagnosticSeverity::kWarning, buf, addr, sources);
          }
        }
      }
      
    }
    
    // Bank Capacity
    if (check_bank_capacity) {
      for (const auto& pair : bank_usage) {
        double percent = (pair.second / 32768.0) * 100.0; // Assuming LoROM 32KB banks
        if (percent > options.warn_bank_full_percent) {
          char buf[128];
          std::snprintf(buf, sizeof(buf), "Bank $%02X is %d%% full (%zu bytes used)", pair.first, (int)percent, pair.second);
          AddDiagnostic(&out, DiagnosticSeverity::kWarning, buf, (pair.first << 16) | 0x8000, sources);
        }
      }
    }

    if (check_org_collision) {
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
  }

  // Prohibited Memory Ranges (independent of ORG collision)
  if (!options.prohibited_memory_ranges.empty()) {
    for (const auto& block : result.written_blocks) {
      if (block.num_bytes <= 0) {
        continue;
      }
      uint32_t start = static_cast<uint32_t>(block.snes_offset);
      uint32_t end = static_cast<uint32_t>(block.snes_offset + block.num_bytes);
      for (const auto& prohibited : options.prohibited_memory_ranges) {
        // Check for overlap: max(start1, start2) < min(end1, end2)
        if (std::max(start, prohibited.start) < std::min(end, prohibited.end)) {
          std::string message = "Write to prohibited memory range";
          if (!prohibited.reason.empty()) {
             message += ": " + prohibited.reason;
          }
          char buf[128];
          std::snprintf(buf, sizeof(buf), " ($%06X-$%06X overlaps prohibited $%06X-$%06X)",
                        start, end - 1, prohibited.start, prohibited.end - 1);
          message += buf;
          AddDiagnostic(&out, DiagnosticSeverity::kError, message, start, sources);
        }
      }
    }
  }

  // Peephole-rule setup.
  SourceText source_text(options.source_overrides);
  std::unordered_map<std::string, uint32_t> label_values;
  for (const auto& label : result.labels) {
    label_values.emplace(label.name, label.address);
  }
  {
    // Source text names a symbol without its namespace prefix
    // (`SprXSpeed` inside `namespace Oracle` is the label Oracle_SprXSpeed).
    // Alias each "_"-suffix when exactly one address claims it.
    constexpr uint32_t kAmbiguous = 0xFFFFFFFFu;
    std::unordered_map<std::string, uint32_t> suffixes;
    for (const auto& label : result.labels) {
      const std::string& name = label.name;
      for (size_t pos = name.find('_'); pos != std::string::npos;
           pos = name.find('_', pos + 1)) {
        std::string suffix = name.substr(pos + 1);
        if (suffix.size() < 3) {
          continue;
        }
        auto [it, inserted] = suffixes.emplace(suffix, label.address);
        if (!inserted && it->second != label.address) {
          it->second = kAmbiguous;
        }
      }
    }
    for (const auto& [suffix, address] : suffixes) {
      if (address != kAmbiguous) {
        label_values.emplace(suffix, address);  // never overrides exact names
      }
    }
  }
  const bool check_slot_tables = options.warn_unindexed_slot_table &&
                                 !options.slot_table_ranges.empty();
  // Table base address -> label name, for readable K1 messages.
  std::unordered_map<uint32_t, std::string> slot_label_names;
  if (check_slot_tables) {
    for (const auto& label : result.labels) {
      if (label.address > 0xFFFF || label.name.empty() ||
          label.name.find('.') != std::string::npos ||
          !InRanges(options.slot_table_ranges, label.address)) {
        continue;
      }
      auto it = slot_label_names.find(label.address);
      if (it == slot_label_names.end() || label.name < it->second) {
        slot_label_names[label.address] = label.name;
      }
    }
  }
  auto slot_label_for = [&](uint32_t addr16) -> std::string {
    for (uint32_t delta = 0; delta < 16 && delta <= addr16; ++delta) {
      auto it = slot_label_names.find(addr16 - delta);
      if (it != slot_label_names.end()) {
        if (delta == 0) {
          return it->second;
        }
        return it->second + "+" + std::to_string(delta);
      }
    }
    return std::string();
  };
  std::unordered_map<uint32_t, std::string> spawn_targets;
  if (options.warn_spawn_unchecked) {
    for (const auto& name : options.spawn_routines) {
      auto it = label_values.find(name);
      if (it != label_values.end()) {
        spawn_targets[it->second & 0x7FFFFF] = name;
      }
    }
  }
  const bool check_spawn = !spawn_targets.empty();

  for (const auto& block : result.written_blocks) {
    if (block.num_bytes <= 0) {
      continue;
    }
    struct PendingSpawn {
      bool active = false;
      uint32_t address = 0;
      int steps = 0;
      bool cpy_seen = false;
      std::string routine;
    } pending_spawn;
    int pc = block.pc_offset;
    int end = block.pc_offset + block.num_bytes;
    if (pc < 0 || end > static_cast<int>(result.rom_data.size())) {
      continue;
    }
    uint32_t snes = static_cast<uint32_t>(block.snes_offset);
    // Source map addresses may sit in the other half of the $80 mirror.
    uint32_t source_bank_xor = 0;
    {
      const SourceMapEntry* direct = FindEntryRaw(sources, snes);
      if (!direct || (direct->address >> 16) != (snes >> 16)) {
        const SourceMapEntry* mirrored = FindEntryRaw(sources, snes ^ 0x800000);
        if (mirrored && (mirrored->address >> 16) == ((snes ^ 0x800000) >> 16)) {
          source_bank_xor = 0x800000;
        }
      }
    }

    WidthState widths;
    widths.m_width = std::max(1, options.default_m_width_bytes);
    widths.x_width = std::max(1, options.default_x_width_bytes);
    widths.m_known = options.default_m_width_bytes > 0;
    widths.x_known = options.default_x_width_bytes > 0;

    // Stack balance tracking
    int stack_depth = 0;
    uint32_t routine_start = snes;
    uint8_t last_opcode = 0;
    bool block_is_hook = !options.known_hooks.empty() &&
                         IsHookAddress(options.known_hooks,
                                       static_cast<uint32_t>(block.snes_offset));

    while (pc < end) {
      uint8_t opcode = result.rom_data[pc];
      
      // Apply state overrides
      for (const auto& override : options.state_overrides) {
        if (override.address == snes) {
          if (override.m_width > 0) {
            widths.m_width = override.m_width;
            widths.m_known = true;
          }
          if (override.x_width > 0) {
            widths.x_width = override.x_width;
            widths.x_known = true;
          }
        }
      }

      const OpcodeInfo& info = GetOpcodeInfo(opcode);

      int m_width = widths.m_known ? widths.m_width
                                   : std::max(1, options.default_m_width_bytes);
      int x_width = widths.x_known ? widths.x_width
                                   : std::max(1, options.default_x_width_bytes);
      int operand_size = OperandSizeBytes(info.mode, m_width, x_width);

      if (pc + 1 + operand_size > end) {
        break;
      }

      // Resync: if this decode would swallow the first byte of another source
      // line (data tables, width desync), restart at that line instead.
      if (operand_size > 0 && !sources.entries.empty()) {
        uint32_t here = snes ^ source_bank_xor;
        auto next = std::upper_bound(
            sources.entries.begin(), sources.entries.end(), here,
            [](uint32_t addr, const SourceMapEntry& entry) {
              return addr < entry.address;
            });
        if (next != sources.entries.end() &&
            next->address < here + 1 + static_cast<uint32_t>(operand_size)) {
          uint32_t skip = next->address - here;
          pc += static_cast<int>(skip);
          snes += skip;
          continue;
        }
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

      // Stack balance and return type checks
      std::string_view mn(info.mnemonic);
      if (options.warn_stack_balance) {
        if (IsPushMnemonic(mn)) {
          stack_depth++;
        } else if (IsPullMnemonic(mn)) {
          stack_depth--;
          if (stack_depth < 0) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "Stack underflow: more pulls than pushes at $%06X",
                          snes);
            AddDiagnostic(&out, DiagnosticSeverity::kWarning, buf, snes,
                          sources);
            stack_depth = 0;
          }
        }

        if (IsReturnMnemonic(mn) && stack_depth != 0) {
          char buf[128];
          std::snprintf(buf, sizeof(buf),
                        "Stack imbalance at %s ($%06X): net depth %+d "
                        "(routine started at $%06X)",
                        info.mnemonic, snes, stack_depth, routine_start);
          AddDiagnostic(&out, DiagnosticSeverity::kWarning, buf, snes, sources);
        }

        if (IsReturnMnemonic(mn)) {
          stack_depth = 0;
          routine_start = snes + 1 + static_cast<uint32_t>(operand_size);
        }
      }

      if (options.warn_hook_return && block_is_hook && IsReturnMnemonic(mn)) {
        if (mn == "RTS") {
          char buf[128];
          std::snprintf(buf, sizeof(buf),
                        "Hook at $%06X returns with RTS instead of RTL "
                        "(hooks are called via JSL/JML)",
                        static_cast<uint32_t>(block.snes_offset));
          AddDiagnostic(&out, DiagnosticSeverity::kWarning, buf, snes, sources);
        }
      }

      // K1 unindexed-slot-table: absolute access to a per-slot table.
      if (check_slot_tables && IsTableAccessMnemonic(mn) &&
          (info.mode == AddrMode::kAbsolute ||
           info.mode == AddrMode::kAbsoluteLong)) {
        uint32_t operand = result.rom_data[pc + 1] |
                           (static_cast<uint32_t>(result.rom_data[pc + 2]) << 8);
        uint32_t bank = 0;
        if (info.mode == AddrMode::kAbsoluteLong) {
          bank = result.rom_data[pc + 3];
          operand |= bank << 16;
        }
        // Long operands only reach WRAM low memory in $7E or a system bank.
        bool wram = info.mode == AddrMode::kAbsolute || bank == 0x7E ||
                    (bank & 0x7F) < 0x40;
        uint32_t addr16 = operand & 0xFFFF;
        if (wram && InRanges(options.slot_table_ranges, addr16) &&
            !InRanges(options.slot_table_exempt, addr16) &&
            !InRanges(options.slot_table_exempt, operand)) {
          std::string code;
          std::string comment;
          if (CodeAndCommentAt(sources, &source_text, snes, &code, &comment) &&
              LineHasMnemonic(code, mn) &&
              !IsSuppressed(comment, kRuleUnindexedSlotTable)) {
            std::string name = slot_label_for(addr16);
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "%s $%04X%s%s%s reads/writes a per-slot table without "
                          ",X/,Y (only slot 0 is touched)",
                          info.mnemonic, addr16, name.empty() ? "" : " (",
                          name.c_str(), name.empty() ? "" : ")");
            AddRuleDiagnostic(&out, kRuleUnindexedSlotTable, buf, snes,
                              sources);
          }
        }
      }

      // K3 spawn-result-unchecked: Y holds the new slot or $FF on failure.
      if (check_spawn) {
        if (pending_spawn.active) {
          ++pending_spawn.steps;
          bool tested = mn == "BMI" || mn == "BPL" || mn == "BCS" ||
                        mn == "BCC" ||
                        (pending_spawn.cpy_seen && (mn == "BEQ" || mn == "BNE"));
          bool leaves = IsReturnMnemonic(mn) || mn == "JMP" || mn == "JML" ||
                        mn == "BRA" || mn == "BRL";
          if (mn == "CPY") {
            pending_spawn.cpy_seen = true;
          }
          if (tested || leaves || pending_spawn.steps > 48) {
            pending_spawn.active = false;
          } else if (IsStoreMnemonic(mn) && IsYIndexedMode(info.mode)) {
            std::string store_at;
            if (auto ref = LineForAddress(sources, snes)) {
              store_at = " (first ,Y store at line " +
                         std::to_string(ref->line) + ")";
            }
            std::string message =
                "JSL " + pending_spawn.routine +
                " result is used via ,Y before a BMI/BCS test; Y=$FF when "
                "no slot is free" + store_at;
            AddRuleDiagnostic(&out, kRuleSpawnUnchecked, message,
                              pending_spawn.address, sources);
            pending_spawn.active = false;
          }
        }
        if (opcode == 0x22) {
          uint32_t target = result.rom_data[pc + 1] |
                            (static_cast<uint32_t>(result.rom_data[pc + 2]) << 8) |
                            (static_cast<uint32_t>(result.rom_data[pc + 3]) << 16);
          auto it = spawn_targets.find(target & 0x7FFFFF);
          if (it != spawn_targets.end()) {
            std::string code;
            std::string comment;
            bool have_source =
                CodeAndCommentAt(sources, &source_text, snes, &code, &comment);
            pending_spawn = PendingSpawn{};
            if (have_source && LineHasMnemonic(code, "JSL") &&
                !IsSuppressed(comment, kRuleSpawnUnchecked)) {
              pending_spawn.active = true;
              pending_spawn.address = snes;
              pending_spawn.routine = it->second;
            }
          }
        }
      }

      last_opcode = opcode;

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

  // K2 forced-width-truncation: source-level scan of every assembled line.
  if (options.warn_forced_width_truncation) {
    std::set<std::pair<int, int>> seen;
    for (const auto& entry : sources.entries) {
      if (!seen.emplace(entry.file_id, entry.line).second) {
        continue;
      }
      auto file = sources.files.find(entry.file_id);
      if (file == sources.files.end()) {
        continue;
      }
      const std::string* line = source_text.Line(file->second, entry.line);
      if (!line || line->find('.') == std::string::npos) {
        continue;
      }
      std::string code;
      std::string comment;
      SplitComment(*line, &code, &comment);
      if (IsSuppressed(comment, kRuleForcedWidth)) {
        continue;
      }
      for (const auto& hit : FindForcedByteTruncations(code, label_values)) {
        char buf[224];
        std::snprintf(buf, sizeof(buf),
                      "[%s] %s.b %s: value $%04X does not fit in a byte; the "
                      "operand is truncated to direct page $%02X",
                      kRuleForcedWidth, hit.mnemonic.c_str(),
                      hit.operand_text.c_str(), hit.value & 0xFFFF,
                      hit.value & 0xFF);
        Diagnostic diag;
        diag.severity = DiagnosticSeverity::kWarning;
        diag.message = buf;
        diag.filename = file->second;
        diag.line = entry.line;
        diag.column = 1;
        out.diagnostics.push_back(std::move(diag));
      }
    }
  }

  // Unused Symbol Detection
  if (options.warn_unused_symbols) {
    for (const auto& label : result.labels) {
      if (!label.used) {
        // Skip labels that start with _ or contain . (internal/sublabels)
        if (!label.name.empty() && label.name[0] != '_' && label.name.find('.') == std::string::npos) {
          std::string message = "Unused label: " + label.name;
          AddDiagnostic(&out, DiagnosticSeverity::kWarning, message, label.address,
                        sources);
        }
      }
    }
  }

  return out;
}

}  // namespace z3dk
