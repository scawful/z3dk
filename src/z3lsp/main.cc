#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <queue>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"
#include "z3dk_core/assembler.h"
#include "z3dk_core/config.h"
#include "z3dk_core/lint.h"
#include "z3dk_core/opcode_descriptions.h"
#include "z3dk_core/opcode_table.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

bool g_log_enabled = true;
std::string g_log_path;

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

  // Debouncing state
  std::chrono::steady_clock::time_point last_change;
  bool needs_analysis = false;

  void BuildLookupMaps() {
    label_map.clear();
    define_map.clear();
    for (const auto& label : labels) {
      label_map[label.name] = &label;
    }
    for (const auto& define : defines) {
      define_map[define.name] = &define;
    }
    address_to_label_map.clear();
    for (const auto& label : labels) {
      if (address_to_label_map.find(label.address) == address_to_label_map.end()) {
        address_to_label_map[label.address] = label.name;
      }
    }
  }

  std::unordered_map<uint32_t, std::string> address_to_label_map;
};

struct WorkspaceState {
  fs::path root;
  std::optional<z3dk::Config> config;
  std::optional<fs::path> config_path;
  std::optional<fs::path> git_root;
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
  fs::file_time_type mtime;
  ParsedFile parsed;
};

#include <dirent.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

struct ProjectGraph {
  std::unordered_map<std::string, std::unordered_set<std::string>> child_to_parents;
  std::unordered_map<std::string, std::unordered_set<std::string>> parent_to_children;

  void RegisterDependency(const std::string& parent_uri, const std::string& child_uri) {
    child_to_parents[child_uri].insert(parent_uri);
    parent_to_children[parent_uri].insert(child_uri);
  }

  std::unordered_set<std::string> GetParents(const std::string& uri) const {
    auto it = child_to_parents.find(uri);
    if (it == child_to_parents.end()) {
      return {};
    }
    return it->second;
  }

  std::unordered_map<std::string, int> GetAncestorDistances(const std::string& uri) const {
    std::unordered_map<std::string, int> distances;
    if (uri.empty()) {
      return distances;
    }
    std::queue<std::string> pending;
    distances[uri] = 0;
    pending.push(uri);
    while (!pending.empty()) {
      std::string current = pending.front();
      pending.pop();
      int current_distance = distances[current];
      auto it = child_to_parents.find(current);
      if (it == child_to_parents.end()) {
        continue;
      }
      for (const auto& parent : it->second) {
        if (distances.find(parent) != distances.end()) {
          continue;
        }
        distances[parent] = current_distance + 1;
        pending.push(parent);
      }
    }
    return distances;
  }

  std::string SelectRoot(const std::string& uri,
                         const std::unordered_set<std::string>& preferred_roots) const {
    if (uri.empty()) {
      return uri;
    }
    auto distances = GetAncestorDistances(uri);
    if (distances.empty()) {
      return uri;
    }

    auto pick_best = [&](const std::vector<std::string>& candidates) -> std::string {
      std::string best;
      int best_distance = std::numeric_limits<int>::max();
      for (const auto& candidate : candidates) {
        auto it = distances.find(candidate);
        if (it == distances.end()) {
          continue;
        }
        int distance = it->second;
        if (distance < best_distance ||
            (distance == best_distance && (best.empty() || candidate < best))) {
          best = candidate;
          best_distance = distance;
        }
      }
      return best.empty() ? uri : best;
    };

    if (!preferred_roots.empty()) {
      std::vector<std::string> preferred;
      preferred.reserve(preferred_roots.size());
      for (const auto& entry : distances) {
        if (preferred_roots.count(entry.first)) {
          preferred.push_back(entry.first);
        }
      }
      if (!preferred.empty()) {
        return pick_best(preferred);
      }
    }

    std::vector<std::string> roots;
    roots.reserve(distances.size());
    for (const auto& entry : distances) {
      auto it = child_to_parents.find(entry.first);
      if (it == child_to_parents.end() || it->second.empty()) {
        roots.push_back(entry.first);
      }
    }
    if (!roots.empty()) {
      return pick_best(roots);
    }
    return uri;
  }
};

ProjectGraph g_project_graph;

std::unordered_map<std::string, CachedParse> g_parse_cache;
struct RomCacheEntry {
  fs::file_time_type mtime;
  std::vector<uint8_t> data;
};
std::unordered_map<std::string, RomCacheEntry> g_rom_cache;

std::string PathToUri(const std::string& path);

