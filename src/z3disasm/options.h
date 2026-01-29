#ifndef Z3DISASM_OPTIONS_H_
#define Z3DISASM_OPTIONS_H_

#include <filesystem>
#include <string>

namespace z3disasm {

struct Options {
  std::filesystem::path rom_path;
  std::filesystem::path symbols_path;
  std::filesystem::path labels_path;
  std::filesystem::path hooks_path;
  bool hooks_auto = false;
  std::filesystem::path out_dir;
  int m_width_bytes = 1;
  int x_width_bytes = 1;
  int bank_start = 0;
  int bank_end = -1;
  bool lorom = true;
};

void PrintUsage(const char* name);
bool ParseArgs(int argc, const char* argv[], Options* options);

}  // namespace z3disasm

#endif  // Z3DISASM_OPTIONS_H_
