#include "logging.h"
#include <fstream>
#include <iostream>
#include <cstdlib>
#include "utils.h"

namespace fs = std::filesystem;

namespace z3lsp {

namespace {
bool g_log_enabled = true;
std::string g_log_path;

// Forward declaration of ResolveConfigPath from utils if not including utils.h
// Actually, I'll include utils.h if needed.
}

std::string DefaultLogPath() {
  static std::string path = []() {
    std::string dir;
    try {
      dir = fs::temp_directory_path().string();
    } catch (...) {
      const char* tmp = std::getenv("TMPDIR");
      if (!tmp || !*tmp) {
        tmp = std::getenv("TEMP");
      }
      if (!tmp || !*tmp) {
        tmp = std::getenv("TMP");
      }
      if (tmp && *tmp) {
        dir = tmp;
      }
    }
    if (dir.empty()) {
      dir = "/tmp";
    }
    return (fs::path(dir) / "z3lsp.log").string();
  }();
  return path;
}

void Log(const std::string& msg) {
  if (!g_log_enabled) {
    return;
  }
  const std::string& resolved_path = g_log_path.empty() ? DefaultLogPath() : g_log_path;
  static std::string current_path;
  static std::ofstream log_file;
  if (resolved_path != current_path) {
    if (log_file.is_open()) {
      log_file.close();
    }
    log_file.clear();
    log_file.open(resolved_path, std::ios::app);
    current_path = resolved_path;
  }
  if (log_file.is_open()) {
    log_file << msg << std::endl;
  }
}

void UpdateLspLogConfig(const z3dk::Config& config,
                        const fs::path& config_dir,
                        const fs::path& workspace_root) {
  if (config.lsp_log_enabled.has_value()) {
    g_log_enabled = *config.lsp_log_enabled;
  }
  if (config.lsp_log_path.has_value()) {
    fs::path resolved = ResolveConfigPath(*config.lsp_log_path, config_dir, workspace_root);
    g_log_path = resolved.empty() ? std::string() : resolved.string();
  }
}

}  // namespace z3lsp