std::string ToHexString(uint32_t value, int width) {
  std::ostringstream ss;
  ss << std::hex << std::uppercase << std::setfill('0') << std::setw(width) << value;
  return ss.str();
}

fs::path NormalizePath(const fs::path& path) {
  std::error_code ec;
  fs::path absolute = fs::absolute(path, ec);
  if (ec) {
    return path.lexically_normal();
  }
  return absolute.lexically_normal();
}

fs::path ResolveConfigPath(const std::string& raw,
                           const fs::path& config_dir,
                           const fs::path& workspace_root) {
  if (raw.empty()) {
    return {};
  }
  fs::path candidate = raw;
  if (candidate.is_absolute()) {
    return candidate.lexically_normal();
  }
  if (!config_dir.empty()) {
    return (config_dir / candidate).lexically_normal();
  }
  if (!workspace_root.empty()) {
    return (workspace_root / candidate).lexically_normal();
  }
  return candidate.lexically_normal();
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

bool AddMainCandidatesFromConfig(const z3dk::Config& config,
                                 const fs::path& config_dir,
                                 const fs::path& workspace_root,
                                 std::unordered_set<std::string>* out) {
  if (!out || config.main_files.empty()) {
    return false;
  }
  bool added = false;
  for (const auto& entry : config.main_files) {
    if (entry.empty()) {
      continue;
    }
    fs::path resolved = ResolveConfigPath(entry, config_dir, workspace_root);
    if (resolved.empty()) {
      continue;
    }
    std::error_code ec;
    if (!fs::exists(resolved, ec)) {
      continue;
    }
    out->insert(PathToUri(NormalizePath(resolved).string()));
    added = true;
  }
  return added;
}

bool LoadRomData(const fs::path& path, std::vector<uint8_t>* out) {
  if (!out || path.empty()) {
    return false;
  }
  std::error_code ec;
  if (!fs::exists(path, ec) || ec) {
    return false;
  }

  fs::path normalized = NormalizePath(path);
  std::string key = normalized.string();
  fs::file_time_type mtime = fs::last_write_time(normalized, ec);
  if (ec) {
    return false;
  }

  auto it = g_rom_cache.find(key);
  if (it != g_rom_cache.end() && it->second.mtime == mtime) {
    *out = it->second.data;
    return true;
  }

  std::ifstream file(normalized, std::ios::binary);
  if (!file.is_open()) {
    return false;
  }
  file.seekg(0, std::ios::end);
  std::streamoff size = file.tellg();
  if (size <= 0) {
    return false;
  }
  file.seekg(0, std::ios::beg);
  std::vector<uint8_t> data(static_cast<size_t>(size));
  if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
    return false;
  }
  g_rom_cache[key] = {mtime, std::move(data)};
  *out = g_rom_cache[key].data;
  return true;
}

bool EndsWithPath(const fs::path& full, const fs::path& suffix) {
  std::string full_str = full.generic_string();
  std::string suffix_str = suffix.generic_string();
  if (suffix_str.empty()) {
    return false;
  }
  if (full_str == suffix_str) {
    return true;
  }
  if (full_str.size() <= suffix_str.size()) {
    return false;
  }
  if (full_str.compare(full_str.size() - suffix_str.size(), suffix_str.size(), suffix_str) != 0) {
    return false;
  }
  char sep = full_str[full_str.size() - suffix_str.size() - 1];
  return sep == '/';
}

bool PathMatchesDocumentPath(const std::string& candidate_path,
                             const fs::path& doc_path,
                             const fs::path& analysis_root_dir,
                             const fs::path& workspace_root) {
  if (candidate_path.empty()) {
    return false;
  }
  fs::path doc_norm = NormalizePath(doc_path);
  fs::path diag_path = fs::path(candidate_path);
  if (diag_path.is_absolute()) {
    return NormalizePath(diag_path) == doc_norm;
  }
  if (!analysis_root_dir.empty()) {
    if (NormalizePath(analysis_root_dir / diag_path) == doc_norm) {
      return true;
    }
  }
  if (!workspace_root.empty()) {
    if (NormalizePath(workspace_root / diag_path) == doc_norm) {
      return true;
    }
  }
  return EndsWithPath(doc_norm, diag_path);
}

bool DiagnosticMatchesDocument(const z3dk::Diagnostic& diag,
                               const fs::path& doc_path,
                               const fs::path& analysis_root_dir,
                               const fs::path& workspace_root,
                               bool doc_is_root) {
  if (diag.filename.empty()) {
    return doc_is_root;
  }
  return PathMatchesDocumentPath(diag.filename, doc_path, analysis_root_dir, workspace_root);
}

