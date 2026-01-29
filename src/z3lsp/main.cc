#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <chrono>
#include <algorithm>
#include <queue>
#include <sstream>
#include <fstream>
#include <iomanip>

#include "nlohmann/json.hpp"
#include "z3dk_core/assembler.h"
#include "z3dk_core/config.h"
#include "z3dk_core/lint.h"
#include "z3dk_core/opcode_descriptions.h"
#include "z3dk_core/opcode_table.h"

#include "logging.h"
#include "utils.h"
#include "state.h"
#include "project_graph.h"
#include "lsp_transport.h"
#include "mesen_client.h"
#include "parser.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace z3lsp {

// Global Project Graph and Mesen client are now in their own modules.
// We use the extern declarations from the headers.

// Forward declarations for functions remaining in main.cc
json BuildDocumentSymbols(const z3lsp::DocumentState& doc);
json BuildWorkspaceSymbols(const z3lsp::WorkspaceState& workspace, const std::string& query);
void PublishDiagnostics(const z3lsp::DocumentState& doc);
z3lsp::DocumentState AnalyzeDocument(const z3lsp::DocumentState& doc, const z3lsp::WorkspaceState& workspace,
                              const std::unordered_map<std::string, z3lsp::DocumentState>* open_documents);
json BuildSemanticTokens(const z3lsp::DocumentState& doc);
std::optional<json> HandleDefinition(const z3lsp::DocumentState& doc, const json& params);
std::optional<json> HandleHover(const z3lsp::DocumentState& doc, const json& params);
std::optional<json> HandleRename(const DocumentState& doc, WorkspaceState& workspace, 
                                 std::unordered_map<std::string, DocumentState>& documents, 
                                 const json& params);
json BuildCompletionItems(const z3lsp::DocumentState& doc, const z3lsp::WorkspaceState& workspace, const std::string& prefix);

// Utility functions remaining in main.cc
std::string SelectRootUri(const std::string& uri, const WorkspaceState& workspace) {
  return z3lsp::g_project_graph.SelectRoot(uri, workspace.main_candidates);
}

// Core LSP logic remains below

void PublishDiagnostics(const z3lsp::DocumentState& doc) {
  json diagnostics = json::array();
  for (const auto& diag : doc.diagnostics) {
    json entry;
    entry["severity"] = diag.severity == z3dk::DiagnosticSeverity::kError ? 1 : 2;
    entry["message"] = diag.message;
    int line = std::max(0, diag.line - 1);
    int column = std::max(0, diag.column - 1);
    entry["range"] = {
        {"start", {{"line", line}, {"character", column}}},
        {"end", {{"line", line}, {"character", column}}},
    };
    diagnostics.push_back(entry);
  }

  json message;
  message["jsonrpc"] = "2.0";
  message["method"] = "textDocument/publishDiagnostics";
  message["params"] = {
      {"uri", doc.uri},
      {"diagnostics", diagnostics},
  };
  z3lsp::SendMessage(message);
}

json BuildDocumentSymbols(const z3lsp::DocumentState& doc) {
  json result = json::array();
  for (const auto& symbol : doc.symbols) {
    if (!symbol.uri.empty() && symbol.uri != doc.uri) {
      continue;
    }
    json entry;
    entry["name"] = symbol.name;
    entry["kind"] = symbol.kind;
    if (!symbol.detail.empty()) {
      entry["detail"] = symbol.detail;
    }
    int line = std::max(0, symbol.line);
    int column = std::max(0, symbol.column);
    int end_column = column + static_cast<int>(symbol.name.size());
    entry["range"] = {
        {"start", {{"line", line}, {"character", column}}},
        {"end", {{"line", line}, {"character", end_column}}},
    };
    entry["selectionRange"] = entry["range"];
    result.push_back(entry);
  }
  return result;
}

json BuildWorkspaceSymbols(const z3lsp::WorkspaceState& workspace,
                           const std::string& query) {
  json result = json::array();
  for (const auto& pair : workspace.symbol_index) {
    const std::string& doc_uri = pair.first;
    for (const auto& symbol : pair.second) {
      if (!z3lsp::ContainsIgnoreCase(symbol.name, query)) {
        continue;
      }
      json entry;
      entry["name"] = symbol.name;
      entry["kind"] = symbol.kind;
      if (!symbol.detail.empty()) {
        entry["containerName"] = symbol.detail;
      }
      std::string uri = symbol.uri.empty() ? doc_uri : symbol.uri;
      if (uri.empty()) {
        continue;
      }
      int line = std::max(0, symbol.line);
      int column = std::max(0, symbol.column);
      int end_column = column + static_cast<int>(symbol.name.size());
      entry["location"] = {
          {"uri", uri},
          {"range",
           {{"start", {{"line", line}, {"character", column}}},
            {"end", {{"line", line}, {"character", end_column}}}}},
      };
      result.push_back(entry);
    }
  }
  return result;
}

