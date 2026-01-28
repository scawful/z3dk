#include "z3dk_core/config.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string_view>

namespace z3dk {
namespace {

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

std::string StripComments(const std::string& line) {
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
    if (!in_string && c == '#') {
      return line.substr(0, i);
    }
  }
  return line;
}

std::string UnescapeString(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool escape = false;
  for (char c : text) {
    if (escape) {
      switch (c) {
        case 'n':
          out.push_back('\n');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case '\\':
        case '"':
          out.push_back(c);
          break;
        default:
          out.push_back(c);
          break;
      }
      escape = false;
    } else if (c == '\\') {
      escape = true;
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string ParseStringValue(std::string_view value) {
  std::string trimmed = Trim(value);
  if (trimmed.size() >= 2 && trimmed.front() == '"' && trimmed.back() == '"') {
    return UnescapeString(std::string_view(trimmed).substr(1, trimmed.size() - 2));
  }
  return trimmed;
}

std::vector<std::string> ParseStringArray(std::string_view value) {
  std::vector<std::string> out;
  std::string trimmed = Trim(value);
  if (trimmed.size() < 2 || trimmed.front() != '[' || trimmed.back() != ']') {
    if (!Trim(trimmed).empty()) {
      out.push_back(ParseStringValue(trimmed));
    }
    return out;
  }
  std::string inner = trimmed.substr(1, trimmed.size() - 2);
  std::string current;
  bool in_string = false;
  bool escape = false;
  for (char c : inner) {
    if (escape) {
      current.push_back(c);
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
    if (!in_string && c == ',') {
      std::string token = Trim(current);
      if (!token.empty()) {
        out.push_back(ParseStringValue(token));
      }
      current.clear();
      continue;
    }
    current.push_back(c);
  }
  std::string token = Trim(current);
  if (!token.empty()) {
    out.push_back(ParseStringValue(token));
  }
  return out;
}

std::optional<int> ParseInt(std::string_view value) {
  std::string trimmed = Trim(value);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  char* end = nullptr;
  long parsed = std::strtol(trimmed.c_str(), &end, 0);
  if (end == trimmed.c_str() || *end != '\0') {
    return std::nullopt;
  }
  return static_cast<int>(parsed);
}

std::optional<bool> ParseBool(std::string_view value) {
  std::string trimmed = Trim(value);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  std::string lower;
  lower.reserve(trimmed.size());
  for (char c : trimmed) {
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (lower == "true" || lower == "1" || lower == "yes" || lower == "on") {
    return true;
  }
  if (lower == "false" || lower == "0" || lower == "no" || lower == "off") {
    return false;
  }
  return std::nullopt;
}

bool ContainsArrayStart(std::string_view value) {
  bool in_string = false;
  bool escape = false;
  for (char c : value) {
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
    if (!in_string && c == '[') {
      return true;
    }
  }
  return false;
}

int ArrayBracketDelta(std::string_view value) {
  bool in_string = false;
  bool escape = false;
  int delta = 0;
  for (char c : value) {
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
    if (!in_string) {
      if (c == '[') {
        ++delta;
      } else if (c == ']') {
        --delta;
      }
    }
  }
  return delta;
}

bool ParseAddress(std::string_view value, uint32_t* out) {
  std::string trimmed = Trim(value);
  if (trimmed.empty()) {
    return false;
  }
  if (trimmed.front() == '$') {
    trimmed.erase(0, 1);
  }
  char* end_ptr = nullptr;
  unsigned long parsed = std::strtoul(trimmed.c_str(), &end_ptr, 0);
  if (end_ptr == trimmed.c_str() || *end_ptr != '\0') {
    return false;
  }
  *out = static_cast<uint32_t>(parsed);
  return true;
}

MemoryRange ParseMemoryRange(std::string_view value) {
  std::string s = ParseStringValue(value);
  MemoryRange range{0, 0, ""};
  auto dash = s.find('-');
  if (dash == std::string::npos) return range;
  auto colon = s.find(':', dash);
  
  std::string start_str = s.substr(0, dash);
  std::string end_str = s.substr(dash + 1, colon != std::string::npos ? colon - dash - 1 : std::string::npos);

  uint32_t start = 0;
  uint32_t end = 0;
  if (!ParseAddress(start_str, &start) || !ParseAddress(end_str, &end) || end < start) {
    return range;
  }
  range.start = start;
  range.end = (end == UINT32_MAX) ? end : end + 1;
  if (colon != std::string::npos) {
    range.reason = Trim(s.substr(colon + 1));
  }
  return range;
}

}  // namespace

bool IsArrayKey(const std::string& key) {
  return key == "include_paths" || key == "defines" || key == "emit" ||
         key == "emits" || key == "main" || key == "main_file" ||
         key == "main_files" || key == "entry" || key == "entry_files" ||
         key == "prohibited_memory_ranges";
}

void ApplyArrayKey(Config* config, const std::string& key, std::string_view value) {
  if (!config) {
    return;
  }
  std::vector<std::string> items = ParseStringArray(value);
  if (key == "include_paths") {
    config->include_paths = std::move(items);
  } else if (key == "defines") {
    config->defines = std::move(items);
  } else if (key == "emit" || key == "emits") {
    config->emits = std::move(items);
  } else if (key == "prohibited_memory_ranges") {
    for (const auto& item : items) {
      MemoryRange range = ParseMemoryRange(item);
      if (range.end > range.start) {
        config->prohibited_memory_ranges.push_back(std::move(range));
      }
    }
  } else {
    config->main_files = std::move(items);
  }
}

Config LoadConfigFile(const std::string& path, std::string* error) {
  Config config;
  std::ifstream file(path);
  if (!file.is_open()) {
    if (error) {
      *error = "Unable to open config: " + path;
    }
    return config;
  }

  std::string line;
  int line_number = 0;
  std::string pending_key;
  std::string pending_value;
  int pending_brackets = 0;
  while (std::getline(file, line)) {
    ++line_number;
    std::string stripped = StripComments(line);
    std::string trimmed = Trim(stripped);
    if (trimmed.empty()) {
      continue;
    }

    if (!pending_key.empty()) {
      if (!pending_value.empty()) {
        pending_value += " ";
      }
      pending_value += trimmed;
      pending_brackets += ArrayBracketDelta(trimmed);
      if (pending_brackets <= 0) {
        ApplyArrayKey(&config, pending_key, pending_value);
        pending_key.clear();
        pending_value.clear();
        pending_brackets = 0;
      }
      continue;
    }

    auto equals = trimmed.find('=');
    if (equals == std::string::npos) {
      continue;
    }

    std::string key = Trim(trimmed.substr(0, equals));
    std::string value = Trim(trimmed.substr(equals + 1));
    if (IsArrayKey(key) && ContainsArrayStart(value)) {
      int delta = ArrayBracketDelta(value);
      if (delta > 0) {
        pending_key = key;
        pending_value = value;
        pending_brackets = delta;
        continue;
      }
      ApplyArrayKey(&config, key, value);
      continue;
    }

    if (key == "preset") {
      config.preset = ParseStringValue(value);
    } else if (key == "include_paths") {
      ApplyArrayKey(&config, key, value);
    } else if (key == "defines") {
      ApplyArrayKey(&config, key, value);
    } else if (key == "emit" || key == "emits") {
      ApplyArrayKey(&config, key, value);
    } else if (key == "main" || key == "main_file" || key == "main_files" ||
               key == "entry" || key == "entry_files") {
      ApplyArrayKey(&config, key, value);
    } else if (key == "std_includes") {
      config.std_includes_path = ParseStringValue(value);
    } else if (key == "std_defines") {
      config.std_defines_path = ParseStringValue(value);
    } else if (key == "mapper") {
      config.mapper = ParseStringValue(value);
    } else if (key == "rom" || key == "rom_path") {
      config.rom_path = ParseStringValue(value);
    } else if (key == "rom_size") {
      config.rom_size = ParseInt(value);
    } else if (key == "symbols") {
      config.symbols_format = ParseStringValue(value);
    } else if (key == "symbols_path") {
      config.symbols_path = ParseStringValue(value);
    } else if (key == "lsp_log_enabled") {
      config.lsp_log_enabled = ParseBool(value);
    } else if (key == "lsp_log_path") {
      config.lsp_log_path = ParseStringValue(value);
    } else if (key == "warn_unused_symbols") {
      config.warn_unused_symbols = ParseBool(value);
    } else if (key == "warn_branch_outside_bank") {
      config.warn_branch_outside_bank = ParseBool(value);
    } else if (key == "warn_unknown_width") {
      config.warn_unknown_width = ParseBool(value);
    } else if (key == "warn_org_collision") {
      config.warn_org_collision = ParseBool(value);
    } else if (key == "warn_unauthorized_hook") {
      config.warn_unauthorized_hook = ParseBool(value);
    } else if (key == "prohibited_memory_ranges") {
      ApplyArrayKey(&config, key, value);
    }
  }

  if (!pending_key.empty()) {
    ApplyArrayKey(&config, pending_key, pending_value);
  }

  return config;
}

Config LoadConfigIfExists(const std::string& path) {
  std::ifstream file(path);
  if (!file.good()) {
    return Config{};
  }
  file.close();
  return LoadConfigFile(path, nullptr);
}

}  // namespace z3dk