std::string ExtractMissingLabel(const std::string& message) {
  const std::string needle = "Label '";
  size_t start = message.find(needle);
  if (start != std::string::npos) {
    start += needle.size();
    size_t end = message.find('\'', start);
    if (end != std::string::npos && end > start) {
      return message.substr(start, end - start);
    }
  }
  const std::string needle2 = "Label ";
  start = message.find(needle2);
  if (start != std::string::npos) {
    start += needle2.size();
    size_t end = message.find(' ', start);
    if (end != std::string::npos && end > start) {
      return message.substr(start, end - start);
    }
  }
  return {};
}

std::string SelectRootUri(const std::string& uri, const WorkspaceState& workspace) {
  return g_project_graph.SelectRoot(uri, workspace.main_candidates);
}

class MesenClient {
 public:
  MesenClient() : socket_fd_(-1) {}
  ~MesenClient() { Disconnect(); }

  bool Connect() {
    if (IsConnected()) return true;
    
    std::string path = FindLatestSocket();
    if (path.empty()) return false;

    socket_path_ = path;
    socket_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket_fd_ < 0) return false;

    // Set non-blocking for connect timeout
    int flags = fcntl(socket_fd_, F_GETFL, 0);
    fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(socket_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
      if (errno != EINPROGRESS) {
        close(socket_fd_);
        socket_fd_ = -1;
        return false;
      }
      
      // Wait for connection with 100ms timeout
      struct timeval tv;
      tv.tv_sec = 0;
      tv.tv_usec = 100000;
      fd_set wset;
      FD_ZERO(&wset);
      FD_SET(socket_fd_, &wset);
      if (select(socket_fd_ + 1, NULL, &wset, NULL, &tv) <= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
        return false;
      }
    }

    // Back to blocking for simple synchronous calls (with timeouts)
    fcntl(socket_fd_, F_SETFL, flags);
    
    struct timeval rtv;
    rtv.tv_sec = 0;
    rtv.tv_usec = 200000; // 200ms read timeout
    setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));

    return true;
  }

  void Disconnect() {
    if (socket_fd_ >= 0) {
      close(socket_fd_);
      socket_fd_ = -1;
    }
    socket_path_.clear();
  }

  bool IsConnected() const { return socket_fd_ >= 0; }

  std::optional<uint8_t> ReadByte(uint32_t addr) {
    if (!Connect()) return std::nullopt;

    json cmd = {{"type", "READ"}, {"addr", "0x" + ToHexString(addr, 6)}};
    auto response = SendCommand(cmd);
    if (response.has_value() && response->value("success", false)) {
      return response->value("data", 0);
    }
    return std::nullopt;
  }

  std::optional<json> SendCommand(const json& cmd) {
    if (!IsConnected()) return std::nullopt;

    std::string request = cmd.dump() + "\n";
    if (send(socket_fd_, request.c_str(), request.length(), 0) < 0) {
      Disconnect();
      return std::nullopt;
    }

    char buffer[4096];
    std::string response;
    ssize_t received = recv(socket_fd_, buffer, sizeof(buffer), 0);
    if (received <= 0) {
      Disconnect();
      return std::nullopt;
    }
    response.append(buffer, received);
    
    try {
      return json::parse(response);
    } catch (const std::exception& e) {
      Log("MesenClient JSON parse error: " + std::string(e.what()));
      return std::nullopt;
    } catch (...) {
      Log("MesenClient JSON parse error: unknown exception");
      return std::nullopt;
    }
  }

 private:
  std::string FindLatestSocket() {
    DIR* dir = opendir("/tmp");
    if (!dir) return "";

    std::string latest;
    long latest_mtime = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
      std::string name = entry->d_name;
      if (name.find("mesen2-") == 0 && name.find(".sock") != std::string::npos) {
        std::string full_path = "/tmp/" + name;
        struct stat st;
        if (stat(full_path.c_str(), &st) == 0) {
          if (st.st_mtime > latest_mtime) {
            latest_mtime = st.st_mtime;
            latest = full_path;
          }
        }
      }
    }
    closedir(dir);
    return latest;
  }

  int socket_fd_;
  std::string socket_path_;
};

// Global Mesen client instance
MesenClient g_mesen;

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

std::string QuoteShellArg(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size() + 2);
  escaped.push_back('"');
  for (char c : value) {
    if (c == '"') {
      escaped.push_back('\\');
    }
    escaped.push_back(c);
  }
  escaped.push_back('"');
  return escaped;
}

