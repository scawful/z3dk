#ifndef Z3DK_CORE_ASSEMBLER_H
#define Z3DK_CORE_ASSEMBLER_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace z3dk {

enum class DiagnosticSeverity {
  kError,
  kWarning,
};

struct Diagnostic {
  DiagnosticSeverity severity = DiagnosticSeverity::kError;
  std::string message;
  std::string filename;
  int line = 0;
  int column = 0;
  std::string raw;
};

struct Label {
  std::string name;
  uint32_t address = 0;
  bool used = false;
};

struct Define {
  std::string name;
  std::string value;
};

struct WrittenBlock {
  int pc_offset = 0;
  int snes_offset = 0;
  int num_bytes = 0;
};

struct SourceFile {
  int id = 0;
  uint32_t crc = 0;
  std::string path;
};

struct SourceMapEntry {
  uint32_t address = 0;
  int file_id = 0;
  int line = 0;
};

struct SourceMap {
  std::vector<SourceFile> files;
  std::vector<SourceMapEntry> entries;
};

struct MemoryFile {
  std::string path;
  std::string contents;
};

struct AssembleOptions {
  std::string patch_path;
  std::vector<uint8_t> rom_data;
  std::vector<std::string> include_paths;
  std::vector<std::pair<std::string, std::string>> defines;
  std::string std_includes_path;
  std::string std_defines_path;
  std::vector<MemoryFile> memory_files;
  bool full_call_stack = false;
  bool override_checksum = false;
  bool generate_checksum = true;
  bool capture_nocash_symbols = false;
  bool inject_snes_registers = false;
};

struct AssembleResult {
  bool success = false;
  std::vector<Diagnostic> diagnostics;
  std::vector<std::string> prints;
  std::vector<Label> labels;
  std::vector<Define> defines;
  std::vector<WrittenBlock> written_blocks;
  std::vector<uint8_t> rom_data;
  int rom_size = 0;
  int mapper = 0;
  SourceMap source_map;
  std::string wla_symbols;
  std::string nocash_symbols;
};

class Assembler {
 public:
  AssembleResult Assemble(const AssembleOptions& options) const;

 private:
  static std::string CopySymbolsFile(std::string_view format);
  static void ParseWlaSourceMap(std::string_view content, SourceMap* map);
};

}  // namespace z3dk

#endif  // Z3DK_CORE_ASSEMBLER_H