z3lsp::DocumentState AnalyzeDocument(const z3lsp::DocumentState& doc,
                              const z3lsp::WorkspaceState& workspace,
                              const std::unordered_map<std::string, z3lsp::DocumentState>* open_documents) {
  z3lsp::DocumentState updated = doc;

  z3dk::Config config;
  fs::path config_dir;
  if (workspace.config.has_value()) {
    config = *workspace.config;
    if (workspace.config_path.has_value()) {
      config_dir = workspace.config_path->parent_path();
    }
  } else {
    fs::path local_config = fs::path(doc.path).parent_path() / "z3dk.toml";
    if (fs::exists(local_config)) {
      config = z3dk::LoadConfigIfExists(local_config.string());
      config_dir = local_config.parent_path();
    }
  }

  z3lsp::UpdateLspLogConfig(config, config_dir, workspace.root);

  std::string root_uri = SelectRootUri(doc.uri, workspace);
  fs::path analysis_root_path = doc.path;
  if (!root_uri.empty()) {
    fs::path candidate = z3lsp::UriToPath(root_uri);
    if (!candidate.empty() && fs::exists(candidate)) {
      analysis_root_path = candidate;
    }
  }
  fs::path analysis_root_dir;
  if (!analysis_root_path.empty()) {
    analysis_root_dir = analysis_root_path.parent_path();
  }
  fs::path doc_path = fs::path(doc.path);
  bool doc_is_root = false;
  if (!analysis_root_path.empty() && !doc_path.empty()) {
    doc_is_root = z3lsp::NormalizePath(analysis_root_path) == z3lsp::NormalizePath(doc_path);
  }

  std::vector<std::string> include_paths;
  if (!config.include_paths.empty()) {
    include_paths = z3lsp::ResolveIncludePaths(config, config_dir);
  }
  if (!analysis_root_dir.empty()) {
    include_paths.push_back(analysis_root_dir.string());
  }
  std::vector<std::string> include_paths_for_index = include_paths;
  std::vector<std::string> include_paths_for_parent_check = include_paths;
  fs::path doc_dir = doc_path.parent_path();
  if (!doc_dir.empty() &&
      std::find(include_paths_for_index.begin(),
                include_paths_for_index.end(),
                doc_dir.string()) == include_paths_for_index.end()) {
    include_paths_for_index.push_back(doc_dir.string());
  }
  std::vector<z3lsp::DocumentState::SymbolEntry> doc_symbols =
      z3lsp::ParseFileText(doc.text, doc.uri).symbols;
  std::unordered_set<std::string> known_symbols = workspace.symbol_names;
  for (const auto& sym : doc_symbols) {
    known_symbols.insert(sym.name);
  }
  if (z3lsp::IsGitIgnoredPath(workspace, doc_path)) {
    updated.symbols = std::move(doc_symbols);
    updated.diagnostics.clear();
    updated.labels.clear();
    updated.defines.clear();
    updated.source_map = z3dk::SourceMap{};
    updated.written_blocks.clear();
    updated.BuildLookupMaps();
    updated.needs_analysis = false;
    return updated;
  }

  z3dk::AssembleOptions options;
  options.patch_path = analysis_root_path.empty() ? doc.path : analysis_root_path.string();
  options.include_paths = std::move(include_paths);
  options.defines.reserve(config.defines.size());
  for (const auto& def : config.defines) {
    auto pos = def.find('=');
    if (pos == std::string::npos) {
      options.defines.emplace_back(def, "");
    } else {
      options.defines.emplace_back(def.substr(0, pos), def.substr(pos + 1));
    }
  }
  if (config.mapper.has_value()) {
    options.defines.emplace_back("z3dk_mapper", *config.mapper);
  }
  if (config.std_includes_path.has_value()) {
    options.std_includes_path = *config.std_includes_path;
  }
  if (config.std_defines_path.has_value()) {
    options.std_defines_path = *config.std_defines_path;
  }
  if (config.rom_path.has_value()) {
    fs::path resolved = z3lsp::ResolveConfigPath(*config.rom_path, config_dir, workspace.root);
    std::vector<uint8_t> rom_data;
    if (z3lsp::LoadRomData(resolved, &rom_data)) {
      options.rom_data = std::move(rom_data);
    }
  }
  if (options.rom_data.empty() && config.rom_size.has_value() && *config.rom_size > 0) {
    options.rom_data.resize(static_cast<size_t>(*config.rom_size), 0);
  }
  if (open_documents) {
    std::unordered_map<std::string, std::string> memory_map;
    for (const auto& entry : *open_documents) {
      if (entry.second.path.empty()) {
        continue;
      }
      memory_map[entry.second.path] = entry.second.text;
    }
    if (!doc.path.empty()) {
      memory_map[doc.path] = doc.text;
    }
    options.memory_files.reserve(memory_map.size());
    for (const auto& entry : memory_map) {
      options.memory_files.push_back({entry.first, entry.second});
    }
  } else if (!doc.path.empty()) {
    options.memory_files.push_back({doc.path, doc.text});
  }

  z3dk::Assembler assembler;
  z3dk::AssembleResult result = assembler.Assemble(options);
  
  z3dk::LintOptions lint_options;
  lint_options.warn_bank_full_percent = 95;
  if (config.warn_unused_symbols.has_value()) {
    lint_options.warn_unused_symbols = *config.warn_unused_symbols;
  } else {
    lint_options.warn_unused_symbols = false;
  }
  if (config.warn_branch_outside_bank.has_value()) {
    lint_options.warn_branch_outside_bank = *config.warn_branch_outside_bank;
  } else {
    lint_options.warn_branch_outside_bank = false;
  }
  if (config.warn_unknown_width.has_value()) {
    lint_options.warn_unknown_width = *config.warn_unknown_width;
  } else {
    lint_options.warn_unknown_width = false;
  }
  if (config.warn_org_collision.has_value()) {
    lint_options.warn_org_collision = *config.warn_org_collision;
  }
  if (config.warn_unauthorized_hook.has_value()) {
    lint_options.warn_unauthorized_hook = *config.warn_unauthorized_hook;
  } else {
    lint_options.warn_unauthorized_hook = false;
  }
  
  // Memory Protection
  if (!config.prohibited_memory_ranges.empty()) {
      lint_options.prohibited_memory_ranges = config.prohibited_memory_ranges;
  }

  // Parse Assume Hints
  {
    std::stringstream ss(doc.text);
    std::string line;
    int line_num = 0;
    while (std::getline(ss, line)) {
        line_num++;
        std::string s_line(line);
        size_t comment_pos = s_line.find(';');
        if (comment_pos != std::string::npos) {
            std::string comment = s_line.substr(comment_pos + 1);
            if (z3lsp::HasPrefixIgnoreCase(z3lsp::Trim(comment), "assume ")) {
                 std::string rest = std::string(z3lsp::Trim(comment)).substr(7); // "assume " length
                 rest = z3lsp::Trim(rest);
                 int m = 0;
                 int x = 0;
                 
                 // rough parsing: "m:8", "x:16", "mx:8"
                 if (rest.find("m:8") != std::string::npos) m = 1;
                 if (rest.find("m:16") != std::string::npos) m = 2;
                 if (rest.find("x:8") != std::string::npos) x = 1;
                 if (rest.find("x:16") != std::string::npos) x = 2;
                 if (rest.find("mx:8") != std::string::npos) { m = 1; x = 1; }
                 if (rest.find("mx:16") != std::string::npos) { m = 2; x = 2; }

                 if (m > 0 || x > 0) {
                     // We need the address of this line.
                     // The AssembleResult only has labels and source map.
                     // Source map maps Address -> Line.
                     // We need Line -> Address.
                     // Scan source map entries for this file and line.
                     // Assuming file_id match.
                     // SourceMap doesn't easily give us the file ID for `doc.path` without searching `files`.
                     // But we can approximate by searching for the approximate PC.
                     // Or, more robustly:
                     for (const auto& entry : result.source_map.entries) {
                         if (entry.line == line_num) {
                              // Verify file path match?
                              // result.source_map.files[entry.file_id].path should match doc.path
                              // This is expensive given the loop structure.
                              // Let's filter result.source_map first.
                              if (result.source_map.files.size() > entry.file_id &&
                                  z3lsp::PathMatchesDocumentPath(result.source_map.files[entry.file_id].path,
                                                          doc_path,
                                                          analysis_root_dir,
                                                          workspace.root)) {
                                  
                                  z3dk::LintOptions::StateOverride override;
                                  override.address = entry.address;
                                  override.m_width = m;
                                  override.x_width = x;
                                  lint_options.state_overrides.push_back(override);
                                  break; // Found the address for this line
                              }
                         }
                     }
                 }
            }
        }
    }
  }
  
  fs::path hooks_json_path = config_dir / "hooks.json";
  if (fs::exists(hooks_json_path)) {
    if (!config.warn_unauthorized_hook.has_value() ||
        (config.warn_unauthorized_hook.has_value() && *config.warn_unauthorized_hook)) {
      lint_options.warn_unauthorized_hook = true;
    }
    try {
      std::ifstream f(hooks_json_path);
      json h = json::parse(f);
      if (h.contains("hooks") && h["hooks"].is_array()) {
        for (const auto& hook : h["hooks"]) {
          z3dk::Hook entry;
          entry.name = hook.value("name", "unknown");
          std::string addr_str = hook.value("address", "0");
          if (addr_str.find("0x") == 0) {
            entry.address = std::stoul(addr_str.substr(2), nullptr, 16);
          } else {
            entry.address = std::stoul(addr_str.substr(0), nullptr, 16);
          }
          entry.size = hook.value("size", 1);
          lint_options.known_hooks.push_back(entry);
        }
      }
    } catch (const std::exception& e) {
      z3lsp::Log("LSP JSON error: " + std::string(e.what()));
    } catch (...) {
      z3lsp::Log("LSP JSON error: unknown exception");
    }
  }

  z3dk::LintResult lint_result = z3dk::RunLint(result, lint_options);
  
  auto filter_diags = [&](const std::vector<z3dk::Diagnostic>& input) {
    std::vector<z3dk::Diagnostic> out;
    out.reserve(input.size());
    for (const auto& diag : input) {
      if (z3lsp::DiagnosticMatchesDocument(diag, doc_path, analysis_root_dir, workspace.root, doc_is_root)) {
        out.push_back(diag);
      }
    }
    return out;
  };

  updated.diagnostics = filter_diags(result.diagnostics);
  std::vector<z3dk::Diagnostic> lint_diags = filter_diags(lint_result.diagnostics);
  updated.diagnostics.insert(updated.diagnostics.end(),
                             lint_diags.begin(),
                             lint_diags.end());
  
  updated.labels = result.labels;
  updated.defines = result.defines;
  updated.source_map = result.source_map;
  updated.written_blocks = result.written_blocks;
  updated.symbols = std::move(doc_symbols);

  if (!known_symbols.empty()) {
    auto should_suppress_missing_label = [&](const z3dk::Diagnostic& diag) {
      if (diag.message.find("Label") == std::string::npos ||
          diag.message.find("wasn't found") == std::string::npos) {
        return false;
      }
      std::string missing = z3lsp::ExtractMissingLabel(diag.message);
      if (missing.empty()) {
        return false;
      }
      if (known_symbols.count(missing)) {
        return true;
      }
      if (missing.find('.') != std::string::npos && known_symbols.count(missing)) {
        return true;
      }
      if (missing.rfind("Oracle_", 0) != 0) {
        std::string oracle_name = "Oracle_" + missing;
        if (known_symbols.count(oracle_name)) {
          return true;
        }
      } else {
        std::string suffix = missing.substr(7);
        if (!suffix.empty() && known_symbols.count(suffix)) {
          return true;
        }
      }
      size_t underscore = missing.find('_');
      if (underscore != std::string::npos && underscore + 1 < missing.size()) {
        std::string suffix = missing.substr(underscore + 1);
        if (known_symbols.count(suffix)) {
          return true;
        }
      }
      return false;
    };

    auto& diags = updated.diagnostics;
    diags.erase(std::remove_if(diags.begin(), diags.end(),
        [&](const z3dk::Diagnostic& d) {
          return should_suppress_missing_label(d);
        }), diags.end());
  }

  // Build O(1) lookup maps
  updated.BuildLookupMaps();
  updated.needs_analysis = false;

  // Error Suppression Heuristic:
  // Suppress "Missing org or freespace command." for include files only when we can see
  // they are included after an org/freespace directive in a parent.
  if (!doc_is_root && !z3lsp::ContainsOrgDirective(doc.text)) {
    bool suppress_missing_org = false;
    auto parents = z3lsp::g_project_graph.GetParents(doc.uri);
    if (!parents.empty()) {
      for (const auto& parent_uri : parents) {
        fs::path parent_path = z3lsp::UriToPath(parent_uri);
        if (!parent_path.empty() && fs::exists(parent_path)) {
          if (z3lsp::ParentIncludesChildAfterOrg(parent_path, doc_path, include_paths_for_parent_check)) {
            suppress_missing_org = true;
            break;
          }
        }
      }
    }

    if (suppress_missing_org) {
      auto& diags = updated.diagnostics;
      diags.erase(std::remove_if(diags.begin(), diags.end(),
          [](const z3dk::Diagnostic& d) {
            return d.message.find("Missing org or freespace command") != std::string::npos;
          }), diags.end());
    }
  }

  return updated;
}

