#ifndef Z3DK_CORE_SNES_DIAGNOSTICS_H
#define Z3DK_CORE_SNES_DIAGNOSTICS_H

#include <vector>
#include <string>
#include "assembler.h" // For Diagnostic struct

namespace z3dk {

// Scans text for potential hardware misuse based on SNES quirks.
// Returns a list of diagnostics (warnings/infos).
std::vector<Diagnostic> DiagnoseRegisterQuirks(const std::string& text, const std::string& filename);

} // namespace z3dk

#endif // Z3DK_CORE_SNES_DIAGNOSTICS_H
