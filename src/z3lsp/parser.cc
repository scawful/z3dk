#include "parser.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include "utils.h"

namespace fs = std::filesystem;

namespace z3lsp {

std::unordered_map<std::string, CachedParse> g_parse_cache;
std::unordered_map<std::string, RomCacheEntry> g_rom_cache;

std::string StripAsmComment(std::string_view line) {
  bool in_string = false;
  bool escape = false;
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (escape) {
      escape = false;
      continue;
    }
    if (c == '\\') {
      escape = true;
      continue;
    }
    if (c == '"') {
      in_string = !in_string;
      continue;
    }
    if (!in_string && c == ';') {
      return std::string(line.substr(0, i));
    }
  }
  return std::string(line);
}

bool ParseIncludeDirective(const std::string& trimmed, std::string* out_path) {
  if (!out_path) return false;
  std::string lower = ToLower(trimmed);
  std::string keyword;
  if (HasPrefixIgnoreCase(lower, "incsrc")) keyword = "incsrc";
  else if (HasPrefixIgnoreCase(lower, "include")) keyword = "include";
  else return false;
  
  std::string rest = Trim(trimmed.substr(keyword.size()));
  if (rest.empty()) return false;
  if (rest.front() == '"') {
    size_t end = rest.find('"', 1);
    if (end == std::string::npos || end <= 1) return false;
    *out_path = rest.substr(1, end - 1);
    return true;
  }
  size_t end = rest.find_first_of(" \t");
  *out_path = rest.substr(0, end);
  return !out_path->empty();
}

bool ParseIncdirDirective(const std::string& trimmed, std::string* out_path) {
  if (!out_path) return false;
  std::string lower = ToLower(trimmed);
  if (!HasPrefixIgnoreCase(lower, "incdir")) return false;
  std::string rest = Trim(trimmed.substr(6));
  if (rest.empty()) return false;
  if (rest.front() == '"') {
    size_t end = rest.find('"', 1);
    if (end == std::string::npos || end <= 1) return false;
    *out_path = rest.substr(1, end - 1);
    return true;
  }
  size_t end = rest.find_first_of(" \t");
  *out_path = rest.substr(0, end);
  return !out_path->empty();
}

std::optional<std::string> ResolveIncdirPath(const std::string& raw,
                                             const fs::path& base_dir) {
  if (raw.empty()) return std::nullopt;
  fs::path candidate = raw;
  if (!candidate.is_absolute()) {
    if (base_dir.empty()) return std::nullopt;
    candidate = base_dir / candidate;
  }
  candidate = candidate.lexically_normal();
  if (!fs::exists(candidate)) return std::nullopt;
  return candidate.string();
}

bool ResolveIncludePath(const std::string& raw,
                        const fs::path& base_dir,
                        const std::vector<std::string>& include_paths,
                        fs::path* out_path) {
  if (!out_path) return false;
  fs::path candidate = raw;
  if (candidate.is_absolute()) {
    if (fs::exists(candidate)) {
      *out_path = candidate;
      return true;
    }
    return false;
  }
  if (!base_dir.empty()) {
    fs::path local = base_dir / candidate;
    if (fs::exists(local)) {
      *out_path = local;
      return true;
    }
  }
  for (const auto& inc : include_paths) {
    fs::path path = fs::path(inc) / candidate;
    if (fs::exists(path)) {
      *out_path = path;
      return true;
    }
  }
  return false;
}

