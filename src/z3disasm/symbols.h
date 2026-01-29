#ifndef Z3DISASM_SYMBOLS_H_
#define Z3DISASM_SYMBOLS_H_

#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>

namespace z3disasm {

struct LabelIndex {
  std::unordered_map<uint32_t, std::vector<std::string>> labels;
};

void AddLabel(LabelIndex* index, uint32_t address, std::string label);
bool LoadSymbols(const std::filesystem::path& path, LabelIndex* index);

}  // namespace z3disasm

#endif  // Z3DISASM_SYMBOLS_H_
