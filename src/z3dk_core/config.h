#ifndef Z3DK_CORE_CONFIG_H
#define Z3DK_CORE_CONFIG_H

#include <optional>
#include <string>
#include <vector>

namespace z3dk {

struct Config {
  std::vector<std::string> include_paths;
  std::vector<std::string> defines;
  std::vector<std::string> emits;
  std::optional<std::string> std_includes_path;
  std::optional<std::string> std_defines_path;
  std::optional<std::string> mapper;
  std::optional<int> rom_size;
  std::optional<std::string> symbols_format;
  std::optional<std::string> symbols_path;
};

Config LoadConfigFile(const std::string& path, std::string* error);
Config LoadConfigIfExists(const std::string& path);

}  // namespace z3dk

#endif  // Z3DK_CORE_CONFIG_H
