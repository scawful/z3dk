#ifndef Z3DK_CORE_EMIT_H
#define Z3DK_CORE_EMIT_H

#include <string>
#include <string_view>
#include <vector>

#include "z3dk_core/assembler.h"

namespace z3dk {

std::string DiagnosticsToJson(const AssembleResult& result);
std::string DiagnosticsListToJson(const std::vector<Diagnostic>& diagnostics,
                                  bool success);
std::string HooksToJson(const AssembleResult& result, const std::string& rom_path);
std::string SourceMapToJson(const SourceMap& map);
std::string SymbolsToMlb(const std::vector<Label>& labels);

bool WriteTextFile(const std::string& path, std::string_view contents,
                   std::string* error);

}  // namespace z3dk

#endif  // Z3DK_CORE_EMIT_H
