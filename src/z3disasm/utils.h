#ifndef Z3DISASM_UTILS_H_
#define Z3DISASM_UTILS_H_

#include <string>
#include <string_view>
#include <optional>
#include <cstdint>
#include <vector>
#include <filesystem>

namespace z3disasm {

bool StartsWith(std::string_view text, std::string_view prefix);
std::string Trim(std::string_view text);
std::optional<uint32_t> ParseHex(std::string_view text);
std::optional<int> ParseInt(std::string_view text);
std::string Hex(uint32_t value, int width);
bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>* data);
uint32_t PcToSnesLoRom(uint32_t pc);

}  // namespace z3disasm

#endif  // Z3DISASM_UTILS_H_
