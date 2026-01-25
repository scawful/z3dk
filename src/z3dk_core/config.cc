#include "z3dk_core/config.h"

#include <cctype>
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

}  // namespace

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
  while (std::getline(file, line)) {
    ++line_number;
    std::string stripped = StripComments(line);
    std::string trimmed = Trim(stripped);
    if (trimmed.empty()) {
      continue;
    }

    auto equals = trimmed.find('=');
    if (equals == std::string::npos) {
      continue;
    }

    std::string key = Trim(trimmed.substr(0, equals));
    std::string value = Trim(trimmed.substr(equals + 1));
    if (key == "include_paths") {
      config.include_paths = ParseStringArray(value);
    } else if (key == "defines") {
      config.defines = ParseStringArray(value);
    } else if (key == "emit" || key == "emits") {
      config.emits = ParseStringArray(value);
    } else if (key == "std_includes") {
      config.std_includes_path = ParseStringValue(value);
    } else if (key == "std_defines") {
      config.std_defines_path = ParseStringValue(value);
    } else if (key == "mapper") {
      config.mapper = ParseStringValue(value);
    } else if (key == "rom_size") {
      config.rom_size = ParseInt(value);
    } else if (key == "symbols") {
      config.symbols_format = ParseStringValue(value);
    } else if (key == "symbols_path") {
      config.symbols_path = ParseStringValue(value);
    }
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