std::string RunCommandCapture(const std::string& command) {
  std::string output;
#ifdef _WIN32
  FILE* pipe = _popen(command.c_str(), "rb");
#else
  FILE* pipe = popen(command.c_str(), "r");
#endif
  if (!pipe) {
    return output;
  }
  char buffer[4096];
  while (true) {
    size_t read = std::fread(buffer, 1, sizeof(buffer), pipe);
    if (read == 0) {
      break;
    }
    output.append(buffer, read);
  }
#ifdef _WIN32
  _pclose(pipe);
#else
  pclose(pipe);
#endif
  return output;
}

std::optional<fs::path> ResolveGitRoot(const fs::path& start) {
  if (start.empty()) {
    return std::nullopt;
  }
  std::string cmd = "git -C " + QuoteShellArg(start.string()) + " rev-parse --show-toplevel";
#ifdef _WIN32
  cmd += " 2>nul";
#else
  cmd += " 2>/dev/null";
#endif
  std::string output = RunCommandCapture(cmd);
  std::string trimmed = Trim(output);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  return fs::path(trimmed);
}

std::unordered_set<std::string> LoadGitIgnoredPaths(const fs::path& git_root) {
  std::unordered_set<std::string> ignored;
  if (git_root.empty()) {
    return ignored;
  }
  std::string cmd = "git -C " + QuoteShellArg(git_root.string()) +
                    " ls-files -o -i --exclude-standard -z";
#ifdef _WIN32
  cmd += " 2>nul";
#else
  cmd += " 2>/dev/null";
#endif
  std::string output = RunCommandCapture(cmd);
  if (output.empty()) {
    return ignored;
  }
  size_t offset = 0;
  while (offset < output.size()) {
    size_t end = output.find('\0', offset);
    if (end == std::string::npos) {
      end = output.size();
    }
    std::string rel = output.substr(offset, end - offset);
    if (!rel.empty()) {
      fs::path absolute = NormalizePath(git_root / rel);
      ignored.insert(absolute.string());
    }
    offset = end + 1;
  }
  return ignored;
}

bool IsPathUnderRoot(const fs::path& path, const fs::path& root) {
  if (path.empty() || root.empty()) {
    return false;
  }
  fs::path norm_path = NormalizePath(path);
  fs::path norm_root = NormalizePath(root);
  auto pit = norm_path.begin();
  auto rit = norm_root.begin();
  for (; rit != norm_root.end(); ++rit, ++pit) {
    if (pit == norm_path.end() || *pit != *rit) {
      return false;
    }
  }
  return true;
}

bool IsGitIgnoredPath(const WorkspaceState& workspace, const fs::path& path) {
  if (workspace.git_ignored_paths.empty()) {
    return false;
  }
  if (!workspace.git_root.has_value()) {
    return false;
  }
  if (!IsPathUnderRoot(path, *workspace.git_root)) {
    return false;
  }
  std::string normalized = NormalizePath(path).string();
  return workspace.git_ignored_paths.find(normalized) != workspace.git_ignored_paths.end();
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
  if (query.size() > text.size()) {
    return false;
  }
  // In-place comparison without string allocation
  for (size_t i = 0; i + query.size() <= text.size(); ++i) {
    bool match = true;
    for (size_t j = 0; j < query.size(); ++j) {
      if (std::tolower(static_cast<unsigned char>(text[i + j])) !=
          std::tolower(static_cast<unsigned char>(query[j]))) {
        match = false;
        break;
      }
    }
    if (match) {
      return true;
    }
  }
  return false;
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
  } catch (const std::exception& e) {
    Log("LSP ReadMessage JSON parse error: " + std::string(e.what()));
    return std::nullopt;
  } catch (...) {
    Log("LSP ReadMessage JSON parse error: unknown exception");
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

bool IsMainFileName(const fs::path& path) {
  std::string stem = path.stem().string();
  if (stem.empty()) {
    return false;
  }
  std::string lower = ToLower(stem);
  if (lower == "main") {
    return true;
  }
  if (lower.size() > 5 && lower.rfind("_main") == lower.size() - 5) {
    return true;
  }
  if (lower.size() > 5 && lower.rfind("-main") == lower.size() - 5) {
    return true;
  }
  return false;
}

void SeedMainCandidates(const fs::path& root,
                        std::unordered_set<std::string>* main_candidates) {
  if (!main_candidates || root.empty()) {
    return;
  }
  std::error_code ec;
  if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
    return;
  }
  for (const auto& entry : fs::directory_iterator(root, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file()) {
      continue;
    }
    const auto& path = entry.path();
    if (path.extension() != ".asm" && path.extension() != ".s" && path.extension() != ".inc") {
      continue;
    }
    if (IsMainFileName(path)) {
      main_candidates->insert(PathToUri(path.string()));
    }
  }
}

