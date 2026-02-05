#include "z3dk_core/emit.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace z3dk {
namespace {

struct HookDirective {
  bool present = false;
  std::string name;
  std::string kind;
  std::string target;
  std::string note;
  std::string module;
  std::string abi_class;
  std::optional<int> expected_m;
  std::optional<int> expected_x;
  std::optional<int> expected_exit_m;
  std::optional<int> expected_exit_x;
  std::optional<bool> skip_abi;
};

std::string EscapeJson(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  return out;
}

std::string Trim(std::string_view text) {
  size_t start = 0;
  while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
    ++start;
  }
  size_t end = text.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return std::string(text.substr(start, end - start));
}

std::optional<int> ParseInt(std::string_view text) {
  std::string token = Trim(text);
  if (token.empty()) {
    return std::nullopt;
  }
  if (token.front() == '"' || token.front() == '\'') {
    if (token.size() >= 2) {
      token = token.substr(1, token.size() - 2);
    }
  }
  if (!token.empty() && token.front() == '$') {
    token = "0x" + token.substr(1);
  }
  try {
    return std::stoi(token, nullptr, 0);
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<bool> ParseBool(std::string_view text) {
  std::string token = Trim(text);
  if (token.empty()) {
    return std::nullopt;
  }
  for (auto& c : token) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (token == "1" || token == "true" || token == "yes" || token == "y") {
    return true;
  }
  if (token == "0" || token == "false" || token == "no" || token == "n") {
    return false;
  }
  return std::nullopt;
}

HookDirective ParseHookDirective(std::string_view line) {
  HookDirective directive;
  auto semicolon = line.find(';');
  if (semicolon == std::string_view::npos) {
    return directive;
  }
  std::string_view comment = line.substr(semicolon + 1);
  auto hook_pos = comment.find("@hook");
  if (hook_pos == std::string_view::npos) {
    return directive;
  }
  directive.present = true;
  std::string_view tail = comment.substr(hook_pos + 5);

  size_t i = 0;
  while (i < tail.size()) {
    while (i < tail.size() &&
           std::isspace(static_cast<unsigned char>(tail[i]))) {
      ++i;
    }
    if (i >= tail.size()) {
      break;
    }
    size_t key_start = i;
    while (i < tail.size() && tail[i] != '=' &&
           !std::isspace(static_cast<unsigned char>(tail[i]))) {
      ++i;
    }
    if (i >= tail.size() || tail[i] != '=') {
      while (i < tail.size() &&
             !std::isspace(static_cast<unsigned char>(tail[i]))) {
        ++i;
      }
      continue;
    }
    std::string key = std::string(tail.substr(key_start, i - key_start));
    ++i;
    while (i < tail.size() &&
           std::isspace(static_cast<unsigned char>(tail[i]))) {
      ++i;
    }
    if (i >= tail.size()) {
      break;
    }
    char quote = 0;
    if (tail[i] == '"' || tail[i] == '\'') {
      quote = tail[i];
      ++i;
    }
    size_t value_start = i;
    if (quote) {
      while (i < tail.size() && tail[i] != quote) {
        ++i;
      }
    } else {
      while (i < tail.size() &&
             !std::isspace(static_cast<unsigned char>(tail[i]))) {
        ++i;
      }
    }
    std::string value = std::string(tail.substr(value_start, i - value_start));
    if (quote && i < tail.size()) {
      ++i;
    }

    for (auto& c : key) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (key == "name") {
      directive.name = value;
    } else if (key == "kind") {
      directive.kind = value;
    } else if (key == "target") {
      directive.target = value;
    } else if (key == "note") {
      directive.note = value;
    } else if (key == "module") {
      directive.module = value;
    } else if (key == "abi" || key == "abi_class") {
      directive.abi_class = value;
    } else if (key == "expected_m") {
      directive.expected_m = ParseInt(value);
    } else if (key == "expected_x") {
      directive.expected_x = ParseInt(value);
    } else if (key == "expected_exit_m") {
      directive.expected_exit_m = ParseInt(value);
    } else if (key == "expected_exit_x") {
      directive.expected_exit_x = ParseInt(value);
    } else if (key == "skip_abi") {
      directive.skip_abi = ParseBool(value);
    }
  }
  return directive;
}

std::optional<uint32_t> ParseOrgAddress(std::string_view line) {
  std::string text = Trim(line);
  if (text.rfind("org", 0) != 0) {
    return std::nullopt;
  }
  size_t pos = text.find('$');
  if (pos == std::string::npos) {
    return std::nullopt;
  }
  size_t end = pos + 1;
  while (end < text.size() && std::isxdigit(static_cast<unsigned char>(text[end]))) {
    ++end;
  }
  if (end == pos + 1) {
    return std::nullopt;
  }
  auto value = ParseInt(text.substr(pos, end - pos));
  if (!value.has_value()) {
    return std::nullopt;
  }
  return static_cast<uint32_t>(*value);
}

HookDirective FindHookDirectiveForAddress(const std::vector<std::string>& lines,
                                          int line_index,
                                          uint32_t address) {
  if (line_index < 0 || static_cast<size_t>(line_index) >= lines.size()) {
    return {};
  }
  HookDirective directive = ParseHookDirective(lines[line_index]);
  if (directive.present) {
    return directive;
  }

  const int max_scan = 40;
  int start = line_index;
  int end = std::max(0, line_index - max_scan);
  for (int i = start; i >= end; --i) {
    const std::string& line = lines[i];
    auto org_addr = ParseOrgAddress(line);
    if (org_addr.has_value()) {
      uint32_t org_value = *org_addr;
      bool matches = (org_value == address) || ((org_value ^ 0x800000) == address);
      if (matches) {
        return ParseHookDirective(line);
      }
      break;
    }
  }
  return {};
}

void AppendDiagnosticJson(const Diagnostic& diag, std::ostringstream* out) {
  *out << "{\"message\":\"" << EscapeJson(diag.message) << "\"";
  if (!diag.filename.empty()) {
    *out << ",\"file\":\"" << EscapeJson(diag.filename) << "\"";
  }
  if (diag.line > 0) {
    *out << ",\"line\":" << diag.line;
  }
  if (diag.column > 0) {
    *out << ",\"column\":" << diag.column;
  }
  if (!diag.raw.empty()) {
    *out << ",\"raw\":\"" << EscapeJson(diag.raw) << "\"";
  }
  *out << "}";
}

}  // namespace

std::string DiagnosticsToJson(const AssembleResult& result) {
  std::ostringstream out;
  out << "{\"version\":1,\"success\":" << (result.success ? "true" : "false");

  out << ",\"errors\":[";
  bool first = true;
  for (const auto& diag : result.diagnostics) {
    if (diag.severity != DiagnosticSeverity::kError) {
      continue;
    }
    if (!first) {
      out << ',';
    }
    AppendDiagnosticJson(diag, &out);
    first = false;
  }
  out << "]";

  out << ",\"warnings\":[";
  first = true;
  for (const auto& diag : result.diagnostics) {
    if (diag.severity != DiagnosticSeverity::kWarning) {
      continue;
    }
    if (!first) {
      out << ',';
    }
    AppendDiagnosticJson(diag, &out);
    first = false;
  }
  out << "]";

  out << "}";
  return out.str();
}

std::string DiagnosticsListToJson(const std::vector<Diagnostic>& diagnostics,
                                  bool success) {
  std::ostringstream out;
  out << "{\"version\":1,\"success\":" << (success ? "true" : "false");

  out << ",\"errors\":[";
  bool first = true;
  for (const auto& diag : diagnostics) {
    if (diag.severity != DiagnosticSeverity::kError) {
      continue;
    }
    if (!first) {
      out << ',';
    }
    AppendDiagnosticJson(diag, &out);
    first = false;
  }
  out << "]";

  out << ",\"warnings\":[";
  first = true;
  for (const auto& diag : diagnostics) {
    if (diag.severity != DiagnosticSeverity::kWarning) {
      continue;
    }
    if (!first) {
      out << ',';
    }
    AppendDiagnosticJson(diag, &out);
    first = false;
  }
  out << "]";

  out << "}";
  return out.str();
}

std::string AnnotationsToJson(const AssembleResult& result) {
  std::unordered_map<std::string, uint32_t> label_index;
  label_index.reserve(result.labels.size());
  for (const auto& label : result.labels) {
    if (label.name.empty()) {
      continue;
    }
    if (label_index.find(label.name) == label_index.end()) {
      label_index[label.name] = label.address;
    }
  }

  std::unordered_map<int, std::vector<std::string>> file_lines;
  for (const auto& file : result.source_map.files) {
    std::ifstream in(file.path);
    if (!in.is_open()) {
      continue;
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
      lines.push_back(line);
    }
    file_lines[file.id] = std::move(lines);
  }

  const std::regex define_re(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(\$[0-9A-Fa-f]{4,6}))");
  const std::regex label_re(R"(^\s*([A-Za-z_][A-Za-z0-9_.]*)\s*:)");
  const std::regex fmt_re(R"(fmt=([a-zA-Z]+))");

  std::ostringstream out;
  out << "{\"version\":1,\"annotations\":[";
  bool first = true;

  for (const auto& file : result.source_map.files) {
    auto it = file_lines.find(file.id);
    if (it == file_lines.end()) {
      continue;
    }
    const auto& lines = it->second;
    for (size_t i = 0; i < lines.size(); ++i) {
      const std::string& line = lines[i];
      auto semicolon = line.find(';');
      if (semicolon == std::string::npos) {
        continue;
      }
      std::string comment = Trim(line.substr(semicolon + 1));
      if (comment.empty()) {
        continue;
      }

      bool has_watch = comment.find("@watch") != std::string::npos;
      bool has_assert = comment.find("@assert") != std::string::npos;
      bool has_abi = comment.find("@abi") != std::string::npos ||
                     comment.find("@no_return") != std::string::npos;

      auto emit_entry = [&](std::string_view type,
                            const std::string& label,
                            std::optional<uint32_t> addr,
                            const std::string& format,
                            const std::string& note,
                            const std::string& expr) {
        if (!first) {
          out << ',';
        }
        first = false;
        out << "{\"type\":\"" << EscapeJson(type) << "\"";
        if (!label.empty()) {
          out << ",\"label\":\"" << EscapeJson(label) << "\"";
        }
        if (addr.has_value()) {
          out << ",\"address\":\"0x";
          out << std::hex << std::uppercase;
          out.width(6);
          out.fill('0');
          out << *addr;
          out << std::dec << "\"";
        } else if (type == "watch") {
          out << ",\"address\":null";
        }
        if (!format.empty()) {
          out << ",\"format\":\"" << EscapeJson(format) << "\"";
        }
        out << ",\"source\":\"" << EscapeJson(file.path) << ":" << (i + 1) << "\"";
        if (!note.empty()) {
          out << ",\"note\":\"" << EscapeJson(note) << "\"";
        }
        if (!expr.empty()) {
          out << ",\"expr\":\"" << EscapeJson(expr) << "\"";
        }
        out << "}";
      };

      if (has_watch) {
        std::string label;
        std::optional<uint32_t> addr;
        std::smatch match;
        if (std::regex_search(line, match, define_re) && match.size() >= 3) {
          label = match[1].str();
          auto parsed = ParseInt(match[2].str());
          if (parsed.has_value()) {
            addr = static_cast<uint32_t>(*parsed);
          }
        } else if (std::regex_search(line, match, label_re) && match.size() >= 2) {
          label = match[1].str();
          auto label_it = label_index.find(label);
          if (label_it != label_index.end()) {
            addr = label_it->second;
          }
        }

        std::string format;
        if (std::regex_search(comment, match, fmt_re) && match.size() >= 2) {
          format = match[1].str();
        }
        emit_entry("watch", label, addr, format, comment, "");
      }

      if (has_assert) {
        auto pos = comment.find("@assert");
        std::string expr;
        if (pos != std::string::npos) {
          expr = Trim(comment.substr(pos + 7));
        }
        emit_entry("assert", "", std::nullopt, "", "", expr);
      }

      if (has_abi) {
        emit_entry("abi", "", std::nullopt, "", comment, "");
      }
    }
  }

  out << "]}";
  return out.str();
}

std::string HooksToJson(const AssembleResult& result,
                        const std::string& rom_path) {
  std::unordered_map<uint32_t, std::string> label_index;
  label_index.reserve(result.labels.size());
  for (const auto& label : result.labels) {
    if (label.name.empty()) {
      continue;
    }
    if (label_index.find(label.address) == label_index.end()) {
      label_index[label.address] = label.name;
    }
  }

  std::unordered_map<int, std::string> file_index;
  std::unordered_map<int, std::vector<std::string>> file_lines;
  for (const auto& file : result.source_map.files) {
    file_index[file.id] = file.path;
    std::ifstream in(file.path);
    if (!in.is_open()) {
      continue;
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
      lines.push_back(line);
    }
    file_lines[file.id] = std::move(lines);
  }

  std::vector<SourceMapEntry> entries = result.source_map.entries;
  std::sort(entries.begin(), entries.end(),
            [](const SourceMapEntry& a, const SourceMapEntry& b) {
              if (a.address != b.address) {
                return a.address < b.address;
              }
              return a.line < b.line;
            });

  struct SourceLoc {
    std::string path;
    int file_id = -1;
    int line = 0;
  };

  auto find_source_loc = [&](uint32_t address) -> SourceLoc {
    if (entries.empty()) {
      return {};
    }
    auto it = std::upper_bound(entries.begin(), entries.end(), address,
                               [](uint32_t addr, const SourceMapEntry& entry) {
                                 return addr < entry.address;
                               });
    if (it == entries.begin()) {
      return {};
    }
    --it;
    auto file_it = file_index.find(it->file_id);
    if (file_it == file_index.end()) {
      return {};
    }
    SourceLoc loc;
    loc.path = file_it->second;
    loc.file_id = it->file_id;
    loc.line = it->line;
    return loc;
  };

  std::ostringstream out;
  out << "{\"version\":1";
  if (!rom_path.empty()) {
    out << ",\"rom\":{\"path\":\"" << EscapeJson(rom_path) << "\"}";
  }
  out << ",\"hooks\":[";

  bool first = true;
  for (const auto& block : result.written_blocks) {
    if (block.num_bytes <= 0) {
      continue;
    }
    uint32_t address = static_cast<uint32_t>(block.snes_offset);
    uint32_t size = static_cast<uint32_t>(block.num_bytes);
    std::string name;
    auto label_it = label_index.find(address);
    if (label_it != label_index.end()) {
      name = label_it->second;
    }
    SourceLoc source_loc = find_source_loc(address);
    std::string source;
    if (!source_loc.path.empty()) {
      std::ostringstream source_out;
      source_out << source_loc.path;
      if (source_loc.line > 0) {
        source_out << ":" << source_loc.line;
      }
      source = source_out.str();
    }

    HookDirective directive;
    if (source_loc.file_id >= 0 && source_loc.line > 0) {
      auto lines_it = file_lines.find(source_loc.file_id);
      if (lines_it != file_lines.end()) {
        const auto& lines = lines_it->second;
        if (static_cast<size_t>(source_loc.line - 1) < lines.size()) {
          directive = FindHookDirectiveForAddress(
              lines, source_loc.line - 1, address);
        }
      }
    }

    if (!first) {
      out << ',';
    }
    first = false;
    out << "{\"address\":\"0x";
    out << std::hex << std::uppercase;
    out.width(6);
    out.fill('0');
    out << address;
    out << std::dec << "\"";
    out << ",\"size\":" << size;
    std::string hook_name = name;
    std::string kind = "patch";
    std::string target;
    if (directive.present) {
      if (!directive.name.empty()) {
        hook_name = directive.name;
      }
      if (!directive.kind.empty()) {
        kind = directive.kind;
      }
      if (!directive.target.empty()) {
        target = directive.target;
      }
    }
    if (hook_name.empty() && !target.empty()) {
      hook_name = target;
    }

    out << ",\"kind\":\"" << EscapeJson(kind) << "\"";
    if (!hook_name.empty()) {
      out << ",\"name\":\"" << EscapeJson(hook_name) << "\"";
    }
    if (!target.empty()) {
      out << ",\"target\":\"" << EscapeJson(target) << "\"";
    }
    if (directive.expected_m.has_value()) {
      out << ",\"expected_m\":" << *directive.expected_m;
    }
    if (directive.expected_x.has_value()) {
      out << ",\"expected_x\":" << *directive.expected_x;
    }
    if (directive.expected_exit_m.has_value()) {
      out << ",\"expected_exit_m\":" << *directive.expected_exit_m;
    }
    if (directive.expected_exit_x.has_value()) {
      out << ",\"expected_exit_x\":" << *directive.expected_exit_x;
    }
    if (directive.skip_abi.has_value()) {
      out << ",\"skip_abi\":" << (*directive.skip_abi ? "true" : "false");
    }
    if (!directive.abi_class.empty()) {
      out << ",\"abi_class\":\"" << EscapeJson(directive.abi_class) << "\"";
    }
    if (!directive.module.empty()) {
      out << ",\"module\":\"" << EscapeJson(directive.module) << "\"";
    }
    if (!directive.note.empty()) {
      out << ",\"note\":\"" << EscapeJson(directive.note) << "\"";
    }
    if (!source.empty()) {
      out << ",\"source\":\"" << EscapeJson(source) << "\"";
    }
    out << "}";
  }
  out << "]}";
  return out.str();
}

std::string SourceMapToJson(const SourceMap& map) {
  std::ostringstream out;
  out << "{\"version\":1";

  out << ",\"files\":[";
  bool first = true;
  for (const auto& file : map.files) {
    if (!first) {
      out << ',';
    }
    out << "{\"id\":" << file.id
        << ",\"crc\":\"0x";
    out << std::hex << std::uppercase << file.crc << std::dec;
    out << "\",\"path\":\"" << EscapeJson(file.path) << "\"}";
    first = false;
  }
  out << "]";

  out << ",\"entries\":[";
  first = true;
  for (const auto& entry : map.entries) {
    if (!first) {
      out << ',';
    }
    out << "{\"address\":\"0x";
    out << std::hex << std::uppercase << entry.address << std::dec;
    out << "\",\"file_id\":" << entry.file_id
        << ",\"line\":" << entry.line << "}";
    first = false;
  }
  out << "]";

  out << "}";
  return out.str();
}

std::string SymbolsToMlb(const std::vector<Label>& labels) {
  std::vector<Label> sorted = labels;
  std::sort(sorted.begin(), sorted.end(), [](const Label& a, const Label& b) {
    if (a.address != b.address) {
      return a.address < b.address;
    }
    return a.name < b.name;
  });

  std::ostringstream out;
  for (const auto& label : sorted) {
    out << "PRG:" << std::hex << std::uppercase << label.address << std::dec
        << ":" << label.name << "\n";
  }
  return out.str();
}

bool WriteTextFile(const std::string& path, std::string_view contents,
                   std::string* error) {
  std::ofstream file(path, std::ios::binary);
  if (!file.is_open()) {
    if (error) {
      *error = "Unable to write file: " + path;
    }
    return false;
  }
  file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (!file.good()) {
    if (error) {
      *error = "Failed to write file: " + path;
    }
    return false;
  }
  return true;
}

}  // namespace z3dk