void IndexIncludeDependencies(ProjectGraph& graph,
                              const ParsedFile& parsed,
                              const fs::path& parent_path,
                              const std::vector<std::string>& include_paths) {
  if (parent_path.empty()) return;
  fs::path base_dir = parent_path.parent_path();
  std::vector<std::string> include_paths_current = include_paths;
  for (const auto& event : parsed.events) {
    if (event.type == IncludeEvent::Type::kIncdir) {
      auto resolved_incdir = ResolveIncdirPath(event.path, base_dir);
      if (resolved_incdir.has_value() &&
          std::find(include_paths_current.begin(),
                    include_paths_current.end(),
                    *resolved_incdir) == include_paths_current.end()) {
        include_paths_current.push_back(*resolved_incdir);
      }
      continue;
    }

    fs::path resolved;
    if (!ResolveIncludePath(event.path, base_dir, include_paths_current, &resolved)) continue;
    std::error_code ec;
    fs::path absolute = fs::absolute(resolved, ec);
    if (ec) continue;
    std::string parent_uri = PathToUri(parent_path.string());
    std::string child_uri = PathToUri(absolute.string());
    graph.RegisterDependency(parent_uri, child_uri);
  }
}

void SeedMainCandidates(const fs::path& root,
                        std::unordered_set<std::string>* main_candidates) {
  if (!main_candidates || root.empty()) return;
  std::error_code ec;
  if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) return;
  for (const auto& entry : fs::directory_iterator(root, ec)) {
    if (ec) break;
    if (!entry.is_regular_file()) continue;
    const auto& path = entry.path();
    if (path.extension() != ".asm" && path.extension() != ".s" && path.extension() != ".inc") continue;
    if (IsMainFileName(path)) main_candidates->insert(PathToUri(path.string()));
  }
}

bool AddMainCandidatesFromConfig(const z3dk::Config& config,
                                 const fs::path& config_dir,
                                 const fs::path& workspace_root,
                                 std::unordered_set<std::string>* out) {
  if (!out || config.main_files.empty()) return false;
  bool added = false;
  for (const auto& entry : config.main_files) {
    if (entry.empty()) continue;
    fs::path resolved = ResolveConfigPath(entry, config_dir, workspace_root);
    if (resolved.empty()) continue;
    std::error_code ec;
    if (!fs::exists(resolved, ec)) continue;
    out->insert(PathToUri(NormalizePath(resolved).string()));
    added = true;
  }
  return added;
}

bool LoadRomData(const fs::path& path, std::vector<uint8_t>* out) {
  if (!out || path.empty()) return false;
  std::error_code ec;
  if (!fs::exists(path, ec) || ec) return false;

  fs::path normalized = NormalizePath(path);
  std::string key = normalized.string();
  fs::file_time_type mtime = fs::last_write_time(normalized, ec);
  if (ec) return false;

  auto it = g_rom_cache.find(key);
  if (it != g_rom_cache.end() && it->second.mtime == mtime) {
    *out = it->second.data;
    return true;
  }

  std::ifstream file(normalized, std::ios::binary);
  if (!file.is_open()) return false;
  file.seekg(0, std::ios::end);
  std::streamoff size = file.tellg();
  if (size <= 0) return false;
  file.seekg(0, std::ios::beg);
  std::vector<uint8_t> data(static_cast<size_t>(size));
  if (!file.read(reinterpret_cast<char*>(data.data()), size)) return false;
  g_rom_cache[key] = {mtime, std::move(data)};
  *out = g_rom_cache[key].data;
  return true;
}

static bool EndsWithPath(const fs::path& full, const fs::path& suffix) {
  std::string full_str = full.generic_string();
  std::string suffix_str = suffix.generic_string();
  if (suffix_str.empty()) return false;
  if (full_str == suffix_str) return true;
  if (full_str.size() <= suffix_str.size()) return false;
  if (full_str.compare(full_str.size() - suffix_str.size(), suffix_str.size(), suffix_str) != 0) return false;
  char sep = full_str[full_str.size() - suffix_str.size() - 1];
  return sep == '/';
}

bool PathMatchesDocumentPath(const std::string& candidate_path,
                             const fs::path& doc_path,
                             const fs::path& analysis_root_dir,
                             const fs::path& workspace_root) {
  if (candidate_path.empty()) return false;
  fs::path doc_norm = NormalizePath(doc_path);
  fs::path diag_path = fs::path(candidate_path);
  if (diag_path.is_absolute()) return NormalizePath(diag_path) == doc_norm;
  if (!analysis_root_dir.empty() && NormalizePath(analysis_root_dir / diag_path) == doc_norm) return true;
  if (!workspace_root.empty() && NormalizePath(workspace_root / diag_path) == doc_norm) return true;
  return EndsWithPath(doc_norm, diag_path);
}

