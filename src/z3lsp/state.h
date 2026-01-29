#ifndef Z3LSP_STATE_H_
#define Z3LSP_STATE_H_

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <optional>
#include <filesystem>
#include "z3dk_core/lint.h"
#include "z3dk_core/config.h"
#include "z3dk_core/assembler.h"

namespace z3lsp {

struct DocumentState {
  std::string uri;
  std::string path;
  std::string text;
  int version = 0;
  std::vector<z3dk::Diagnostic> diagnostics;
  std::vector<z3dk::Label> labels;
  std::vector<z3dk::Define> defines;

  struct SymbolEntry {
    std::string name;
    int kind = 0;
    int line = 0;
    int column = 0;
    std::string detail;
    std::string uri;
    std::vector<std::string> parameters;
  };
  std::vector<SymbolEntry> symbols;
  z3dk::SourceMap source_map;
  std::vector<z3dk::WrittenBlock> written_blocks;

  // O(1) lookup maps (populated from vectors above)
  std::unordered_map<std::string, const z3dk::Label*> label_map;
  std::unordered_map<std::string, const z3dk::Define*> define_map;
  std::unordered_map<uint32_t, std::string> address_to_label_map;

  // Debouncing state
  std::chrono::steady_clock::time_point last_change;
  bool needs_analysis = false;

  void BuildLookupMaps();
};

struct WorkspaceState {
  std::filesystem::path root;
  std::optional<z3dk::Config> config;
  std::optional<std::filesystem::path> config_path;
  std::optional<std::filesystem::path> git_root;
  std::unordered_set<std::string> git_ignored_paths;
  std::unordered_map<std::string, std::vector<DocumentState::SymbolEntry>> symbol_index;
  std::unordered_set<std::string> main_candidates;
  std::unordered_set<std::string> symbol_names;
};

struct IncludeEvent {
  enum class Type {
    kInclude,
    kIncdir,
  };
  Type type = Type::kInclude;
  std::string path;
};

struct ParsedFile {
  std::vector<DocumentState::SymbolEntry> symbols;
  std::vector<IncludeEvent> events;
};

struct CachedParse {
  std::filesystem::file_time_type mtime;
  ParsedFile parsed;
};

struct RomCacheEntry {
  std::filesystem::file_time_type mtime;
  std::vector<uint8_t> data;
};

}  // namespace z3lsp

#endif  // Z3LSP_STATE_H_
