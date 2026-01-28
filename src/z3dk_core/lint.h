#ifndef Z3DK_CORE_LINT_H
#define Z3DK_CORE_LINT_H

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

}  // namespace z3dk

#endif  // Z3DK_CORE_LINT_H