bool DiagnosticMatchesDocument(const z3dk::Diagnostic& diag,
                               const fs::path& doc_path,
                               const fs::path& analysis_root_dir,
                               const fs::path& workspace_root,
                               bool doc_is_root) {
  if (diag.filename.empty()) return doc_is_root;
  return PathMatchesDocumentPath(diag.filename, doc_path, analysis_root_dir, workspace_root);
}

std::string ExtractMissingLabel(const std::string& message) {
  const std::string needle = "Label '";
  size_t start = message.find(needle);
  if (start != std::string::npos) {
    start += needle.size();
    size_t end = message.find('\'', start);
    if (end != std::string::npos && end > start) return message.substr(start, end - start);
  }
  const std::string needle2 = "Label ";
  start = message.find(needle2);
  if (start != std::string::npos) {
    start += needle2.size();
    size_t end = message.find(' ', start);
    if (end != std::string::npos && end > start) return message.substr(start, end - start);
  }
  return {};
}

ParsedFile ParseFileText(const std::string& text, const std::string& uri) {
  ParsedFile parsed;
  const int kSymbolFunction = 12;
  const int kSymbolConstant = 21;

  size_t line_start = 0;
  int line_number = 0;
  std::vector<std::string> namespace_stack;
  std::string current_struct;
  bool in_struct = false;
  auto get_current_namespace = [&]() {
    std::string ns;
    for (const auto& s : namespace_stack) {
      if (!ns.empty()) ns += "_";
      ns += s;
    }
    return ns;
  };

  while (line_start <= text.size()) {
    size_t line_end = text.find('\n', line_start);
    if (line_end == std::string::npos) line_end = text.size();
    std::string line = text.substr(line_start, line_end - line_start);
    std::string stripped = StripAsmComment(line);
    std::string trimmed = Trim(stripped);
    if (!trimmed.empty()) {
      auto is_ident_start = [](char c) {
        return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '.';
      };
      auto is_ident_char = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '.';
      };

      std::string incdir_path;
      if (ParseIncdirDirective(trimmed, &incdir_path)) {
        parsed.events.push_back({IncludeEvent::Type::kIncdir, incdir_path});
      }

      std::string include_path;
      if (ParseIncludeDirective(trimmed, &include_path)) {
        parsed.events.push_back({IncludeEvent::Type::kInclude, include_path});
      }

      if (HasPrefixIgnoreCase(trimmed, "namespace ")) {
        std::string name = Trim(trimmed.substr(10));
        if (name == "off") namespace_stack.clear();
        else if (!name.empty()) namespace_stack.push_back(name);
        line_start = line_end + 1;
        ++line_number;
        continue;
      }

      if (HasPrefixIgnoreCase(trimmed, "struct ")) {
        std::string rest = Trim(trimmed.substr(6));
        size_t end = rest.find_first_of(" \t{");
        std::string struct_name = end == std::string::npos ? rest : rest.substr(0, end);
        if (!struct_name.empty()) {
          std::string full_name = struct_name;
          std::string ns = get_current_namespace();
          if (!ns.empty()) full_name = ns + "_" + struct_name;
          current_struct = full_name;
          in_struct = true;

          DocumentState::SymbolEntry entry;
          entry.name = full_name;
          entry.kind = kSymbolConstant;
          entry.line = line_number;
          entry.detail = "struct";
          entry.uri = uri;
          size_t column = line.find(struct_name);
          entry.column = column == std::string::npos ? 0 : static_cast<int>(column);
          parsed.symbols.push_back(std::move(entry));
        }
        line_start = line_end + 1;
        ++line_number;
        continue;
      }

      if (HasPrefixIgnoreCase(trimmed, "endstruct")) {
        current_struct.clear();
        in_struct = false;
        line_start = line_end + 1;
        ++line_number;
        continue;
      }

      if (in_struct && !current_struct.empty() && trimmed.size() > 1 && trimmed[0] == '.') {
        size_t colon = trimmed.find(':');
        if (colon != std::string::npos) {
          std::string field = Trim(trimmed.substr(1, colon - 1));
          if (!field.empty()) {
            DocumentState::SymbolEntry entry;
            entry.name = current_struct + "." + field;
            entry.kind = kSymbolConstant;
            entry.line = line_number;
            entry.detail = "struct-field";
            entry.uri = uri;
            size_t column = line.find(field);
            entry.column = column == std::string::npos ? 0 : static_cast<int>(column);
            parsed.symbols.push_back(std::move(entry));
            line_start = line_end + 1;
            ++line_number;
            continue;
          }
        }
      }

      if (HasPrefixIgnoreCase(trimmed, "pushns ")) {
        std::string name = Trim(trimmed.substr(7));
        if (!name.empty()) namespace_stack.push_back(name);
        line_start = line_end + 1;
        ++line_number;
        continue;
      }

      if (HasPrefixIgnoreCase(trimmed, "popns")) {
        if (!namespace_stack.empty()) namespace_stack.pop_back();
        line_start = line_end + 1;
        ++line_number;
        continue;
      }

      if (HasPrefixIgnoreCase(trimmed, "macro")) {
        std::string rest = Trim(trimmed.substr(5));
        if (!rest.empty()) {
          size_t end = rest.find_first_of(" \t(");
          std::string name = rest.substr(0, end);
          if (!name.empty()) {
            std::string full_name = name;
            std::string ns = get_current_namespace();
            if (!ns.empty()) full_name = ns + "_" + name;

            DocumentState::SymbolEntry entry;
            entry.name = full_name;
            entry.kind = kSymbolFunction;
            entry.line = line_number;
            entry.detail = "macro";
            entry.uri = uri;
            size_t column = line.find(name);
            entry.column = column == std::string::npos ? 0 : static_cast<int>(column);

            size_t open_paren = rest.find('(');
            size_t close_paren = rest.find(')');
            if (open_paren != std::string::npos && close_paren != std::string::npos && close_paren > open_paren) {
              std::string params_str = rest.substr(open_paren + 1, close_paren - open_paren - 1);
              size_t start = 0;
              size_t end = params_str.find(',');
              while (end != std::string::npos) {
                entry.parameters.push_back(std::string(Trim(params_str.substr(start, end - start))));
                start = end + 1;
                end = params_str.find(',', start);
              }
              std::string last = std::string(Trim(params_str.substr(start)));
              if (!last.empty()) entry.parameters.push_back(last);
            }
            parsed.symbols.push_back(std::move(entry));
            line_start = line_end + 1;
            ++line_number;
            continue;
          }
        }
      }

      if (trimmed[0] == '!') {
        size_t i = 1;
        while (i < trimmed.size() && IsSymbolChar(trimmed[i])) ++i;
        std::string name = trimmed.substr(1, i - 1);
        if (!name.empty()) {
          std::string full_name = name;
          std::string ns = get_current_namespace();
          if (!ns.empty()) full_name = ns + "_" + name;

          DocumentState::SymbolEntry entry;
          entry.name = full_name;
          entry.kind = kSymbolConstant;
          entry.line = line_number;
          entry.detail = "define";
          entry.uri = uri;
          std::string needle = "!" + name;
          size_t column = line.find(needle);
          entry.column = column == std::string::npos ? 0 : static_cast<int>(column + 1);
          parsed.symbols.push_back(std::move(entry));
        }
      } else if (HasPrefixIgnoreCase(trimmed, "define ")) {
        std::string rest = Trim(trimmed.substr(7));
        size_t end = rest.find_first_of(" \t");
        std::string name = rest.substr(0, end);
        if (!name.empty()) {
          std::string full_name = name;
          std::string ns = get_current_namespace();
          if (!ns.empty()) full_name = ns + "_" + name;

          DocumentState::SymbolEntry entry;
          entry.name = full_name;
          entry.kind = kSymbolConstant;
          entry.line = line_number;
          entry.detail = "define";
          entry.uri = uri;
          size_t column = line.find(name);
          entry.column = column == std::string::npos ? 0 : static_cast<int>(column);
          parsed.symbols.push_back(std::move(entry));
        }
      }

      size_t eq_pos = trimmed.find('=');
      if (eq_pos != std::string::npos) {
        std::string left = Trim(trimmed.substr(0, eq_pos));
        if (!left.empty()) {
          bool has_bang = false;
          if (!left.empty() && left[0] == '!') {
            has_bang = true;
            left = Trim(left.substr(1));
          }
          if (!left.empty() && is_ident_start(left[0])) {
            bool valid = true;
            for (char c : left) {
              if (!is_ident_char(c)) {
                valid = false;
                break;
              }
            }
            if (valid) {
              std::string full_name = left;
              if (!has_bang && left[0] != '.') {
                std::string ns = get_current_namespace();
                if (!ns.empty()) full_name = ns + "_" + left;
              }
              DocumentState::SymbolEntry entry;
              entry.name = full_name;
              entry.kind = kSymbolConstant;
              entry.line = line_number;
              entry.detail = "define";
              entry.uri = uri;
              size_t column = line.find(left);
              entry.column = column == std::string::npos ? 0 : static_cast<int>(column);
              parsed.symbols.push_back(std::move(entry));
              line_start = line_end + 1;
              ++line_number;
              continue;
            }
          }
        }
      }

      size_t ws = trimmed.find_first_of(" \t");
      if (ws != std::string::npos) {
        std::string token = trimmed.substr(0, ws);
        std::string rest = Trim(trimmed.substr(ws + 1));
        if (!token.empty() && is_ident_start(token[0])) {
          bool valid = true;
          for (char c : token) {
            if (!is_ident_char(c)) {
              valid = false;
              break;
            }
          }
          if (valid && !rest.empty()) {
            std::string lower_rest = ToLower(rest);
            if (HasPrefixIgnoreCase(lower_rest, "db") ||
                HasPrefixIgnoreCase(lower_rest, "dw") ||
                HasPrefixIgnoreCase(lower_rest, "dl")) {
              std::string full_name = token;
              if (token[0] != '.') {
                std::string ns = get_current_namespace();
                if (!ns.empty()) full_name = ns + "_" + token;
              }
              DocumentState::SymbolEntry entry;
              entry.name = full_name;
              entry.kind = kSymbolConstant;
              entry.line = line_number;
              entry.detail = "data";
              entry.uri = uri;
              size_t column = line.find(token);
              entry.column = column == std::string::npos ? 0 : static_cast<int>(column);
              parsed.symbols.push_back(std::move(entry));
              line_start = line_end + 1;
              ++line_number;
              continue;
            }
          }
        }
      }

      size_t token_end = trimmed.find_first_of(" \t");
      std::string token = token_end == std::string::npos ? trimmed : trimmed.substr(0, token_end);
      if (token.size() > 1 && token.back() == ':') {
        std::string name = token.substr(0, token.size() - 1);
        if (!name.empty()) {
          std::string full_name = name;
          if (name[0] != '.') {
            std::string ns = get_current_namespace();
            if (!ns.empty()) full_name = ns + "_" + name;
          }
          DocumentState::SymbolEntry entry;
          entry.name = full_name;
          entry.kind = kSymbolConstant;
          entry.line = line_number;
          entry.detail = "label";
          entry.uri = uri;
          size_t column = line.find(name);
          entry.column = column == std::string::npos ? 0 : static_cast<int>(column);
          parsed.symbols.push_back(std::move(entry));
        }
      }
    }
    line_start = line_end + 1;
    ++line_number;
  }
  return parsed;
}

