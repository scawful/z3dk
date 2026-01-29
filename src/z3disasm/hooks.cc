#include "hooks.h"
#include "utils.h"
#include <fstream>
#include "nlohmann/json.hpp"

namespace z3disasm {

using json = nlohmann::json;

std::optional<uint32_t> ParseJsonAddress(const json& value) {
  if (value.is_number_unsigned()) {
    return value.get<uint32_t>();
  }
  if (value.is_string()) {
    return ParseHex(value.get<std::string>());
  }
  return std::nullopt;
}

std::optional<int> ParseJsonInt(const json& value) {
  if (value.is_number_integer()) {
    return value.get<int>();
  }
  if (value.is_number_unsigned()) {
    return static_cast<int>(value.get<uint32_t>());
  }
  if (value.is_boolean()) {
    return value.get<bool>() ? 8 : 16;
  }
  if (value.is_string()) {
    auto parsed = ParseHex(value.get<std::string>());
    if (parsed.has_value()) {
      return static_cast<int>(*parsed);
    }
  }
  return std::nullopt;
}

bool LoadHooks(const std::filesystem::path& path, HookMap* hooks, std::string* error) {
  if (path.empty()) {
    return true;
  }
  std::ifstream file(path);
  if (!file.is_open()) {
    if (error) {
      *error = "Unable to read hooks manifest: " + path.string();
    }
    return false;
  }
  json root;
  try {
    file >> root;
  } catch (...) {
    if (error) {
      *error = "Invalid hooks manifest JSON";
    }
    return false;
  }
  if (!root.is_object() || !root.contains("hooks")) {
    return true;
  }
  for (const auto& entry : root["hooks"]) {
    if (!entry.is_object()) {
      continue;
    }
    auto addr = entry.contains("address") ? ParseJsonAddress(entry["address"]) :
                                            std::nullopt;
    if (!addr.has_value()) {
      continue;
    }
    HookEntry hook;
    hook.address = *addr;
    hook.size = entry.value("size", 0u);
    hook.name = entry.value("name", "");
    hook.kind = entry.value("kind", "");
    hook.target = entry.value("target", "");
    hook.source = entry.value("source", "");
    hook.note = entry.value("note", "");
    hook.module = entry.value("module", "");
    hook.abi_class = entry.value("abi_class", "");
    hook.skip_abi = entry.value("skip_abi", false);
    if (entry.contains("expected_m")) {
      if (auto exp = ParseJsonInt(entry["expected_m"])) {
        hook.expected_m = *exp;
      }
    }
    if (entry.contains("expected_x")) {
      if (auto exp = ParseJsonInt(entry["expected_x"])) {
        hook.expected_x = *exp;
      }
    }
    (*hooks)[hook.address].push_back(std::move(hook));
  }
  return true;
}

}  // namespace z3disasm