void IndexIncludeDependencies(const ParsedFile& parsed,
                              const fs::path& parent_path,
                              const std::vector<std::string>& include_paths) {
  if (parent_path.empty()) {
    return;
  }
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
    if (!ResolveIncludePath(event.path, base_dir, include_paths_current, &resolved)) {
      continue;
    }
    std::error_code ec;
    fs::path absolute = fs::absolute(resolved, ec);
    if (ec) {
      continue;
    }
    std::string parent_uri = PathToUri(parent_path.string());
    std::string child_uri = PathToUri(absolute.string());
    g_project_graph.RegisterDependency(parent_uri, child_uri);
  }
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
    if (line_end == std::string::npos) {
      line_end = text.size();
    }
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
        if (name == "off") {
          namespace_stack.clear();
        } else if (!name.empty()) {
          namespace_stack.push_back(name);
        }
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
        if (!name.empty()) {
          namespace_stack.push_back(name);
        }
        line_start = line_end + 1;
        ++line_number;
        continue;
      }

      if (HasPrefixIgnoreCase(trimmed, "popns")) {
        if (!namespace_stack.empty()) {
          namespace_stack.pop_back();
        }
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

            // Extract parameters
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
              if (!last.empty()) {
                entry.parameters.push_back(last);
              }
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
        while (i < trimmed.size() && IsSymbolChar(trimmed[i])) {
          ++i;
        }
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
          if (name[0] == '.') {
            // Sublabel - we don't handle parent tracking perfectly here but it's better than nothing
          } else {
            std::string ns = get_current_namespace();
            if (!ns.empty()) full_name = ns + "_" + name;
          }

          DocumentState::SymbolEntry entry;
          entry.name = full_name;
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
    std::string child_uri = PathToUri(key);
    g_project_graph.RegisterDependency(uri, child_uri);

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

bool IsOrgDirective(const std::string& trimmed) {
  if (trimmed.empty()) {
    return false;
  }
  std::string lower = ToLower(trimmed);
  if (HasPrefixIgnoreCase(lower, "org")) {
    if (lower.size() == 3) {
      return true;
    }
    char next = lower[3];
    return std::isspace(static_cast<unsigned char>(next)) != 0 || next == '(';
  }
  if (HasPrefixIgnoreCase(lower, "freespace")) {
    if (lower.size() == 9) {
      return true;
    }
    char next = lower[9];
    return std::isspace(static_cast<unsigned char>(next)) != 0 || next == '(';
  }
  return false;
}

bool ContainsOrgDirective(const std::string& text) {
  std::stringstream ss(text);
  std::string line;
  while (std::getline(ss, line)) {
    std::string trimmed = Trim(StripAsmComment(line));
    if (IsOrgDirective(trimmed)) {
      return true;
    }
  }
  return false;
}

bool IsPushPcDirective(const std::string& trimmed) {
  if (trimmed.empty()) {
    return false;
  }
  std::string lower = ToLower(trimmed);
  if (!HasPrefixIgnoreCase(lower, "pushpc")) {
    return false;
  }
  if (lower.size() == 6) {
    return true;
  }
  char next = lower[6];
  return std::isspace(static_cast<unsigned char>(next)) != 0 || next == '(';
}

bool IsPullPcDirective(const std::string& trimmed) {
  if (trimmed.empty()) {
    return false;
  }
  std::string lower = ToLower(trimmed);
  if (!HasPrefixIgnoreCase(lower, "pullpc")) {
    return false;
  }
  if (lower.size() == 6) {
    return true;
  }
  char next = lower[6];
  return std::isspace(static_cast<unsigned char>(next)) != 0 || next == '(';
}

bool ParentIncludesChildAfterOrg(const fs::path& parent_path,
                                 const fs::path& child_path,
                                 const std::vector<std::string>& include_paths) {
  if (parent_path.empty() || child_path.empty()) {
    return false;
  }
  std::ifstream file(parent_path);
  if (!file.is_open()) {
    return false;
  }

  fs::path base_dir = parent_path.parent_path();
  std::vector<std::string> include_paths_current = include_paths;
  bool org_mode = false;
  std::vector<bool> org_stack;

  std::string line;
  while (std::getline(file, line)) {
    std::string trimmed = Trim(StripAsmComment(line));
    if (trimmed.empty()) {
      continue;
    }

    std::string incdir_path;
    if (ParseIncdirDirective(trimmed, &incdir_path)) {
      auto resolved_incdir = ResolveIncdirPath(incdir_path, base_dir);
      if (resolved_incdir.has_value() &&
          std::find(include_paths_current.begin(),
                    include_paths_current.end(),
                    *resolved_incdir) == include_paths_current.end()) {
        include_paths_current.push_back(*resolved_incdir);
      }
      continue;
    }

    if (IsPushPcDirective(trimmed)) {
      org_stack.push_back(org_mode);
      continue;
    }

    if (IsPullPcDirective(trimmed)) {
      if (!org_stack.empty()) {
        org_mode = org_stack.back();
        org_stack.pop_back();
      }
      continue;
    }

    if (IsOrgDirective(trimmed)) {
      org_mode = true;
    }

    std::string include_path;
    if (ParseIncludeDirective(trimmed, &include_path)) {
      fs::path resolved;
      if (!ResolveIncludePath(include_path, base_dir, include_paths_current, &resolved)) {
        continue;
      }
      std::error_code ec;
      fs::path absolute = fs::absolute(resolved, ec);
      if (ec) {
        continue;
      }
      if (NormalizePath(absolute) == NormalizePath(child_path)) {
        return org_mode;
      }
    }
  }

  return false;
}

std::optional<WorkspaceState> BuildWorkspaceState(const json& params) {
  WorkspaceState state;
  if (params.contains("rootUri")) {
    state.root = UriToPath(params["rootUri"].get<std::string>());
  } else if (params.contains("rootPath")) {
    state.root = params["rootPath"].get<std::string>();
  }

  auto has_toml = [](const fs::path& root) {
    if (root.empty()) {
      return false;
    }
    std::error_code ec;
    return fs::exists(root / "z3dk.toml", ec);
  };

  if ((!state.root.empty() && !has_toml(state.root)) || state.root.empty()) {
    if (params.contains("workspaceFolders") && params["workspaceFolders"].is_array()) {
      for (const auto& folder : params["workspaceFolders"]) {
        if (!folder.is_object()) {
          continue;
        }
        fs::path candidate;
        if (folder.contains("uri")) {
          candidate = UriToPath(folder["uri"].get<std::string>());
        } else if (folder.contains("path")) {
          candidate = folder["path"].get<std::string>();
        }
        if (!candidate.empty() && has_toml(candidate)) {
          state.root = candidate;
          break;
        }
      }
    }
  }

  if (!state.root.empty()) {
    state.git_root = ResolveGitRoot(state.root);
    if (state.git_root.has_value()) {
      state.git_ignored_paths = LoadGitIgnoredPaths(*state.git_root);
    }
    fs::path config_path = state.root / "z3dk.toml";
    if (fs::exists(config_path)) {
      state.config_path = config_path;
      state.config = z3dk::LoadConfigIfExists(config_path.string());
    }

    std::vector<std::string> include_paths;
    fs::path config_dir = state.root;
    if (state.config_path.has_value()) {
      config_dir = state.config_path->parent_path();
    }
    if (state.config.has_value()) {
      UpdateLspLogConfig(*state.config, config_dir, state.root);
    }
    if (state.config.has_value() && !state.config->include_paths.empty()) {
      include_paths = ResolveIncludePaths(*state.config, config_dir);
    }

    bool has_config_mains = false;
    if (state.config.has_value()) {
      has_config_mains = AddMainCandidatesFromConfig(*state.config,
                                                    config_dir,
                                                    state.root,
                                                    &state.main_candidates);
    }
    if (!has_config_mains) {
      SeedMainCandidates(state.root, &state.main_candidates);
    }
    bool has_seeded_mains = !state.main_candidates.empty();

    // Crawl workspace for symbols
    for (const auto& entry : fs::recursive_directory_iterator(state.root)) {
      if (entry.is_regular_file()) {
        auto ext = entry.path().extension();
        if (ext == ".asm" || ext == ".s" || ext == ".inc") {
          if (IsGitIgnoredPath(state, entry.path())) {
            continue;
          }
          std::string path = entry.path().string();
          std::string uri = PathToUri(path);
          std::ifstream file(path);
          if (file.is_open()) {
            std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            ParsedFile parsed = ParseFileText(text, uri);
            for (const auto& sym : parsed.symbols) {
              state.symbol_names.insert(sym.name);
            }
            state.symbol_index[uri] = std::move(parsed.symbols);
            IndexIncludeDependencies(parsed, entry.path(), include_paths);
            if (!has_seeded_mains && IsMainFileName(entry.path())) {
              state.main_candidates.insert(uri);
            }
          }
        }
      }
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

json BuildWorkspaceSymbols(const WorkspaceState& workspace,
                           const std::string& query) {
  json result = json::array();
  for (const auto& pair : workspace.symbol_index) {
    const std::string& doc_uri = pair.first;
    for (const auto& symbol : pair.second) {
      if (!ContainsIgnoreCase(symbol.name, query)) {
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

DocumentState AnalyzeDocument(const DocumentState& doc,
                              const WorkspaceState& workspace,
                              const std::unordered_map<std::string, DocumentState>* open_documents) {
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

  UpdateLspLogConfig(config, config_dir, workspace.root);

  std::string root_uri = SelectRootUri(doc.uri, workspace);
  fs::path analysis_root_path = doc.path;
  if (!root_uri.empty()) {
    fs::path candidate = UriToPath(root_uri);
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
    doc_is_root = NormalizePath(analysis_root_path) == NormalizePath(doc_path);
  }

  std::vector<std::string> include_paths;
  if (!config.include_paths.empty()) {
    include_paths = ResolveIncludePaths(config, config_dir);
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
  std::vector<DocumentState::SymbolEntry> doc_symbols =
      ExtractSymbolsFromText(doc.text, fs::path(doc.path), include_paths_for_index, doc.uri);
  std::unordered_set<std::string> known_symbols = workspace.symbol_names;
  for (const auto& sym : doc_symbols) {
    known_symbols.insert(sym.name);
  }
  if (IsGitIgnoredPath(workspace, doc_path)) {
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
    fs::path resolved = ResolveConfigPath(*config.rom_path, config_dir, workspace.root);
    std::vector<uint8_t> rom_data;
    if (LoadRomData(resolved, &rom_data)) {
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
            if (HasPrefixIgnoreCase(Trim(comment), "assume ")) {
                 std::string rest = std::string(Trim(comment)).substr(7); // "assume " length
                 rest = Trim(rest);
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
                                  PathMatchesDocumentPath(result.source_map.files[entry.file_id].path,
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
      Log("LSP JSON error: " + std::string(e.what()));
    } catch (...) {
      Log("LSP JSON error: unknown exception");
    }
  }

  z3dk::LintResult lint_result = z3dk::RunLint(result, lint_options);
  
  auto filter_diags = [&](const std::vector<z3dk::Diagnostic>& input) {
    std::vector<z3dk::Diagnostic> out;
    out.reserve(input.size());
    for (const auto& diag : input) {
      if (DiagnosticMatchesDocument(diag, doc_path, analysis_root_dir, workspace.root, doc_is_root)) {
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
      std::string missing = ExtractMissingLabel(diag.message);
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
  if (!doc_is_root && !ContainsOrgDirective(doc.text)) {
    bool suppress_missing_org = false;
    auto parents = g_project_graph.GetParents(doc.uri);
    if (!parents.empty()) {
      for (const auto& parent_uri : parents) {
        fs::path parent_path = UriToPath(parent_uri);
        if (!parent_path.empty() && fs::exists(parent_path)) {
          if (ParentIncludesChildAfterOrg(parent_path, doc_path, include_paths_for_parent_check)) {
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

std::optional<json> HandleRename(const DocumentState& doc, const WorkspaceState& workspace, 
                                 const std::unordered_map<std::string, DocumentState>& documents, 
                                 const json& params) {
  if (!params.contains("textDocument") || !params.contains("position") || !params.contains("newName")) {
    return std::nullopt;
  }
  
  std::string new_name = params["newName"];
  if (new_name.empty()) return std::nullopt;
  
  auto position = params["position"];
  int line = position.value("line", 0);
  int character = position.value("character", 0);
  auto token_opt = ExtractTokenAt(doc.text, line, character);
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
                 if (IsGitIgnoredPath(workspace, entry.path())) {
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
      std::string file_uri = PathToUri(path.string());
      
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
                   bool start_ok = (i == 0) || !IsSymbolChar(text[i - 1]);
                   bool end_ok = (i + token.size() == text.size()) || !IsSymbolChar(text[i + token.size()]);
                   
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
  auto token = ExtractTokenAt(doc.text, line, character);
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

  std::string trimmed = Trim(StripAsmComment(line_text));
  std::string include_path;
  if (ParseIncludeDirective(trimmed, &include_path) || ParseIncdirDirective(trimmed, &include_path)) {
    // If the cursor is within the quotes of the include path
    size_t quote_start = line_text.find('"');
    size_t quote_end = line_text.find('"', quote_start + 1);
    if (quote_start != std::string::npos && quote_end != std::string::npos &&
        character >= static_cast<int>(quote_start) && character <= static_cast<int>(quote_end)) {
      
      fs::path base_dir = fs::path(doc.path).parent_path();
      fs::path target_path;
      std::vector<std::string> include_paths;
      include_paths.push_back(base_dir.string());
      
      if (ResolveIncludePath(include_path, base_dir, include_paths, &target_path)) {
        json location;
        location["uri"] = PathToUri(target_path.string());
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
    location["uri"] = PathToUri(file_it->second);
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
  auto token = ExtractTokenAt(doc.text, line, character);
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
      auto val = g_mesen.ReadByte(addr);
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
      Log("LSP JSON error: " + std::string(e.what()));
    } catch (...) {
      Log("LSP JSON error: unknown exception");
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
      keyword_set.insert(ToLower(directive));
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
      if (c == '+' || c == '-' || c == '*' || c == '/' || c == ',' || c == '#' || c == '(' || c == ')') {
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

  // Debounce settings: delay full analysis until typing pauses
  constexpr auto kDebounceDelay = std::chrono::milliseconds(500);
  auto last_change_time = std::chrono::steady_clock::now();

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
      SendMessage(response);
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
          {"result", nullptr},
      };
      auto it = documents.find(uri);
      if (it != documents.end()) {
        response["result"] = BuildSemanticTokens(it->second);
      }
      SendMessage(response);
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
            g_mesen.SendCommand(mesen_cmd);
            response["result"] = "Synced " + std::to_string(pair.second.labels.size()) + " symbols";
            break;
          }
        }
      } else if (command == "mesen.toggleBreakpoint") {
        if (!args.empty() && args[0].is_number()) {
          uint32_t addr = args[0].get<uint32_t>();
          json mesen_cmd = {{"type", "BREAKPOINT"}, {"action", "toggle"}, {"addr", addr}};
          g_mesen.SendCommand(mesen_cmd);
          std::string addr_str;
          std::ostringstream ss;
          ss << std::hex << std::uppercase << std::setfill('0') << std::setw(6) << addr;
          response["result"] = "Toggled breakpoint at $" + ss.str();
        }
      } else if (command == "mesen.stepInstruction") {
        json mesen_cmd = {{"type", "STEP_INTO"}};
        if (g_mesen.SendCommand(mesen_cmd)) {
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
        auto result = g_mesen.SendCommand(mesen_cmd);
        if (result) {
           response["result"] = result->dump(2);
        } else {
           response["result"] = "Failed to retrieve CPU state";
        }
      }

      SendMessage(response);
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

              const DocumentState::SymbolEntry* found_symbol = nullptr;
              
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
      SendMessage(response);
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
                 
                 // Minimum 2 digits for a byte, 4 for word, 6 for long
                 if (len >= 2 && line >= start_line) {
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

                     const DocumentState::SymbolEntry* macro = nullptr;
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
      SendMessage(response);
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
          auto extracted = ExtractTokenAt(doc.text, line, character);
          if (extracted) token = *extracted;
      }
      
      if (!token.empty()) {
          std::vector<fs::path> files_to_scan;
          if (fs::exists(workspace.root) && fs::is_directory(workspace.root)) {
             for (const auto& entry : fs::recursive_directory_iterator(workspace.root)) {
                 if (entry.is_regular_file()) {
                     auto ext = entry.path().extension();
                     if (ext == ".asm" || ext == ".s" || ext == ".inc" || ext == ".a") {
                         if (IsGitIgnoredPath(workspace, entry.path())) {
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
                       bool start_ok = (i == 0) || !IsSymbolChar(text[i - 1]);
                       bool end_ok = (i + token.size() == text.size()) || !IsSymbolChar(text[i + token.size()]);
                       
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
      std::vector<std::string> include_paths;
      include_paths.push_back(fs::path(it->second.path).parent_path().string());
      it->second.symbols = ExtractSymbolsFromText(it->second.text,
                                                   fs::path(it->second.path),
                                                   include_paths,
                                                   it->second.uri);
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
          {"result", BuildWorkspaceSymbols(workspace, query)},
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
          response["result"] = BuildCompletionItems(it->second, workspace, *prefix);
        }
      }
      SendMessage(response);
      continue;
    }
  }

  return 0;
}
