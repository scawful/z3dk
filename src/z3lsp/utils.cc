#include "utils.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

namespace z3lsp {

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

std::string ToHexString(uint32_t value, int width) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%0*X", width, value);
  return std::string(buf);
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

std::string ToLower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

fs::path NormalizePath(const fs::path& path) {
  return path.lexically_normal();
}

fs::path ResolveConfigPath(const std::string& raw,
                           const fs::path& config_dir,
                           const fs::path& workspace_root) {
  if (raw.empty()) return {};
  fs::path p(raw);
  if (p.is_absolute()) return NormalizePath(p);
  if (!config_dir.empty()) {
    fs::path c = NormalizePath(config_dir / p);
    if (fs::exists(c)) return c;
  }
  if (!workspace_root.empty()) {
    return NormalizePath(workspace_root / p);
  }
  return NormalizePath(p);
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

std::optional<std::filesystem::path> ResolveGitRoot(const std::filesystem::path& start_path) {
  if (start_path.empty()) return std::nullopt;
  std::error_code ec;
  fs::path current = fs::absolute(start_path, ec);
  if (ec) return std::nullopt;
  while (!current.empty()) {
    if (fs::exists(current / ".git", ec)) {
      return current;
    }
    fs::path parent = current.parent_path();
    if (parent == current) break;
    current = parent;
  }
  return std::nullopt;
}

std::unordered_set<std::string> LoadGitIgnoredPaths(const std::filesystem::path& git_root) {
  std::unordered_set<std::string> ignored;
  if (git_root.empty()) return ignored;
  std::string output = RunCommandCapture("git -C " + QuoteShellArg(git_root.string()) + " ls-files --others --ignored --exclude-standard --directory");
  std::stringstream ss(output);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.empty()) continue;
    fs::path full = git_root / line;
    ignored.insert(NormalizePath(full).string());
  }
  return ignored;
}

std::vector<ReferenceLocation> FindReferencesInText(const std::string& text, const std::string& token) {
  std::vector<ReferenceLocation> refs;
  if (token.empty() || text.empty()) return refs;

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

      // Skip comments
      if (text[i] == ';') {
          while (i < text.size() && text[i] != '\n') {
              i++;
          }
          continue;
      }
      
      // Skip strings
      if (text[i] == '"') {
          size_t start = i;
          bool escaped = false;
          i++; 
          while (i < text.size()) {
               if (escaped) { escaped = false; }
               else if (text[i] == '\\') { escaped = true; }
               else if (text[i] == '"') { break; }
               else if (text[i] == '\n') { break; } 
               i++;
          }
          int len = static_cast<int>(i - start) + 1; // +1 includes closing quote or newline/eof
          current_col += len;
          i++; 
          continue;
      }

      // Check match
      if (text[i] == token[0]) {
           if (i + token.size() <= text.size() && text.compare(i, token.size(), token) == 0) {
                // Determine boundaries
                bool start_ok = (i == 0) || !IsSymbolChar(text[i-1]);
                bool end_ok = (i + token.size() == text.size()) || !IsSymbolChar(text[i + token.size()]);
                
                if (start_ok && end_ok) {
                    refs.push_back({current_line, current_col, (int)token.size()});
                    i += token.size();
                    current_col += (int)token.size();
                    continue;
                }
           }
      }
      
      i++;
      current_col++;
  }
  return refs;
}

}  // namespace z3lsp