std::optional<json> HandleRename(const DocumentState& doc, WorkspaceState& workspace, 
                                 std::unordered_map<std::string, DocumentState>& documents, 
                                 const json& params) {
  if (!params.contains("textDocument") || !params.contains("position") || !params.contains("newName")) {
    return std::nullopt;
  }
  
  std::string new_name = params["newName"];
  if (new_name.empty()) return std::nullopt;
  
  auto position = params["position"];
  int line = position.value("line", 0);
  int character = position.value("character", 0);
  auto token_opt = z3lsp::ExtractTokenAt(doc.text, line, character);
  if (!token_opt.has_value()) {
    return std::nullopt;
  }
  std::string token = *token_opt;

  // We need to find all occurrences of 'token' in the workspace.
  // This is similar to References, but we return a WorkspaceEdit.
  // We can scan all ASM files in the workspace.

  std::vector<fs::path> files_to_scan;
  if (!workspace.root.empty() && fs::exists(workspace.root) && fs::is_directory(workspace.root)) {
     for (const auto& entry : fs::recursive_directory_iterator(workspace.root)) {
         if (entry.is_regular_file()) {
             auto ext = entry.path().extension();
             if (ext == ".asm" || ext == ".s" || ext == ".inc" || ext == ".a") {
                 if (z3lsp::IsGitIgnoredPath(workspace, entry.path())) {
                     continue;
                 }
                 files_to_scan.push_back(entry.path());
             }
         }
     }
  } else {
      // Fallback: just scan open documents if no workspace root
      for (const auto& pair : documents) {
          files_to_scan.push_back(fs::path(pair.second.path));
      }
  }
  
  // Deduplicate files
  std::sort(files_to_scan.begin(), files_to_scan.end());
  files_to_scan.erase(std::unique(files_to_scan.begin(), files_to_scan.end()), files_to_scan.end());

  json changes = json::object();

  for (const auto& path : files_to_scan) {
      std::string text;
      std::string file_uri = z3lsp::PathToUri(path.string());
      
      // Use in-memory version if available
      if (documents.count(file_uri)) {
          text = documents.at(file_uri).text;
      } else {
          std::ifstream f(path);
          if (f) {
              std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
              text = std::move(content);
          }
      }
      
      if (text.empty()) continue;
      
      json file_edits = json::array();
      
      int current_line = 0;
      int current_col = 0;
      size_t i = 0;
      while (i < text.size()) {
          if (text[i] == '\n') {
              current_line++;
              current_col = 0;
              i++;
              continue;
          }
          
          // Fast check for first char
          if (text[i] == token[0]) {
              // Check full token match
              if (i + token.size() <= text.size() && 
                  text.compare(i, token.size(), token) == 0) {
                   
                   // Check boundaries
                   bool start_ok = (i == 0) || !z3lsp::IsSymbolChar(text[i - 1]);
                   bool end_ok = (i + token.size() == text.size()) || !z3lsp::IsSymbolChar(text[i + token.size()]);
                   
                   if (start_ok && end_ok) {
                       json edit = {
                           {"range", {
                               {"start", {{"line", current_line}, {"character", current_col}}},
                               {"end", {{"line", current_line}, {"character", current_col + (int)token.size()}}}
                           }},
                           {"newText", new_name}
                       };
                       file_edits.push_back(edit);
                       
                       i += token.size();
                       current_col += (int)token.size();
                       continue;
                   }
              }
          }
          i++;
          current_col++;
      }
      
      if (!file_edits.empty()) {
          changes[file_uri] = file_edits;
      }
  }

  json workspace_edit;
  workspace_edit["changes"] = changes;
  return workspace_edit;

}

std::optional<json> HandleDefinition(const DocumentState& doc, const json& params) {
  if (!params.contains("textDocument") || !params.contains("position")) {
    return std::nullopt;
  }
  auto position = params["position"];
  int line = position.value("line", 0);
  int character = position.value("character", 0);
  auto token = z3lsp::ExtractTokenAt(doc.text, line, character);
  if (!token.has_value()) {
    return json(nullptr);
  }

  // Check if we are on an incsrc or incbin line
  std::string line_text;
  size_t start = 0;
  for (int i = 0; i < line; ++i) {
    start = doc.text.find('\n', start);
    if (start == std::string::npos) break;
    start++;
  }
  if (start != std::string::npos) {
    size_t end = doc.text.find('\n', start);
    line_text = doc.text.substr(start, end == std::string::npos ? std::string::npos : end - start);
  }

  std::string trimmed = z3lsp::Trim(z3lsp::StripAsmComment(line_text));
  std::string include_path;
  if (z3lsp::ParseIncludeDirective(trimmed, &include_path) || z3lsp::ParseIncdirDirective(trimmed, &include_path)) {
    // If the cursor is within the quotes of the include path
    size_t quote_start = line_text.find('"');
    size_t quote_end = line_text.find('"', quote_start + 1);
    if (quote_start != std::string::npos && quote_end != std::string::npos &&
        character >= static_cast<int>(quote_start) && character <= static_cast<int>(quote_end)) {
      
      fs::path base_dir = fs::path(doc.path).parent_path();
      fs::path target_path;
      std::vector<std::string> include_paths;
      include_paths.push_back(base_dir.string());
      
      if (z3lsp::ResolveIncludePath(include_path, base_dir, include_paths, &target_path)) {
        json location;
        location["uri"] = z3lsp::PathToUri(target_path.string());
        location["range"] = {
            {"start", {{"line", 0}, {"character", 0}}},
            {"end", {{"line", 0}, {"character", 0}}},
        };
        return json::array({location});
      }
    }
  }

  // O(1) lookup using hash map
  auto label_it = doc.label_map.find(*token);
  if (label_it == doc.label_map.end()) {
    return json(nullptr);
  }
  const z3dk::Label* label_ptr = label_it->second;

  std::unordered_map<int, std::string> file_map;
  for (const auto& file : doc.source_map.files) {
    file_map[file.id] = file.path;
  }

  for (const auto& entry : doc.source_map.entries) {
    if (entry.address != label_ptr->address) {
      continue;
    }
    auto file_it = file_map.find(entry.file_id);
    if (file_it == file_map.end()) {
      continue;
    }
    int target_line = std::max(0, entry.line - 1);
    json location;
    location["uri"] = z3lsp::PathToUri(file_it->second);
    location["range"] = {
        {"start", {{"line", target_line}, {"character", 0}}},
        {"end", {{"line", target_line}, {"character", 0}}},
    };
    return json::array({location});
  }

  return json(nullptr);
}

struct ZeldaRoutineInfo {
  std::string name;
  std::string description;
  std::string expected_state;
};

const std::unordered_map<uint32_t, ZeldaRoutineInfo> kVanillaZeldaKnowledge = {
    {0x008000, {"Reset", "ROM entry point. Initializes the CPU and starts the game engine.", "M=8, X=8"}},
    {0x0080C9, {"NMI_Handler", "V-Blank interrupt handler. Performs DMA transfers and updates PPU registers.", "M=8, X=8"}},
    {0x02C0C3, {"Overworld_SetCameraBounds", "Calculates the scroll boundaries for the current overworld screen based on Link's position.", "M=8, X=8"}},
    {0x099A50, {"Ancilla_AddDamageNumber", "Spawns a damage number ancilla at the specified coordinates.", "M=8, X=8"}},
    {0x0080B5, {"Music_PlayTrack", "Sets the current music track to be played by the APU.", "M=8, X=8"}},
    {0x0791B3, {"Link_ReceiveItem", "Triggers the item receiving sequence for Link, including animations and inventory updates.", "M=8, X=8"}},
    {0x028364, {"BedCutscene_ColorFix", "Initializes palette and screen state for the intro bed cutscene.", "M=8, X=8"}},
    {0x008891, {"APU_SyncWait", "Handshake routine for APU communication. Common point for soft-locks if APU hangs.", "M=8, X=8"}},
    {0x7E0020, {"LinkX", "Link's current X-coordinate in the room/overworld.", "RAM"}},
    {0x7E0022, {"LinkY", "Link's current Y-coordinate in the room/overworld.", "RAM"}},
    {0x7E036C, {"LinkHealth", "Current heart count (in halves).", "RAM"}},
    {0x7E00A0, {"RoomIndex", "The ID of the current dungeon room.", "RAM"}},
};

