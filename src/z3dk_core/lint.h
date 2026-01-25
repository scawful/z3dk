#ifndef Z3DK_CORE_LINT_H
#define Z3DK_CORE_LINT_H

#include <vector>

#include "z3dk_core/assembler.h"

namespace z3dk {

struct LintOptions {
  int default_m_width_bytes = 1;
  int default_x_width_bytes = 1;
  bool warn_unknown_width = true;
  bool warn_branch_outside_bank = true;
  bool warn_org_collision = true;
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