std::optional<WorkspaceState> BuildWorkspaceState(const json& params) {
  WorkspaceState state;
  if (params.contains("rootUri") && !params["rootUri"].is_null()) {
    state.root = UriToPath(params["rootUri"]);
  } else if (params.contains("rootPath") && !params["rootPath"].is_null()) {
    state.root = params["rootPath"].get<std::string>();
  }

  if (!state.root.empty()) {
    state.git_root = ResolveGitRoot(state.root);
    if (state.git_root.has_value()) {
      state.git_ignored_paths = LoadGitIgnoredPaths(*state.git_root);
    }

    fs::path config_path = state.root / "z3dk.toml";
    if (fs::exists(config_path)) {
      state.config = z3dk::LoadConfigIfExists(config_path.string());
      state.config_path = config_path;
      AddMainCandidatesFromConfig(*state.config, config_path.parent_path(), state.root, &state.main_candidates);
    }
    SeedMainCandidates(state.root, &state.main_candidates);
  }
  return state;
}

std::vector<std::string> ResolveIncludePaths(const z3dk::Config& config, const fs::path& config_dir) {
  std::vector<std::string> include_paths;
  for (const auto& raw : config.include_paths) {
    fs::path p(raw);
    if (!p.is_absolute()) {
      p = config_dir / p;
    }
    include_paths.push_back(p.lexically_normal().string());
  }
  return include_paths;
}