std::optional<json> HandleHover(const DocumentState& doc, const json& params) {
  if (!params.contains("textDocument") || !params.contains("position")) {
    return std::nullopt;
  }
  auto position = params["position"];
  int line = position.value("line", 0);
  int character = position.value("character", 0);
  auto token = z3lsp::ExtractTokenAt(doc.text, line, character);
  if (!token.has_value()) {
    return json(nullptr);
  }

  // Check if token is a label (O(1) lookup)
  auto label_it = doc.label_map.find(*token);
  if (label_it != doc.label_map.end()) {
    const z3dk::Label* label = label_it->second;
    std::ostringstream hover_text;
    hover_text << label->name << " = $" << std::hex << std::uppercase
               << label->address;

    // Zelda Intelligence: Routine/RAM documentation
    auto zelda_it = kVanillaZeldaKnowledge.find(label->address);
    if (zelda_it != kVanillaZeldaKnowledge.end()) {
      hover_text << "\n\n**Zelda Routine:** " << zelda_it->second.name;
      hover_text << "\n\n" << zelda_it->second.description;
      hover_text << "\n\n**Expects:** " << zelda_it->second.expected_state;
    }

    // Check for live value if it's a RAM address
    uint32_t addr = label->address;
    // Common RAM ranges for SNES/OoS
    bool is_ram = (addr >= 0x7E0000 && addr <= 0x7FFFFF) || 
                  ((addr & 0xFFFF) < 0x2000);
    
    if (is_ram) {
      auto val = z3lsp::g_mesen.ReadByte(addr);
      if (val.has_value()) {
        hover_text << "\n\n**Live Value:** $" << std::hex << std::uppercase 
                   << std::setw(2) << std::setfill('0') << (int)*val;
      }
    }

    json hover;
    hover["contents"] = { {"kind", "markdown"}, {"value", hover_text.str()} };
    return hover;
  }

  // Zelda Intelligence: Raw hex address hover
  if (token->size() >= 2 && (*token)[0] == '$') {
    try {
      uint32_t addr = std::stoul(token->substr(1), nullptr, 16);
      auto zelda_it = kVanillaZeldaKnowledge.find(addr);
      if (zelda_it != kVanillaZeldaKnowledge.end()) {
        std::ostringstream hover_text;
        hover_text << "**" << zelda_it->second.name << "** - $" << std::hex << std::uppercase << addr << "\n\n";
        hover_text << zelda_it->second.description << "\n\n";
        hover_text << "**Expects:** " << zelda_it->second.expected_state;

        json hover;
        hover["contents"] = { {"kind", "markdown"}, {"value", hover_text.str()} };
        return hover;
      }
    } catch (const std::exception& e) {
      z3lsp::Log("LSP JSON error: " + std::string(e.what()));
    } catch (...) {
      z3lsp::Log("LSP JSON error: unknown exception");
    }
  }

  // Check if token is a 65816 opcode (opcode_descs is already a hash map)
  std::string upper_token = *token;
  for (char& c : upper_token) c = std::toupper(c);

  const auto& opcode_descs = z3dk::GetOpcodeDescriptions();
  auto opcode_it = opcode_descs.find(upper_token);
  if (opcode_it != opcode_descs.end()) {
    const auto& desc = opcode_it->second;
    std::ostringstream hover_text;
    hover_text << "**" << upper_token << "** - " << desc.full_name << "\n\n";
    hover_text << desc.description << "\n\n";
    hover_text << "**Flags:** " << desc.flags_affected << "\n\n";
    if (std::string(desc.cycles) != "None") {
      hover_text << "**Cycles:** " << desc.cycles;
    }

    json hover;
    hover["contents"] = { {"kind", "markdown"}, {"value", hover_text.str()} };
    return hover;
  }

  // Check if token is a define (O(1) lookup)
  auto define_it = doc.define_map.find(*token);
  if (define_it != doc.define_map.end()) {
    const z3dk::Define* def = define_it->second;
    std::ostringstream hover_text;
    hover_text << "!" << def->name;
    if (!def->value.empty()) {
      hover_text << " = " << def->value;
    }
    json hover;
    hover["contents"] = { {"kind", "plaintext"}, {"value", hover_text.str()} };
    return hover;
  }

  return json(nullptr);
}

json BuildCompletionItems(const DocumentState& doc, const WorkspaceState& workspace, const std::string& prefix) {
  if (prefix.empty()) {
    return json::array();
  }

  json items = json::array();
  std::unordered_set<std::string> seen;
  auto matches_prefix = [&](std::string_view name) {
    return z3lsp::HasPrefixIgnoreCase(name, prefix);
  };
  auto push_item = [&](const std::string& label, int kind, const std::string& detail) {
    if (!seen.insert(label).second) {
      return;
    }
    json item;
    item["label"] = label;
    item["kind"] = kind;
    if (!detail.empty()) {
      item["detail"] = detail;
    }
    items.push_back(item);
  };

  static const char* const kDirectives[] = {
      "arch", "autoclean", "bank", "bankbyte", "base", "cleartable", "cmode",
      "db", "dw", "dl", "dd", "dq", "define", "elif", "elseif", "else", "endif",
      "endmacro", "endstruct", "endwhile", "endfor", "error", "fill",
      "fillbyte", "fillword", "freecode", "freedata", "freespace", "hirom",
      "if", "incbin", "incgfx", "incmsg", "incsrc", "include", "incdir", "lorom", "exlorom",
      "exhirom", "macro", "namespace", "org", "pad", "padbyte", "padword",
      "pc2snes", "print", "pullpc", "pushpc", "pushns", "popns", "snes2pc",
      "struct", "table", "undef", "warn", "warning", "while", "for",
      "math", "function", "reset", "optimize", "check", "bankcross",
  };

  for (const char* directive : kDirectives) {
    if (!matches_prefix(directive)) {
      continue;
    }
    push_item(directive, 14, "directive");
  }

  // Add workspace-wide symbols
  for (const auto& pair : workspace.symbol_index) {
    for (const auto& symbol : pair.second) {
      if (!matches_prefix(symbol.name)) {
        continue;
      }
      push_item(symbol.name, symbol.kind, symbol.detail);
    }
  }

  for (const auto& label : doc.labels) {
    if (!matches_prefix(label.name)) {
      continue;
    }
    push_item(label.name, 6, "label");
  }

  for (const auto& def : doc.defines) {
    if (!matches_prefix(def.name)) {
      continue;
    }
    std::string detail = def.value.empty() ? "define" : def.value;
    push_item(def.name, 21, detail);
  }

  for (const auto& symbol : doc.symbols) {
    if (symbol.detail != "macro") {
      continue;
    }
    if (!matches_prefix(symbol.name)) {
      continue;
    }
    push_item(symbol.name, 3, "macro");
  }

  static std::vector<std::string> kOpcodes65816;
  if (kOpcodes65816.empty()) {
    std::unordered_set<std::string> names;
    names.reserve(128);
    for (int i = 0; i < 256; ++i) {
      const auto& info = z3dk::GetOpcodeInfo(static_cast<uint8_t>(i));
      if (info.mnemonic != nullptr && info.mnemonic[0] != '\0') {
        names.insert(info.mnemonic);
      }
    }
    kOpcodes65816.assign(names.begin(), names.end());
    std::sort(kOpcodes65816.begin(), kOpcodes65816.end());
  }
  for (const auto& opcode : kOpcodes65816) {
    if (!matches_prefix(opcode)) {
      continue;
    }
    push_item(opcode, 14, "opcode 65816");
  }

  static const char* const kOpcodesSpc700[] = {
      "ADC", "ADDW", "AND", "AND1", "AND1C", "ASL", "BBC", "BBS", "BCC", "BCS",
      "BEQ", "BMI", "BNE", "BPL", "BVC", "BVS", "BRA", "BRK", "CALL", "CBNE",
      "CLR1", "CLRC", "CLRP", "CLRV", "CMP", "CMPW", "DAA", "DAS", "DBNZ",
      "DEC", "DECW", "DI", "DIV", "EI", "EOR", "EOR1", "INC", "INCW", "JMP",
      "LSR", "MOV", "MOV1", "MOVW", "MUL", "NOP", "NOT1", "NOTC", "OR",
      "OR1", "OR1C", "PCALL", "POP", "PUSH", "RET", "RETI", "ROL", "ROR",
      "SBC", "SET1", "SETC", "SETM", "SETP", "SLEEP", "STOP", "SUBW", "TCALL",
      "TCLR1", "TSET1", "XCN",
  };
  for (const char* opcode : kOpcodesSpc700) {
    if (!matches_prefix(opcode)) {
      continue;
    }
    push_item(opcode, 14, "opcode SPC700");
  }

  static const char* const kOpcodesSuperFx[] = {
      "ADC", "ADD", "AND", "ASR", "BCC", "BCS", "BEQ", "BGE", "BGT", "BLE",
      "BLT", "BMI", "BNE", "BPL", "BVC", "BVS", "CACHE", "CMODE", "CMP",
      "DEC", "DIV2", "FMULT", "FROM", "GETB", "GETBH", "GETBL", "GETBS", "GETC",
      "HIB", "IBT", "INC", "IWT", "JMP", "LMS", "LM", "LSR", "MERGE", "MOV",
      "MOVE", "MULT", "NOP", "NOT", "OR", "PLOT", "RADC", "ROL", "ROMB", "ROR",
      "RPLOT", "SBC", "SBK", "SEXB", "SEXT", "SM", "STW", "SUB", "SWAP", "TO",
      "UMULT", "WITH",
  };
  for (const char* opcode : kOpcodesSuperFx) {
    if (!matches_prefix(opcode)) {
      continue;
    }
    push_item(opcode, 14, "opcode SuperFX");
  }

  return items;
}

