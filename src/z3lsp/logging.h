#ifndef Z3LSP_LOGGING_H_
#define Z3LSP_LOGGING_H_

#include <string>
#include <filesystem>
#include "z3dk_core/config.h"

namespace z3lsp {

void Log(const std::string& msg);
void UpdateLspLogConfig(const z3dk::Config& config,
                        const std::filesystem::path& config_dir,
                        const std::filesystem::path& workspace_root);
std::string DefaultLogPath();

}  // namespace z3lsp

#endif  // Z3LSP_LOGGING_H_