bool IsGitIgnoredPath(const WorkspaceState& workspace, const fs::path& path) {
  if (workspace.git_ignored_paths.empty()) return false;
  fs::path norm = z3lsp::NormalizePath(path);
  std::string s = norm.string();
  if (workspace.git_ignored_paths.count(s)) return true;
  
  // Check if any ancestor is ignored (directory ignore)
  fs::path parent = norm.parent_path();
  while (!parent.empty() && parent != workspace.root) {
    if (workspace.git_ignored_paths.count(parent.string())) return true;
    fs::path next = parent.parent_path();
    if (next == parent) break;
    parent = next;
  }
  return false;
}

bool ContainsOrgDirective(const std::string& text) {
  std::stringstream ss(text);
  std::string line;
  while (std::getline(ss, line)) {
    std::string stripped = z3lsp::StripAsmComment(line);
    std::string trimmed = z3lsp::Trim(stripped);
    std::string lower = z3lsp::ToLower(trimmed);
    if (z3lsp::HasPrefixIgnoreCase(lower, "org ") || 
        z3lsp::HasPrefixIgnoreCase(lower, "freespace ") ||
        z3lsp::HasPrefixIgnoreCase(lower, "freecode ") ||
        z3lsp::HasPrefixIgnoreCase(lower, "freedata ")) {
      return true;
    }
  }
  return false;
}

