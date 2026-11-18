#ifndef Z3DK_CORE_LINT_H
#define Z3DK_CORE_LINT_H

#include <string>
#include <vector>

#include "z3dk_core/assembler.h"
#include "z3dk_core/config.h"

namespace z3dk {

struct Hook {
  std::string name;
  uint32_t address;
  int size = 0;
};

struct LintOptions {
  int default_m_width_bytes = 1;
  int default_x_width_bytes = 1;
  bool warn_unknown_width = true;
  bool warn_branch_outside_bank = true;
  bool warn_org_collision = true;
  bool warn_unused_symbols = true;
  bool warn_unauthorized_hook = true;
  bool warn_stack_balance = true;
  bool warn_hook_return = true;
  // Peephole rules (K1-K3 from the Oracle QUEUE257 code-smell sweep).
  // unindexed-slot-table: absolute access to a per-slot table without ,X/,Y.
  bool warn_unindexed_slot_table = true;
  // forced-width-truncation: `.b` operand whose value is above $FF.
  bool warn_forced_width_truncation = true;
  // spawn-result-unchecked: spawn call result used via ,Y before a test.
  bool warn_spawn_unchecked = true;
  // Per-slot table address ranges (end exclusive). Empty disables the rule.
  std::vector<MemoryRange> slot_table_ranges;
  // Singleton addresses inside slot_table_ranges that are not per-slot.
  std::vector<MemoryRange> slot_table_exempt;
  // Routine labels whose return value (Y = slot, N/C = failure) must be
  // tested before ,Y stores.
  std::vector<std::string> spawn_routines = {"Sprite_SpawnDynamically"};
  // Source text for unsaved buffers (LSP); other files are read from disk.
  std::vector<MemoryFile> source_overrides;
  int warn_bank_full_percent = 0; // e.g. 95 for 95%
  std::vector<Hook> known_hooks;
  std::vector<MemoryRange> prohibited_memory_ranges;
  
  struct StateOverride {
    uint32_t address;
    int m_width = 0; // 0 = no change
    int x_width = 0; // 0 = no change
  };
  std::vector<StateOverride> state_overrides;
};

struct LintResult {
  std::vector<Diagnostic> diagnostics;

  bool success() const {
    for (const auto& diag : diagnostics) {
      if (diag.severity == DiagnosticSeverity::kError) {
        return false;
      }
    }
    return true;
  }
};

LintResult RunLint(const AssembleResult& result, const LintOptions& options);

// Vanilla ALTTP 16-slot sprite tables in WRAM (used by preset = "alttp" when
// the config does not list slot_table_ranges itself).
std::vector<MemoryRange> AlttpSpriteSlotTableRanges();

// Copies the peephole-rule keys (warn_* toggles, slot_table_ranges,
// slot_table_exempt, spawn_routines) from a z3dk.toml config.
void ApplyPeepholeLintConfig(const Config& config, LintOptions* options);

}  // namespace z3dk

#endif  // Z3DK_CORE_LINT_H
