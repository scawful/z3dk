#ifndef Z3DISASM_HOOKS_H_
#define Z3DISASM_HOOKS_H_

#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include "nlohmann/json.hpp"

namespace z3disasm {

struct HookEntry {
  uint32_t address = 0;
  uint32_t size = 0;
  std::string name;
  std::string kind;
  std::string target;
  std::string source;
  std::string note;
  std::string module;
  std::string abi_class;
  int expected_m = 0;
  int expected_x = 0;
  bool skip_abi = false;
};

using HookMap = std::unordered_map<uint32_t, std::vector<HookEntry>>;

bool LoadHooks(const std::filesystem::path& path, HookMap* hooks, std::string* error);

}  // namespace z3disasm

#endif  // Z3DISASM_HOOKS_H_
