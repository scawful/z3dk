#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"
#include "z3dk_core/assembler.h"
#include "z3dk_core/config.h"
#include "z3dk_core/opcode_table.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

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
  };
  std::vector<SymbolEntry> symbols;
  z3dk::SourceMap source_map;
};

struct WorkspaceState {
  fs::path root;
  std::optional<z3dk::Config> config;
  std::optional<fs::path> config_path;
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
  fs::file_time_type mtime;
  ParsedFile parsed;
};

std::unordered_map<std::string, CachedParse> g_parse_cache;

std::string UrlDecode(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size()) {
      char hex[3] = {static_cast<char>(text[i + 1]),
                     static_cast<char>(text[i + 2]), '\0'};
      char* end = nullptr;
      long value = std::strtol(hex, &end, 16);
      if (end != hex) {
        out.push_back(static_cast<char>(value));
        i += 2;
        continue;
      }
    }
    out.push_back(static_cast<char>(text[i]));
  }
  return out;
}

std::string UriToPath(const std::string& uri) {
  const std::string prefix = "file://";
  if (uri.rfind(prefix, 0) == 0) {
    std::string path = uri.substr(prefix.size());
    return UrlDecode(path);
  }
  return uri;
}

std::string PathToUri(const std::string& path) {
  std::string uri = "file://";
  uri += path;
  return uri;
}

std::string Trim(std::string_view text) {
  size_t start = 0;
  while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0) {
    ++start;
  }
  size_t end = text.size();
  while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
    --end;
  }
  return std::string(text.substr(start, end - start));
}

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