bool ParentIncludesChildAfterOrg(const fs::path& parent_path,
                                 const fs::path& child_path,
                                 const std::vector<std::string>& include_paths) {
  std::ifstream f(parent_path);
  if (!f) return false;
  
  fs::path base_dir = parent_path.parent_path();
  std::string line;
  bool found_org = false;
  fs::path child_norm = z3lsp::NormalizePath(child_path);
  
  while (std::getline(f, line)) {
    std::string stripped = z3lsp::StripAsmComment(line);
    std::string trimmed = z3lsp::Trim(stripped);
    std::string lower = z3lsp::ToLower(trimmed);
    
    if (z3lsp::HasPrefixIgnoreCase(lower, "org ") || 
        z3lsp::HasPrefixIgnoreCase(lower, "freespace ") ||
        z3lsp::HasPrefixIgnoreCase(lower, "freecode ") ||
        z3lsp::HasPrefixIgnoreCase(lower, "freedata ")) {
      found_org = true;
    }
    
    std::string include_val;
    if (z3lsp::ParseIncludeDirective(trimmed, &include_val)) {
      fs::path resolved;
      if (z3lsp::ResolveIncludePath(include_val, base_dir, include_paths, &resolved)) {
        if (z3lsp::NormalizePath(resolved) == child_norm) {
          return found_org;
        }
      }
    }
  }
  return false;
}

}  // namespace z3lsp