json BuildSemanticTokens(const DocumentState& doc) {
  struct Token {
    int line = 0;
    int column = 0;
    int length = 0;
    int type = 0;
  };

  const std::vector<std::string> token_types = {
      "function",  // 0
      "macro",     // 1
      "variable",  // 2
      "keyword",   // 3
      "string",    // 4
      "number",    // 5
      "operator",  // 6
      "register",  // 7
  };

  const int kTypeKeyword = 3;
  const int kTypeString = 4;
  const int kTypeNumber = 5;
  const int kTypeOperator = 6;
  const int kTypeRegister = 7;

  std::vector<Token> tokens;
  for (const auto& symbol : doc.symbols) {
    if (!symbol.uri.empty() && symbol.uri != doc.uri) {
      continue;
    }
    Token token;
    token.line = std::max(0, symbol.line);
    token.column = std::max(0, symbol.column);
    token.length = static_cast<int>(symbol.name.size());
    if (symbol.detail == "macro") {
      token.type = 1;
    } else if (symbol.detail == "define") {
      token.type = 2;
    } else {
      token.type = 0;
    }
    tokens.push_back(token);
  }

  static std::unordered_set<std::string> keyword_set;
  static std::unordered_set<std::string> register_set;
  if (keyword_set.empty()) {
    static const char* const kDirectives[] = {
        "arch", "autoclean", "bank", "bankbyte", "base", "cleartable", "cmode",
        "db", "dw", "dl", "dd", "dq", "define", "elif", "elseif", "else", "endif",
        "endmacro", "endstruct", "endwhile", "endfor", "error", "fill",
        "fillbyte", "fillword", "freecode", "freedata", "freespace", "hirom",
        "if", "incbin", "incgfx", "incmsg", "incsrc", "include", "incdir", "lorom", "exlorom",
        "exhirom", "macro", "namespace", "org", "pad", "padbyte", "padword",
        "pc2snes", "print", "pullpc", "pushpc", "pushns", "popns", "snes2pc",
        "struct", "table", "undef", "warn", "warning", "while", "for",
        "math", "function", "reset", "optimize", "check", "bankcross",
        "hook", "endhook",
    };
    for (const char* directive : kDirectives) {
      keyword_set.insert(z3lsp::ToLower(directive));
    }

    static const char* const kRegisters[] = {
        "a", "x", "y", "s", "p", "d", "db", "dp", "pc", "sp", "pb"
    };
    for (const char* reg : kRegisters) {
      register_set.insert(reg);
    }

    for (int i = 0; i < 256; ++i) {
      const auto& info = z3dk::GetOpcodeInfo(static_cast<uint8_t>(i));
      if (info.mnemonic != nullptr && info.mnemonic[0] != '\0') {
        keyword_set.insert(z3lsp::ToLower(info.mnemonic));
      }
    }

    static const char* const kOpcodesSpc700[] = {
        "ADC", "ADDW", "AND", "AND1", "AND1C", "ASL", "BBC", "BBS", "BCC", "BCS",
        "BEQ", "BMI", "BNE", "BPL", "BVC", "BVS", "BRA", "BRK", "CALL", "CBNE",
        "CLR1", "CLRC", "CLRP", "CLRV", "CMP", "CMPW", "DAA", "DAS", "DBNZ",
        "DEC", "DECW", "DI", "DIV", "EI", "EOR", "EOR1", "INC", "INCW", "JMP",
        "LSR", "MOV", "MOV1", "MOVW", "MUL", "NOP", "NOT1", "NOTC", "OR",
        "OR1", "OR1C", "PCALL", "POP", "PUSH", "RET", "RETI", "ROL", "ROR",
        "SBC", "SET1", "SETC", "SETM", "SETP", "SLEEP", "STOP", "SUBW", "TCALL",
        "TCLR1", "TSET1", "XCN",
    };
    for (const char* opcode : kOpcodesSpc700) {
      keyword_set.insert(z3lsp::ToLower(opcode));
    }

    static const char* const kOpcodesSuperFx[] = {
        "ADC", "ADD", "AND", "ASR", "BCC", "BCS", "BEQ", "BGE", "BGT", "BLE",
        "BLT", "BMI", "BNE", "BPL", "BVC", "BVS", "CACHE", "CMODE", "CMP",
        "DEC", "DIV2", "FMULT", "FROM", "GETB", "GETBH", "GETBL", "GETBS", "GETC",
        "HIB", "IBT", "INC", "IWT", "JMP", "LMS", "LM", "LSR", "MERGE", "MOV",
        "MOVE", "MULT", "NOP", "NOT", "OR", "PLOT", "RADC", "ROL", "ROMB", "ROR",
        "RPLOT", "SBC", "SBK", "SEXB", "SEXT", "SM", "STW", "SUB", "SWAP", "TO",
        "UMULT", "WITH",
    };
    for (const char* opcode : kOpcodesSuperFx) {
      keyword_set.insert(z3lsp::ToLower(opcode));
    }
  }

  size_t line_start = 0;
  int line_number = 0;
  while (line_start <= doc.text.size()) {
    size_t line_end = doc.text.find('\n', line_start);
    if (line_end == std::string::npos) {
      line_end = doc.text.size();
    }
    std::string line = doc.text.substr(line_start, line_end - line_start);
    std::string code = z3lsp::StripAsmComment(line);

    std::vector<std::pair<size_t, size_t>> string_ranges;
    for (size_t i = 0; i < code.size(); ++i) {
      if (code[i] != '"') {
        continue;
      }
      size_t start = i;
      ++i;
      bool escape = false;
      for (; i < code.size(); ++i) {
        if (escape) {
          escape = false;
          continue;
        }
        if (code[i] == '\\') {
          escape = true;
          continue;
        }
        if (code[i] == '"') {
          ++i;
          break;
        }
      }
      size_t end = i;
      if (end > start) {
        Token token;
        token.line = line_number;
        token.column = static_cast<int>(start);
        token.length = static_cast<int>(end - start);
        token.type = kTypeString;
        tokens.push_back(token);
        string_ranges.emplace_back(start, end);
      }
    }

    auto in_string = [&](size_t pos) {
      for (const auto& range : string_ranges) {
        if (pos >= range.first && pos < range.second) {
          return true;
        }
      }
      return false;
    };

    size_t token_pos = 0;
    while (token_pos < code.size() &&
           std::isspace(static_cast<unsigned char>(code[token_pos])) != 0) {
      ++token_pos;
    }
    size_t token_end = token_pos;
    while (token_end < code.size() &&
           std::isspace(static_cast<unsigned char>(code[token_end])) == 0) {
      ++token_end;
    }
    if (token_end > token_pos && !in_string(token_pos)) {
      std::string token = code.substr(token_pos, token_end - token_pos);
      std::string token_lower = z3lsp::ToLower(token);
      if (keyword_set.find(token_lower) != keyword_set.end()) {
        Token keyword;
        keyword.line = line_number;
        keyword.column = static_cast<int>(token_pos);
        keyword.length = static_cast<int>(token.size());
        keyword.type = kTypeKeyword;
        tokens.push_back(keyword);
      } else if (register_set.find(token_lower) != register_set.end()) {
        Token reg;
        reg.line = line_number;
        reg.column = static_cast<int>(token_pos);
        reg.length = static_cast<int>(token.size());
        reg.type = kTypeRegister;
        tokens.push_back(reg);
      }
    }

    for (size_t i = 0; i < code.size();) {
      if (in_string(i)) {
        ++i;
        continue;
      }
      char c = code[i];
      if (c == '+' || c == '-' || c == '*' || c == '/' || c == ',' || c == '(' || c == ')') {
        Token op;
        op.line = line_number;
        op.column = static_cast<int>(i);
        op.length = 1;
        op.type = kTypeOperator;
        tokens.push_back(op);
        ++i;
        continue;
      }
      if (c == '$' || c == '%') {
        size_t start = i;
        bool allow_token = true;
        if (c == '$' && start > 0) {
          char prev = code[start - 1];
          if (prev == '#') {
            // Skip immediate constants like #$xx/#$xxxx to reduce noisy highlights.
            allow_token = false;
          }
        }
        ++i;
        size_t digits = 0;
        while (i < code.size()) {
          char d = code[i];
          if (c == '$' && std::isxdigit(static_cast<unsigned char>(d)) != 0) {
            ++digits;
            ++i;
            continue;
          }
          if (c == '%' && (d == '0' || d == '1')) {
            ++digits;
            ++i;
            continue;
          }
          break;
        }
        if (digits > 0 && allow_token) {
          Token number;
          number.line = line_number;
          number.column = static_cast<int>(start);
          number.length = static_cast<int>(i - start);
          number.type = kTypeNumber;
          tokens.push_back(number);
        }
        continue;
      }
      if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
        if (i > 0) {
          char prev = code[i - 1];
          if (std::isalnum(static_cast<unsigned char>(prev)) != 0 ||
              prev == '_' || prev == '!' || prev == '.') {
            ++i;
            continue;
          }
        }
        size_t start = i;
        while (i < code.size() &&
               std::isdigit(static_cast<unsigned char>(code[i])) != 0) {
          ++i;
        }
        Token number;
        number.line = line_number;
        number.column = static_cast<int>(start);
        number.length = static_cast<int>(i - start);
        number.type = kTypeNumber;
        tokens.push_back(number);
        continue;
      }
      ++i;
    }

    line_start = line_end + 1;
    ++line_number;
  }

  std::sort(tokens.begin(), tokens.end(), [](const Token& a, const Token& b) {
    if (a.line != b.line) {
      return a.line < b.line;
    }
    return a.column < b.column;
  });

  json data = json::array();
  int last_line = 0;
  int last_column = 0;
  for (const auto& token : tokens) {
    int delta_line = token.line - last_line;
    int delta_start = delta_line == 0 ? token.column - last_column : token.column;
    data.push_back(delta_line);
    data.push_back(delta_start);
    data.push_back(token.length);
    data.push_back(token.type);
    data.push_back(0);
    last_line = token.line;
    last_column = token.column;
  }

  json result;
  result["data"] = data;
  result["legend"] = {{"tokenTypes", token_types}, {"tokenModifiers", json::array()}};
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;

  z3lsp::WorkspaceState workspace;
  std::unordered_map<std::string, z3lsp::DocumentState> documents;
  bool shutting_down = false;

  // Debounce settings: delay full analysis until typing pauses
  constexpr auto kDebounceDelay = std::chrono::milliseconds(500);
  auto last_change_time = std::chrono::steady_clock::now();

  while (std::cin.good()) {
    auto message = z3lsp::ReadMessage();
    if (!message.has_value()) {
      if (std::cin.eof()) {
        break;
      }
      continue;
    }

    const json& request = *message;
    const std::string method = request.value("method", "");

    if (method == "initialize") {
      auto params = request.value("params", json::object());
      auto workspace_state = z3lsp::BuildWorkspaceState(params);
      if (workspace_state.has_value()) {
        workspace = *workspace_state;
      }
      json capabilities = {
          {"capabilities",
           {{"textDocumentSync", 1},
            {"definitionProvider", true},
            {"hoverProvider", true},
            {"executeCommandProvider", {{"commands", json::array({"mesen.toggleBreakpoint", "mesen.syncSymbols", "mesen.showCpuState", "mesen.stepInstruction"})}}},
            {"completionProvider",
             {{"triggerCharacters", json::array({"!", ".", "@"})}}},
            {"signatureHelpProvider", 
             {{"triggerCharacters", json::array({"(", ","})}}},
            {"inlayHintProvider", {{"resolveProvider", false}}},
            {"inlayHintProvider", {{"resolveProvider", false}}},
            {"referencesProvider", true},
            {"renameProvider", true},
            {"documentSymbolProvider", true},
            {"workspaceSymbolProvider", true},
            {"semanticTokensProvider",
             {{"legend",
               {{"tokenTypes",
                 {"function", "macro", "variable", "keyword", "string", "number",
                  "operator", "register"}},
                {"tokenModifiers", json::array()}}},
              {"full", true}}}}},
      };
      json response = {
          {"jsonrpc", "2.0"},
          {"id", request["id"]},
          {"result", capabilities},
      };
      z3lsp::SendMessage(response);
      continue;
    }

    if (method == "textDocument/rename") {
      auto params = request.value("params", json::object());
      json response = {
          {"jsonrpc", "2.0"},
          {"id", request["id"]},
          {"result", nullptr},
      };
      
      auto text_doc = params.value("textDocument", json::object());
      std::string uri = text_doc.value("uri", "");
      auto it = documents.find(uri);
      
      if (it != documents.end()) {
          auto result = HandleRename(it->second, workspace, documents, params);
          if (result.has_value()) {
            response["result"] = *result;
          }
      }
      z3lsp::SendMessage(response);
      continue;
    }

    if (method == "textDocument/semanticTokens/full") {
      auto params = request.value("params", json::object());
      auto text_doc = params.value("textDocument", json::object());
      std::string uri = text_doc.value("uri", "");
      json response = {
          {"jsonrpc", "2.0"},
          {"id", request["id"]},
          {"result", nullptr},
      };
      auto it = documents.find(uri);
      if (it != documents.end()) {
        response["result"] = BuildSemanticTokens(it->second);
      }
      z3lsp::SendMessage(response);
      continue;
    }

    if (method == "workspace/executeCommand") {
      auto params = request["params"];
      std::string command = params["command"];
      auto args = params.value("arguments", json::array());

      json response = {
          {"jsonrpc", "2.0"},
          {"id", request["id"]},
          {"result", nullptr},
      };

      if (command == "mesen.syncSymbols") {
        // Find an open document that has labels from the assembler
        for (const auto& pair : documents) {
          if (!pair.second.labels.empty()) {
            json mesen_cmd = {{"type", "SYMBOLS_LOAD"}, {"symbols", json::array()}};
            for (const auto& label : pair.second.labels) {
              mesen_cmd["symbols"].push_back({
                {"name", label.name},
                {"addr", label.address}
              });
            }
            z3lsp::g_mesen.SendCommand(mesen_cmd);
            response["result"] = "Synced " + std::to_string(pair.second.labels.size()) + " symbols";
            break;
          }
        }
      } else if (command == "mesen.toggleBreakpoint") {
        if (!args.empty() && args[0].is_number()) {
          uint32_t addr = args[0].get<uint32_t>();
          json mesen_cmd = {{"type", "BREAKPOINT"}, {"action", "toggle"}, {"addr", addr}};
          z3lsp::g_mesen.SendCommand(mesen_cmd);
          std::string addr_str;
          std::ostringstream ss;
          ss << std::hex << std::uppercase << std::setfill('0') << std::setw(6) << addr;
          response["result"] = "Toggled breakpoint at $" + ss.str();
        }
      } else if (command == "mesen.stepInstruction") {
        json mesen_cmd = {{"type", "STEP_INTO"}};
        if (z3lsp::g_mesen.SendCommand(mesen_cmd)) {
           response["result"] = "Stepped one instruction";
        } else {
           response["result"] = "Failed to step execution";
        }
      } else if (command == "z3dk.getBankUsage") {
        json blocks = json::array();
        std::unordered_set<std::string> seen;
        for (const auto& pair : documents) {
          if (pair.second.written_blocks.empty()) {
            continue;
          }
          for (const auto& block : pair.second.written_blocks) {
            std::string key = std::to_string(block.snes_offset) + ":" +
                              std::to_string(block.pc_offset) + ":" +
                              std::to_string(block.num_bytes);
            if (!seen.insert(key).second) {
              continue;
            }
            blocks.push_back({
              {"snes", block.snes_offset},
              {"pc", block.pc_offset},
              {"size", block.num_bytes}
            });
          }
        }
        response["result"] = blocks;
      } else if (command == "mesen.showCpuState") {
        json mesen_cmd = {{"type", "GAMESTATE"}};
        auto result = z3lsp::g_mesen.SendCommand(mesen_cmd);
        if (result) {
           response["result"] = result->dump(2);
        } else {
           response["result"] = "Failed to retrieve CPU state";
        }
      }

      z3lsp::SendMessage(response);
      continue;
    }

    if (method == "textDocument/signatureHelp") {
      json result = {
        {"signatures", json::array()},
        {"activeSignature", 0},
        {"activeParameter", 0}
      };

      auto params = request["params"];
      std::string uri = params["textDocument"]["uri"];
      int line = params["position"]["line"];
      int character = params["position"]["character"];

      if (documents.count(uri)) {
        const auto& doc = documents[uri];
        int current_line = 0;
        size_t offset = 0;
        
        while (offset < doc.text.size() && current_line < line) {
          if (doc.text[offset] == '\n') ++current_line;
          ++offset;
        }

        if (current_line == line) {
          size_t line_end = doc.text.find('\n', offset);
          if (line_end == std::string::npos) line_end = doc.text.size();
          std::string line_text = doc.text.substr(offset, line_end - offset);
          
          int cursor_col = character;
          if (cursor_col > static_cast<int>(line_text.size())) cursor_col = static_cast<int>(line_text.size());
          
          int balance = 0;
          int param_index = 0;
          int p = cursor_col - 1;
          bool found_start = false;

          while (p >= 0) {
            char c = line_text[p];
            if (c == ')') balance++;
            else if (c == '(') {
              if (balance > 0) balance--;
              else {
                found_start = true;
                break;
              }
            } else if (c == ',' && balance == 0) {
              param_index++;
            }
            p--;
          }

          if (found_start && p > 0) {
            std::string prefix = line_text.substr(0, p);
            size_t name_end = prefix.find_last_not_of(" \t");
            if (name_end != std::string::npos) {
              size_t name_start = name_end;
              while (name_start > 0 && (std::isalnum(prefix[name_start - 1]) || prefix[name_start - 1] == '_' || prefix[name_start - 1] == '+')) {
                name_start--;
              }
              std::string macro_name = prefix.substr(name_start, name_end - name_start + 1);
              if (macro_name.size() > 1 && macro_name[0] == '+') macro_name = macro_name.substr(1);

              const z3lsp::DocumentState::SymbolEntry* found_symbol = nullptr;
              
              for (const auto& sym : doc.symbols) {
                if (sym.kind == 12 && sym.name == macro_name) {
                   found_symbol = &sym;
                   break;
                }
              }

              if (!found_symbol && workspace.symbol_index.count(macro_name)) {
                 const auto& candidates = workspace.symbol_index.at(macro_name);
                 for (const auto& sym : candidates) {
                    if (sym.kind == 12) {
                        found_symbol = &sym;
                        break;
                    }
                 }
              }

              if (found_symbol && !found_symbol->parameters.empty()) {
                std::string label = found_symbol->name + "(";
                for (size_t i = 0; i < found_symbol->parameters.size(); ++i) {
                  if (i > 0) label += ", ";
                  label += found_symbol->parameters[i];
                }
                label += ")";
                
                json signature = {
                    {"label", label},
                    {"parameters", json::array()}
                };
                for (const auto& param : found_symbol->parameters) {
                    signature["parameters"].push_back({{"label", param}});
                }
                result["signatures"].push_back(signature);
                result["activeParameter"] = param_index;
              }
            }
          }
        }
      }

      json response = {
          {"jsonrpc", "2.0"},
          {"id", request["id"]},
          {"result", result},
      };
      z3lsp::SendMessage(response);
      continue;
    }


    if (method == "textDocument/inlayHint") {
      json result = json::array();
      auto params = request["params"];
      std::string uri = params["textDocument"]["uri"];
      
      int start_line = 0;
      int end_line = std::numeric_limits<int>::max();
      if (params.contains("range")) {
          start_line = params["range"]["start"]["line"];
          end_line = params["range"]["end"]["line"];
      }

      if (documents.count(uri)) {
        const auto& doc = documents[uri];
        int line = 0;
        int col = 0;
        for (size_t i = 0; i < doc.text.size(); ++i) {
             if (doc.text[i] == '\n') {
                 line++;
                 col = 0;
                 continue;
             }
             if (line > end_line) break;
             
             // Hex Address Hints
             if (doc.text[i] == '$') {
                 size_t j = i + 1;
                 while (j < doc.text.size() && std::isxdigit(static_cast<unsigned char>(doc.text[j]))) {
                     j++;
                 }
                 size_t len = j - (i + 1);
                 
                 // Only hint fully-qualified long addresses ($xxxxxx)
                 if (len == 6 && line >= start_line) {
                     std::string hex = doc.text.substr(i + 1, len);
                     try {
                         uint32_t addr = std::stoul(hex, nullptr, 16);
                         if (doc.address_to_label_map.count(addr)) {
                             std::string label = doc.address_to_label_map.at(addr);
                             result.push_back({
                                 {"position", {{"line", line}, {"character", col + (int)len + 1}}},
                                 {"label", " :" + label}, 
                                 {"kind", 1}, 
                                 {"paddingLeft", true}
                             });
                         }
                     } catch (...) {}
                 }
                 col += (int)(j - i);
                 i = j - 1;
                 continue;
             }
             
             // Macro Hints
             auto is_ident_start = [](char c) { 
               return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '.' || c == '+' || c == '!'; 
             };
             auto is_ident_char = [](char c) { 
               return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '.' || c == '+' || c == '!'; 
             };

             if (is_ident_start(doc.text[i])) {
                 size_t start = i;
                 size_t j = i;
                 while (j < doc.text.size() && is_ident_char(doc.text[j])) {
                     j++;
                 }
                 size_t len = j - start;
                 
                 if (len > 0 && line >= start_line) {
                     std::string word = doc.text.substr(start, len);
                     std::string clean = word;
                     if (clean.size() > 1 && clean[0] == '+') clean = clean.substr(1);

                     const z3lsp::DocumentState::SymbolEntry* macro = nullptr;
                     for (const auto& s : doc.symbols) {
                         if (s.kind == 12 && s.name == clean) { macro = &s; break; }
                     }
                     if (!macro && workspace.symbol_index.count(clean)) {
                         for (const auto& s : workspace.symbol_index.at(clean)) {
                             if (s.kind == 12) { macro = &s; break; }
                         }
                     }

                     if (macro && !macro->parameters.empty()) {
                         // Look ahead for '('
                         size_t k = j;
                         int col_off = 0;
                         while (k < doc.text.size() && doc.text[k] != '\n' &&
                                std::isspace(static_cast<unsigned char>(doc.text[k]))) {
                             k++;
                             col_off++;
                         }
                         
                         if (k < doc.text.size() && doc.text[k] == '(') {
                             k++; // skip '('
                             col_off++;
                             
                             // Add first param hint
                             result.push_back({
                                 {"position", {{"line", line}, {"character", col + (int)len + col_off}}},
                                 {"label", macro->parameters[0] + ":"},
                                 {"kind", 2},
                                 {"paddingRight", true}
                             });

                             // Parse rest (basic csv info parens)
                             int p_idx = 1;
                             int bal = 0;
                             bool in_str = false;
                             int arg_col_off = col + (int)len + col_off;
                             
                             while(k < doc.text.size() && doc.text[k] != '\n' && p_idx < macro->parameters.size()) {
                                 char c = doc.text[k];
                                 if(c == '"') in_str = !in_str;
                                 else if(!in_str) {
                                     if(c == '(') bal++;
                                     else if(c == ')') {
                                         if(bal == 0) break;
                                         bal--;
                                     } else if(c == ',' && bal == 0) {
                                         k++;
                                         arg_col_off++;
                                         while(k < doc.text.size() && doc.text[k] != '\n' &&
                                               std::isspace(static_cast<unsigned char>(doc.text[k]))) {
                                             k++;
                                             arg_col_off++;
                                         }
                                         result.push_back({
                                             {"position", {{"line", line}, {"character", arg_col_off}}},
                                             {"label", macro->parameters[p_idx] + ":"},
                                             {"kind", 2},
                                             {"paddingRight", true}
                                         });
                                         p_idx++;
                                         continue;
                                     }
                                 }
                                 k++;
                                 arg_col_off++;
                             }
                         }
                     }
                 }
                 col += (int)len;
                 i = j - 1;
                 continue;
             }

             col++;
        }
      }

      json response = {
          {"jsonrpc", "2.0"},
          {"id", request["id"]},
          {"result", result},
      };
      z3lsp::SendMessage(response);
      continue;
    }



    if (method == "textDocument/references") {
      json result = json::array();
      auto params = request["params"];
      std::string uri = params["textDocument"]["uri"];
      int line = params["position"]["line"];
      int character = params["position"]["character"];
      
      std::string token;
      if (documents.count(uri)) {
          const auto& doc = documents[uri];
          auto extracted = z3lsp::ExtractTokenAt(doc.text, line, character);
          if (extracted) token = *extracted;
      }
      
      if (!token.empty()) {
          std::vector<fs::path> files_to_scan;
          if (fs::exists(workspace.root) && fs::is_directory(workspace.root)) {
             for (const auto& entry : fs::recursive_directory_iterator(workspace.root)) {
                 if (entry.is_regular_file()) {
                     auto ext = entry.path().extension();
                     if (ext == ".asm" || ext == ".s" || ext == ".inc" || ext == ".a") {
                         if (z3lsp::IsGitIgnoredPath(workspace, entry.path())) {
                             continue;
                         }
                         files_to_scan.push_back(entry.path());
                     }
                 }
             }
          }
          
          for (const auto& path : files_to_scan) {
              std::string text;
              std::string file_uri = "file://" + path.string();
              
              if (documents.count(file_uri)) {
                  text = documents[file_uri].text;
              } else {
                  std::ifstream f(path);
                  if (f) {
                      std::stringstream buffer;
                      buffer << f.rdbuf();
                      text = buffer.str();
                  }
              }
              
              if (text.empty()) continue;
              
              int current_line = 0;
              int current_col = 0;
              size_t i = 0;
              while (i < text.size()) {
                  if (text[i] == '\n') {
                      current_line++;
                      current_col = 0;
                      i++;
                      continue;
                  }
                  
                  if (text[i] == token[0] && text.compare(i, token.size(), token) == 0) {
                       bool start_ok = (i == 0) || !z3lsp::IsSymbolChar(text[i - 1]);
                       bool end_ok = (i + token.size() == text.size()) || !z3lsp::IsSymbolChar(text[i + token.size()]);
                       
                       if (start_ok && end_ok) {
                           result.push_back({
                               {"uri", file_uri},
                               {"range", {
                                   {"start", {{"line", current_line}, {"character", current_col}}},
                                   {"end", {{"line", current_line}, {"character", current_col + (int)token.size()}}}
                               }}
                           });
                           i += token.size();
                           current_col += (int)token.size();
                           continue;
                       }
                  }
                  i++;
                  current_col++;
              }
          }
      }

      json response = {
          {"jsonrpc", "2.0"},
          {"id", request["id"]},
          {"result", result},
      };
      z3lsp::SendMessage(response);
      continue;
    }

    if (method == "shutdown") {
      shutting_down = true;
      json response = {
          {"jsonrpc", "2.0"},
          {"id", request["id"]},
          {"result", nullptr},
      };
      z3lsp::SendMessage(response);
      continue;
    }

    if (method == "exit") {
      return shutting_down ? 0 : 1;
    }

    if (method == "textDocument/didOpen") {
      auto params = request.value("params", json::object());
      auto text_doc = params.value("textDocument", json::object());
      z3lsp::DocumentState doc;
      doc.uri = text_doc.value("uri", "");
      doc.path = z3lsp::UriToPath(doc.uri);
      doc.text = text_doc.value("text", "");
      doc.version = text_doc.value("version", 0);
      doc = AnalyzeDocument(doc, workspace, &documents);
      documents[doc.uri] = doc;
      PublishDiagnostics(doc);
      continue;
    }

    if (method == "textDocument/didChange") {
      auto params = request.value("params", json::object());
      auto text_doc = params.value("textDocument", json::object());
      std::string uri = text_doc.value("uri", "");
      auto it = documents.find(uri);
      if (it == documents.end()) {
        continue;
      }
      auto changes = params.value("contentChanges", json::array());
      if (!changes.empty()) {
        it->second.text = changes[0].value("text", it->second.text);
      }
      it->second.version = text_doc.value("version", it->second.version);

      // Mark document as needing analysis and record change time
      it->second.needs_analysis = true;
      it->second.last_change = std::chrono::steady_clock::now();
      last_change_time = it->second.last_change;

      // Propagate analysis to root
      std::string root_uri = SelectRootUri(uri, workspace);
      if (root_uri != uri && documents.count(root_uri)) {
        documents.at(root_uri).needs_analysis = true;
        documents.at(root_uri).last_change = it->second.last_change;
      }

      // Do lightweight symbol extraction (fast) for immediate responsiveness
      it->second.symbols = z3lsp::ParseFileText(it->second.text, it->second.uri).symbols;
      continue;
    }

    // Process pending analyses after debounce delay
    if (!documents.empty()) {
      auto now = std::chrono::steady_clock::now();
      if (now - last_change_time > kDebounceDelay) {
        for (auto& pair : documents) {
          if (pair.second.needs_analysis) {
            pair.second = AnalyzeDocument(pair.second, workspace, &documents);
            PublishDiagnostics(pair.second);
          }
        }
      }
    }

    if (method == "textDocument/didClose") {
      auto params = request.value("params", json::object());
      auto text_doc = params.value("textDocument", json::object());
      std::string uri = text_doc.value("uri", "");
      auto it = documents.find(uri);
      if (it != documents.end()) {
        z3lsp::DocumentState cleared = it->second;
        cleared.diagnostics.clear();
        PublishDiagnostics(cleared);
        documents.erase(it);
      }
      continue;
    }

    if (method == "textDocument/definition") {
      auto params = request.value("params", json::object());
      auto text_doc = params.value("textDocument", json::object());
      std::string uri = text_doc.value("uri", "");
      json response = {
          {"jsonrpc", "2.0"},
          {"id", request["id"]},
          {"result", nullptr},
      };
      auto it = documents.find(uri);
      if (it != documents.end()) {
        auto result = HandleDefinition(it->second, params);
        if (result.has_value()) {
          response["result"] = *result;
        }
      }
      z3lsp::SendMessage(response);
      continue;
    }

    if (method == "textDocument/documentSymbol") {
      auto params = request.value("params", json::object());
      auto text_doc = params.value("textDocument", json::object());
      std::string uri = text_doc.value("uri", "");
      json response = {
          {"jsonrpc", "2.0"},
          {"id", request["id"]},
          {"result", json::array()},
      };
      auto it = documents.find(uri);
      if (it != documents.end()) {
        response["result"] = BuildDocumentSymbols(it->second);
      }
      z3lsp::SendMessage(response);
      continue;
    }

    if (method == "textDocument/hover") {
      auto params = request.value("params", json::object());
      auto text_doc = params.value("textDocument", json::object());
      std::string uri = text_doc.value("uri", "");
      json response = {
          {"jsonrpc", "2.0"},
          {"id", request["id"]},
          {"result", nullptr},
      };
      auto it = documents.find(uri);
      if (it != documents.end()) {
        auto result = HandleHover(it->second, params);
        if (result.has_value()) {
          response["result"] = *result;
        }
      }
      z3lsp::SendMessage(response);
      continue;
    }

    if (method == "workspace/symbol") {
      auto params = request.value("params", json::object());
      std::string query = params.value("query", "");
      json response = {
          {"jsonrpc", "2.0"},
          {"id", request["id"]},
          {"result", BuildWorkspaceSymbols(workspace, query)},
      };
      z3lsp::SendMessage(response);
      continue;
    }

    if (method == "textDocument/semanticTokens/full") {
      auto params = request.value("params", json::object());
      auto text_doc = params.value("textDocument", json::object());
      std::string uri = text_doc.value("uri", "");
      json response = {
          {"jsonrpc", "2.0"},
          {"id", request["id"]},
          {"result", json::object()},
      };
      auto it = documents.find(uri);
      if (it != documents.end()) {
        response["result"] = BuildSemanticTokens(it->second);
      }
      z3lsp::SendMessage(response);
      continue;
    }

    if (method == "textDocument/completion") {
      auto params = request.value("params", json::object());
      auto text_doc = params.value("textDocument", json::object());
      std::string uri = text_doc.value("uri", "");
      json response = {
          {"jsonrpc", "2.0"},
          {"id", request["id"]},
          {"result", json::array()},
      };
      auto it = documents.find(uri);
      if (it != documents.end()) {
        auto position = params.value("position", json::object());
        int line = position.value("line", 0);
        int character = position.value("character", 0);
        auto prefix = z3lsp::ExtractTokenPrefix(it->second.text, line, character);
        if (prefix.has_value()) {
          response["result"] = BuildCompletionItems(it->second, workspace, *prefix);
        }
      }
      z3lsp::SendMessage(response);
      continue;
    }
  }

  return 0;
}