bool HasPrefixIgnoreCase(std::string_view text, std::string_view prefix) {
  if (prefix.empty() || text.size() < prefix.size()) {
    return false;
  }
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(text[i])) !=
        std::tolower(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

bool ContainsIgnoreCase(std::string_view text, std::string_view query) {
  if (query.empty()) {
    return true;
  }
  std::string lower_text(text);
  std::string lower_query(query);
  std::transform(lower_text.begin(), lower_text.end(), lower_text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  std::transform(lower_query.begin(), lower_query.end(), lower_query.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower_text.find(lower_query) != std::string::npos;
}

std::string ToLower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::optional<json> ReadMessage() {
  std::string line;
  int content_length = 0;
  while (std::getline(std::cin, line)) {
    if (line.rfind("Content-Length:", 0) == 0) {
      content_length = std::stoi(line.substr(15));
    } else if (line == "\r" || line.empty()) {
      break;
    }
  }

  if (content_length <= 0) {
  return std::nullopt;
}

  std::string payload(content_length, '\0');
  std::cin.read(payload.data(), content_length);
  try {
    return json::parse(payload);
  } catch (...) {
    return std::nullopt;
  }
}

void SendMessage(const json& message) {
  std::string payload = message.dump();
  std::cout << "Content-Length: " << payload.size() << "\r\n\r\n" << payload;
  std::cout.flush();
}

bool IsSymbolChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '.' ||
         c == '!' || c == '@';
}

std::optional<std::string> ExtractTokenAt(const std::string& text, int line,
                                          int character) {
  if (line < 0 || character < 0) {
    return std::nullopt;
  }
  int current_line = 0;
  size_t offset = 0;
  while (offset < text.size() && current_line < line) {
    if (text[offset] == '\n') {
      ++current_line;
    }
    ++offset;
  }
  if (current_line != line) {
    return std::nullopt;
  }
  size_t line_start = offset;
  size_t line_end = text.find('\n', line_start);
  if (line_end == std::string::npos) {
    line_end = text.size();
  }
  if (line_start + static_cast<size_t>(character) > line_end) {
    return std::nullopt;
  }
  size_t pos = line_start + static_cast<size_t>(character);
  if (pos >= text.size()) {
    return std::nullopt;
  }

  size_t left = pos;
  while (left > line_start && IsSymbolChar(text[left - 1])) {
    --left;
  }
  size_t right = pos;
  while (right < line_end && IsSymbolChar(text[right])) {
    ++right;
  }
  if (left == right) {
    return std::nullopt;
  }
  return text.substr(left, right - left);
}

std::optional<std::string> ExtractTokenPrefix(const std::string& text, int line,
                                              int character) {
  if (line < 0 || character < 0) {
    return std::nullopt;
  }
  int current_line = 0;
  size_t offset = 0;
  while (offset < text.size() && current_line < line) {
    if (text[offset] == '\n') {
      ++current_line;
    }
    ++offset;
  }
  if (current_line != line) {
    return std::nullopt;
  }
  size_t line_start = offset;
  size_t line_end = text.find('\n', line_start);
  if (line_end == std::string::npos) {
    line_end = text.size();
  }
  size_t pos = line_start + static_cast<size_t>(character);
  if (pos > line_end) {
    pos = line_end;
  }
  size_t left = pos;
  while (left > line_start && IsSymbolChar(text[left - 1])) {
    --left;
  }
  if (left == pos) {
    return std::nullopt;
  }
  return text.substr(left, pos - left);
}

bool ParseIncludeDirective(const std::string& trimmed, std::string* out_path) {
  if (!out_path) {
    return false;
  }
  std::string lower = ToLower(trimmed);
  std::string keyword;
  if (HasPrefixIgnoreCase(lower, "incsrc")) {
    keyword = "incsrc";
  } else if (HasPrefixIgnoreCase(lower, "include")) {
    keyword = "include";
  } else {
    return false;
  }
  std::string rest = Trim(trimmed.substr(keyword.size()));
  if (rest.empty()) {
    return false;
  }
  if (rest.front() == '"') {
    size_t end = rest.find('"', 1);
    if (end == std::string::npos || end <= 1) {
      return false;
    }
    *out_path = rest.substr(1, end - 1);
    return true;
  }
  size_t end = rest.find_first_of(" \t");
  *out_path = rest.substr(0, end);
  return !out_path->empty();
}

bool ParseIncdirDirective(const std::string& trimmed, std::string* out_path) {
  if (!out_path) {
    return false;
  }
  std::string lower = ToLower(trimmed);
  if (!HasPrefixIgnoreCase(lower, "incdir")) {
    return false;
  }
  std::string rest = Trim(trimmed.substr(6));
  if (rest.empty()) {
    return false;
  }
  if (rest.front() == '"') {
    size_t end = rest.find('"', 1);
    if (end == std::string::npos || end <= 1) {
      return false;
    }
    *out_path = rest.substr(1, end - 1);
    return true;
  }
  size_t end = rest.find_first_of(" \t");
  *out_path = rest.substr(0, end);
  return !out_path->empty();
}

std::optional<std::string> ResolveIncdirPath(const std::string& raw,
                                             const fs::path& base_dir) {
  if (raw.empty()) {
    return std::nullopt;
  }
  fs::path candidate = raw;
  if (!candidate.is_absolute()) {
    if (base_dir.empty()) {
      return std::nullopt;
    }
    candidate = base_dir / candidate;
  }
  candidate = candidate.lexically_normal();
  if (!fs::exists(candidate)) {
    return std::nullopt;
  }
  return candidate.string();
}

bool ResolveIncludePath(const std::string& raw,
                        const fs::path& base_dir,
                        const std::vector<std::string>& include_paths,
                        fs::path* out_path) {
  if (!out_path) {
    return false;
  }
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

ParsedFile ParseFileText(const std::string& text, const std::string& uri) {
  ParsedFile parsed;
  const int kSymbolFunction = 12;
  const int kSymbolConstant = 21;

  size_t line_start = 0;
  int line_number = 0;
  while (line_start <= text.size()) {
    size_t line_end = text.find('\n', line_start);
    if (line_end == std::string::npos) {
      line_end = text.size();
    }
    std::string line = text.substr(line_start, line_end - line_start);
    std::string stripped = StripAsmComment(line);
    std::string trimmed = Trim(stripped);
    if (!trimmed.empty()) {
      std::string incdir_path;
      if (ParseIncdirDirective(trimmed, &incdir_path)) {
        parsed.events.push_back({IncludeEvent::Type::kIncdir, incdir_path});
      }

      std::string include_path;
      if (ParseIncludeDirective(trimmed, &include_path)) {
        parsed.events.push_back({IncludeEvent::Type::kInclude, include_path});
      }

      if (HasPrefixIgnoreCase(trimmed, "macro")) {
        std::string rest = Trim(trimmed.substr(5));
        if (!rest.empty()) {
          size_t end = rest.find_first_of(" \t(");
          std::string name = rest.substr(0, end);
          if (!name.empty()) {
            DocumentState::SymbolEntry entry;
            entry.name = name;
            entry.kind = kSymbolFunction;
            entry.line = line_number;
            entry.detail = "macro";
            entry.uri = uri;
            size_t column = line.find(name);
            entry.column = column == std::string::npos ? 0 : static_cast<int>(column);
            parsed.symbols.push_back(std::move(entry));
            line_start = line_end + 1;
            ++line_number;
            continue;
          }
        }
      }

      if (trimmed[0] == '!') {
        size_t i = 1;
        while (i < trimmed.size() && IsSymbolChar(trimmed[i])) {
          ++i;
        }
        std::string name = trimmed.substr(1, i - 1);
        if (!name.empty()) {
          DocumentState::SymbolEntry entry;
          entry.name = name;
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
          DocumentState::SymbolEntry entry;
          entry.name = name;
          entry.kind = kSymbolConstant;
          entry.line = line_number;
          entry.detail = "define";
          entry.uri = uri;
          size_t column = line.find(name);
          entry.column = column == std::string::npos ? 0 : static_cast<int>(column);
          parsed.symbols.push_back(std::move(entry));
        }
      }

      size_t token_end = trimmed.find_first_of(" \t");
      std::string token = token_end == std::string::npos ? trimmed : trimmed.substr(0, token_end);
      if (token.size() > 1 && token.back() == ':') {
        std::string name = token.substr(0, token.size() - 1);
        if (!name.empty()) {
          DocumentState::SymbolEntry entry;
          entry.name = name;
          entry.kind = kSymbolFunction;
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

bool LoadParsedFile(const fs::path& path, ParsedFile* parsed) {
  if (!parsed) {
    return false;
  }
  std::error_code ec;
  fs::file_time_type mtime = fs::last_write_time(path, ec);
  if (ec) {
    return false;
  }
  std::string key = path.string();
  auto it = g_parse_cache.find(key);
  if (it != g_parse_cache.end() && it->second.mtime == mtime) {
    *parsed = it->second.parsed;
    return true;
  }

  std::ifstream file(path);
  if (!file.is_open()) {
    return false;
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  ParsedFile parsed_file = ParseFileText(buffer.str(), PathToUri(key));
  g_parse_cache[key] = {mtime, parsed_file};
  *parsed = parsed_file;
  return true;
}

void CollectSymbolsRecursiveParsed(const ParsedFile& parsed,
                                   const fs::path& base_dir,
                                   const std::vector<std::string>& include_paths,
                                   const std::string& uri,
                                   int depth,
                                   std::unordered_set<std::string>* visited,
                                   std::vector<DocumentState::SymbolEntry>* symbols) {
  if (!symbols || !visited) {
    return;
  }
  if (depth > 16 || visited->size() > 128) {
    return;
  }

  for (const auto& entry : parsed.symbols) {
    DocumentState::SymbolEntry copy = entry;
    if (copy.uri.empty()) {
      copy.uri = uri;
    }
    symbols->push_back(std::move(copy));
  }

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
    if (!ResolveIncludePath(event.path, base_dir, include_paths_current, &resolved)) {
      continue;
    }
    std::error_code ec;
    fs::path absolute = fs::absolute(resolved, ec);
    if (ec) {
      continue;
    }
    std::string key = absolute.string();
    if (!visited->insert(key).second) {
      continue;
    }

    ParsedFile child;
    if (!LoadParsedFile(absolute, &child)) {
      continue;
    }
    CollectSymbolsRecursiveParsed(child,
                                  absolute.parent_path(),
                                  include_paths_current,
                                  PathToUri(key),
                                  depth + 1,
                                  visited,
                                  symbols);
  }
}

std::vector<DocumentState::SymbolEntry> ExtractSymbolsFromText(
    const std::string& text,
    const fs::path& doc_path,
    const std::vector<std::string>& include_paths,
    const std::string& uri) {
  std::vector<DocumentState::SymbolEntry> symbols;
  std::unordered_set<std::string> visited;
  std::error_code ec;
  fs::path absolute = fs::absolute(doc_path, ec);
  if (!ec) {
    visited.insert(absolute.string());
  }
  ParsedFile parsed = ParseFileText(text, uri);
  CollectSymbolsRecursiveParsed(parsed,
                                doc_path.parent_path(),
                                include_paths,
                                uri,
                                0,
                                &visited,
                                &symbols);
  return symbols;
}

std::vector<std::string> ResolveIncludePaths(const z3dk::Config& config,
                                             const fs::path& base_dir) {
  std::vector<std::string> out;
  out.reserve(config.include_paths.size());
  for (const auto& path : config.include_paths) {
    fs::path resolved = path;
    if (!resolved.is_absolute()) {
      resolved = base_dir / resolved;
    }
    out.push_back(resolved.lexically_normal().string());
  }
  return out;
}

std::optional<WorkspaceState> BuildWorkspaceState(const json& params) {
  WorkspaceState state;
  if (params.contains("rootUri")) {
    state.root = UriToPath(params["rootUri"].get<std::string>());
  } else if (params.contains("rootPath")) {
    state.root = params["rootPath"].get<std::string>();
  }

  if (!state.root.empty()) {
    fs::path config_path = state.root / "z3dk.toml";
    if (fs::exists(config_path)) {
      state.config_path = config_path;
      state.config = z3dk::LoadConfigIfExists(config_path.string());
    }
  }

  return state;
}

void PublishDiagnostics(const DocumentState& doc) {
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
  SendMessage(message);
}

json BuildDocumentSymbols(const DocumentState& doc) {
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

json BuildWorkspaceSymbols(const std::unordered_map<std::string, DocumentState>& documents,
                           const std::string& query) {
  json result = json::array();
  for (const auto& pair : documents) {
    const DocumentState& doc = pair.second;
    for (const auto& symbol : doc.symbols) {
      if (!ContainsIgnoreCase(symbol.name, query)) {
        continue;
      }
      json entry;
      entry["name"] = symbol.name;
      entry["kind"] = symbol.kind;
      if (!symbol.detail.empty()) {
        entry["containerName"] = symbol.detail;
      }
      std::string uri = symbol.uri.empty() ? doc.uri : symbol.uri;
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

DocumentState AnalyzeDocument(const DocumentState& doc,
                              const WorkspaceState& workspace) {
  DocumentState updated = doc;

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

  std::vector<std::string> include_paths;
  if (!config.include_paths.empty()) {
    include_paths = ResolveIncludePaths(config, config_dir);
  }
  include_paths.push_back(fs::path(doc.path).parent_path().string());
  std::vector<std::string> include_paths_for_index = include_paths;

  z3dk::AssembleOptions options;
  options.patch_path = doc.path;
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
  if (config.rom_size.has_value() && *config.rom_size > 0) {
    options.rom_data.resize(static_cast<size_t>(*config.rom_size), 0);
  }
  options.memory_files.push_back({doc.path, doc.text});

  z3dk::Assembler assembler;
  z3dk::AssembleResult result = assembler.Assemble(options);
  updated.diagnostics = result.diagnostics;
  updated.labels = result.labels;
  updated.defines = result.defines;
  updated.source_map = result.source_map;
  updated.symbols = ExtractSymbolsFromText(doc.text,
                                           fs::path(doc.path),
                                           include_paths_for_index,
                                           doc.uri);

  return updated;
}

std::optional<json> HandleDefinition(const DocumentState& doc, const json& params) {
  if (!params.contains("textDocument") || !params.contains("position")) {
    return std::nullopt;
  }
  auto position = params["position"];
  int line = position.value("line", 0);
  int character = position.value("character", 0);
  auto token = ExtractTokenAt(doc.text, line, character);
  if (!token.has_value()) {
    return json(nullptr);
  }

  auto label_it = std::find_if(doc.labels.begin(), doc.labels.end(),
                               [&](const z3dk::Label& label) {
                                 return label.name == *token;
                               });
  if (label_it == doc.labels.end()) {
    return json(nullptr);
  }

  std::unordered_map<int, std::string> file_map;
  for (const auto& file : doc.source_map.files) {
    file_map[file.id] = file.path;
  }

  for (const auto& entry : doc.source_map.entries) {
    if (entry.address != label_it->address) {
      continue;
    }
    auto file_it = file_map.find(entry.file_id);
    if (file_it == file_map.end()) {
      continue;
    }
    int target_line = std::max(0, entry.line - 1);
    json location;
    location["uri"] = PathToUri(file_it->second);
    location["range"] = {
        {"start", {{"line", target_line}, {"character", 0}}},
        {"end", {{"line", target_line}, {"character", 0}}},
    };
    return json::array({location});
  }

  return json(nullptr);
}

std::optional<json> HandleHover(const DocumentState& doc, const json& params) {
  if (!params.contains("textDocument") || !params.contains("position")) {
    return std::nullopt;
  }
  auto position = params["position"];
  int line = position.value("line", 0);
  int character = position.value("character", 0);
  auto token = ExtractTokenAt(doc.text, line, character);
  if (!token.has_value()) {
    return json(nullptr);
  }

  auto label_it = std::find_if(doc.labels.begin(), doc.labels.end(),
                               [&](const z3dk::Label& label) {
                                 return label.name == *token;
                               });
  if (label_it == doc.labels.end()) {
    return json(nullptr);
  }

  std::ostringstream hover_text;
  hover_text << label_it->name << " = $" << std::hex << std::uppercase
             << label_it->address;

  json hover;
  hover["contents"] = { {"kind", "plaintext"}, {"value", hover_text.str()} };
  return hover;
}

json BuildCompletionItems(const DocumentState& doc, const std::string& prefix) {
  if (prefix.empty()) {
    return json::array();
  }

  json items = json::array();
  std::unordered_set<std::string> seen;
  auto matches_prefix = [&](std::string_view name) {
    return HasPrefixIgnoreCase(name, prefix);
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
      "if", "incbin", "incsrc", "include", "incdir", "lorom", "exlorom",
      "exhirom", "macro", "namespace", "org", "pad", "padbyte", "padword",
      "pc2snes", "print", "pullpc", "pushpc", "pushns", "popns", "snes2pc",
      "struct", "table", "undef", "warn", "warning", "while", "for",
  };

  for (const char* directive : kDirectives) {
    if (!matches_prefix(directive)) {
      continue;
    }
    push_item(directive, 14, "directive");
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
      "BLT", "BMI", "BNE", "BPL", "BRA", "BVC", "BVS", "CACHE", "CMODE", "CMP",
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
  };

  const int kTypeKeyword = 3;
  const int kTypeString = 4;
  const int kTypeNumber = 5;

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
  if (keyword_set.empty()) {
    static const char* const kDirectives[] = {
        "arch", "autoclean", "bank", "bankbyte", "base", "cleartable", "cmode",
        "db", "dw", "dl", "dd", "dq", "define", "elif", "elseif", "else", "endif",
        "endmacro", "endstruct", "endwhile", "endfor", "error", "fill",
        "fillbyte", "fillword", "freecode", "freedata", "freespace", "hirom",
        "if", "incbin", "incsrc", "include", "incdir", "lorom", "exlorom",
        "exhirom", "macro", "namespace", "org", "pad", "padbyte", "padword",
        "pc2snes", "print", "pullpc", "pushpc", "pushns", "popns", "snes2pc",
        "struct", "table", "undef", "warn", "warning", "while", "for",
    };
    for (const char* directive : kDirectives) {
      keyword_set.insert(ToLower(directive));
    }

    for (int i = 0; i < 256; ++i) {
      const auto& info = z3dk::GetOpcodeInfo(static_cast<uint8_t>(i));
      if (info.mnemonic != nullptr && info.mnemonic[0] != '\0') {
        keyword_set.insert(ToLower(info.mnemonic));
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
      keyword_set.insert(ToLower(opcode));
    }

    static const char* const kOpcodesSuperFx[] = {
        "ADC", "ADD", "AND", "ASR", "BCC", "BCS", "BEQ", "BGE", "BGT", "BLE",
        "BLT", "BMI", "BNE", "BPL", "BRA", "BVC", "BVS", "CACHE", "CMODE", "CMP",
        "DEC", "DIV2", "FMULT", "FROM", "GETB", "GETBH", "GETBL", "GETBS", "GETC",
        "HIB", "IBT", "INC", "IWT", "JMP", "LMS", "LM", "LSR", "MERGE", "MOV",
        "MOVE", "MULT", "NOP", "NOT", "OR", "PLOT", "RADC", "ROL", "ROMB", "ROR",
        "RPLOT", "SBC", "SBK", "SEXB", "SEXT", "SM", "STW", "SUB", "SWAP", "TO",
        "UMULT", "WITH",
    };
    for (const char* opcode : kOpcodesSuperFx) {
      keyword_set.insert(ToLower(opcode));
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
    std::string code = StripAsmComment(line);

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
      std::string token_lower = ToLower(token);
      if (keyword_set.find(token_lower) != keyword_set.end()) {
        Token keyword;
        keyword.line = line_number;
        keyword.column = static_cast<int>(token_pos);
        keyword.length = static_cast<int>(token.size());
        keyword.type = kTypeKeyword;
        tokens.push_back(keyword);
      }
    }

    for (size_t i = 0; i < code.size();) {
      if (in_string(i)) {
        ++i;
        continue;
      }
      char c = code[i];
      if (c == '$' || c == '%') {
        size_t start = i;
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
        if (digits > 0) {
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

  WorkspaceState workspace;
  std::unordered_map<std::string, DocumentState> documents;
  bool shutting_down = false;

  while (std::cin.good()) {
    auto message = ReadMessage();
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
      auto workspace_state = BuildWorkspaceState(params);
      if (workspace_state.has_value()) {
        workspace = *workspace_state;
      }
      json capabilities = {
          {"capabilities",
           {{"textDocumentSync", 1},
            {"definitionProvider", true},
            {"hoverProvider", true},
            {"completionProvider",
             {{"triggerCharacters", json::array({"!", ".", "@"})}}},
            {"documentSymbolProvider", true},
            {"workspaceSymbolProvider", true},
            {"semanticTokensProvider",
             {{"legend",
               {{"tokenTypes", json::array({"function", "macro", "variable", "keyword", "string", "number"})},
                {"tokenModifiers", json::array()}}},
              {"full", true}}}}},
      };
      json response = {
          {"jsonrpc", "2.0"},
          {"id", request["id"]},
          {"result", capabilities},
      };
      SendMessage(response);
      continue;
    }

    if (method == "shutdown") {
      shutting_down = true;
      json response = {
          {"jsonrpc", "2.0"},
          {"id", request["id"]},
          {"result", nullptr},
      };
      SendMessage(response);
      continue;
    }

    if (method == "exit") {
      return shutting_down ? 0 : 1;
    }

    if (method == "textDocument/didOpen") {
      auto params = request.value("params", json::object());
      auto text_doc = params.value("textDocument", json::object());
      DocumentState doc;
      doc.uri = text_doc.value("uri", "");
      doc.path = UriToPath(doc.uri);
      doc.text = text_doc.value("text", "");
      doc.version = text_doc.value("version", 0);
      doc = AnalyzeDocument(doc, workspace);
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
      it->second = AnalyzeDocument(it->second, workspace);
      PublishDiagnostics(it->second);
      continue;
    }

    if (method == "textDocument/didClose") {
      auto params = request.value("params", json::object());
      auto text_doc = params.value("textDocument", json::object());
      std::string uri = text_doc.value("uri", "");
      auto it = documents.find(uri);
      if (it != documents.end()) {
        DocumentState cleared = it->second;
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
      SendMessage(response);
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
      SendMessage(response);
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
      SendMessage(response);
      continue;
    }

    if (method == "workspace/symbol") {
      auto params = request.value("params", json::object());
      std::string query = params.value("query", "");
      json response = {
          {"jsonrpc", "2.0"},
          {"id", request["id"]},
          {"result", BuildWorkspaceSymbols(documents, query)},
      };
      SendMessage(response);
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
      SendMessage(response);
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
        auto prefix = ExtractTokenPrefix(it->second.text, line, character);
        if (prefix.has_value()) {
          response["result"] = BuildCompletionItems(it->second, *prefix);
        }
      }
      SendMessage(response);
      continue;
    }
  }

  return 0;
}
