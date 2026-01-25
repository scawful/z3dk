#include "z3dk_core/assembler.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "interface-lib.h"

namespace z3dk {
namespace {

std::string_view Trim(std::string_view text) {
  size_t start = 0;
  while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0) {
    ++start;
  }
  size_t end = text.size();
  while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
    --end;
  }
  return text.substr(start, end - start);
}

bool ParseHex(std::string_view text, uint32_t* value) {
  std::string tmp(text);
  char* end = nullptr;
  unsigned long parsed = std::strtoul(tmp.c_str(), &end, 16);
  if (end == tmp.c_str() || *end != '\0') {
    return false;
  }
  *value = static_cast<uint32_t>(parsed);
  return true;
}

}  // namespace

AssembleResult Assembler::Assemble(const AssembleOptions& options) const {
  AssembleResult result;
  if (options.patch_path.empty()) {
    Diagnostic diag;
    diag.severity = DiagnosticSeverity::kError;
    diag.message = "patch_path is required";
    result.diagnostics.push_back(std::move(diag));
    return result;
  }

  asar_reset();

  std::vector<uint8_t> rom_buffer = options.rom_data;
  int max_size = asar_maxromsize();
  if (max_size <= 0) {
    max_size = 16 * 1024 * 1024;
  }
  int rom_length = static_cast<int>(rom_buffer.size());
  if (rom_length > max_size) {
    Diagnostic diag;
    diag.severity = DiagnosticSeverity::kError;
    diag.message = "ROM buffer larger than max supported size";
    result.diagnostics.push_back(std::move(diag));
    return result;
  }

  std::unique_ptr<unsigned char, decltype(&std::free)> rom_storage(
      static_cast<unsigned char*>(std::malloc(static_cast<size_t>(max_size))),
      &std::free);
  if (!rom_storage) {
    Diagnostic diag;
    diag.severity = DiagnosticSeverity::kError;
    diag.message = "Failed to allocate ROM buffer";
    result.diagnostics.push_back(std::move(diag));
    return result;
  }

  std::memset(rom_storage.get(), 0, static_cast<size_t>(max_size));
  if (!rom_buffer.empty()) {
    std::memcpy(rom_storage.get(), rom_buffer.data(),
                static_cast<size_t>(rom_length));
  }

  std::vector<std::string> include_storage = options.include_paths;
  std::vector<const char*> include_cstrs;
  include_cstrs.reserve(include_storage.size());
  for (const auto& path : include_storage) {
    include_cstrs.push_back(path.c_str());
  }

  std::vector<std::string> define_names;
  std::vector<std::string> define_values;
  std::vector<definedata> define_data;
  define_names.reserve(options.defines.size());
  define_values.reserve(options.defines.size());
  define_data.reserve(options.defines.size());
  for (const auto& def : options.defines) {
    define_names.push_back(def.first);
    define_values.push_back(def.second);
    definedata entry{};
    entry.name = define_names.back().c_str();
    entry.contents = define_values.back().c_str();
    define_data.push_back(entry);
  }

  std::vector<std::string> memory_paths;
  std::vector<std::string> memory_contents;
  std::vector<memoryfile> memory_files;
  memory_paths.reserve(options.memory_files.size());
  memory_contents.reserve(options.memory_files.size());
  memory_files.reserve(options.memory_files.size());
  for (const auto& file : options.memory_files) {
    memory_paths.push_back(file.path);
    memory_contents.push_back(file.contents);
    memoryfile mem{};
    mem.path = memory_paths.back().c_str();
    mem.buffer = memory_contents.back().data();
    mem.length = memory_contents.back().size();
    memory_files.push_back(mem);
  }

  patchparams params{};
  int expected_size = asar_patchparams_size();
  if (expected_size <= 0) {
    expected_size = static_cast<int>(sizeof(params));
  }
  params.structsize = expected_size;
  params.patchloc = options.patch_path.c_str();
  params.romdata = reinterpret_cast<char*>(rom_storage.get());
  params.buflen = max_size;
  params.romlen = &rom_length;
  params.includepaths = include_cstrs.empty() ? nullptr : include_cstrs.data();
  params.numincludepaths = static_cast<int>(include_cstrs.size());
  params.additional_defines = define_data.empty() ? nullptr : define_data.data();
  params.additional_define_count = static_cast<int>(define_data.size());
  params.stdincludesfile = options.std_includes_path.empty() ? nullptr : options.std_includes_path.c_str();
  params.stddefinesfile = options.std_defines_path.empty() ? nullptr : options.std_defines_path.c_str();
  params.memory_files = memory_files.empty() ? nullptr : memory_files.data();
  params.memory_file_count = static_cast<int>(memory_files.size());
  params.override_checksum_gen = options.override_checksum;
  params.generate_checksum = options.generate_checksum;
  params.full_call_stack = options.full_call_stack;

  bool ok = asar_patch(&params);

  int error_count = 0;
  const errordata* errors = asar_geterrors(&error_count);
  for (int i = 0; i < error_count; ++i) {
    Diagnostic diag;
    diag.severity = DiagnosticSeverity::kError;
    diag.message = errors[i].rawerrdata ? errors[i].rawerrdata : "";
    diag.raw = errors[i].fullerrdata ? errors[i].fullerrdata : "";
    diag.filename = errors[i].filename ? errors[i].filename : "";
    diag.line = errors[i].line;
    result.diagnostics.push_back(std::move(diag));
  }

  int warning_count = 0;
  const errordata* warnings = asar_getwarnings(&warning_count);
  for (int i = 0; i < warning_count; ++i) {
    Diagnostic diag;
    diag.severity = DiagnosticSeverity::kWarning;
    diag.message = warnings[i].rawerrdata ? warnings[i].rawerrdata : "";
    diag.raw = warnings[i].fullerrdata ? warnings[i].fullerrdata : "";
    diag.filename = warnings[i].filename ? warnings[i].filename : "";
    diag.line = warnings[i].line;
    result.diagnostics.push_back(std::move(diag));
  }

  int print_count = 0;
  const char* const* prints = asar_getprints(&print_count);
  for (int i = 0; i < print_count; ++i) {
    if (prints[i]) {
      result.prints.emplace_back(prints[i]);
    }
  }

  int label_count = 0;
  const labeldata* labels = asar_getalllabels(&label_count);
  for (int i = 0; i < label_count; ++i) {
    Label label;
    label.name = labels[i].name ? labels[i].name : "";
    label.address = static_cast<uint32_t>(labels[i].location);
    result.labels.push_back(std::move(label));
  }

  int define_count = 0;
  const definedata* defines = asar_getalldefines(&define_count);
  for (int i = 0; i < define_count; ++i) {
    Define def;
    def.name = defines[i].name ? defines[i].name : "";
    def.value = defines[i].contents ? defines[i].contents : "";
    result.defines.push_back(std::move(def));
  }

  int block_count = 0;
  const writtenblockdata* blocks = asar_getwrittenblocks(&block_count);
  for (int i = 0; i < block_count; ++i) {
    WrittenBlock block;
    block.pc_offset = blocks[i].pcoffset;
    block.snes_offset = blocks[i].snesoffset;
    block.num_bytes = blocks[i].numbytes;
    result.written_blocks.push_back(block);
  }

  result.mapper = static_cast<int>(asar_getmapper());

  result.success = ok && error_count == 0;
  if (result.success) {
    if (rom_length < 0 || rom_length > max_size) {
      Diagnostic diag;
      diag.severity = DiagnosticSeverity::kError;
      diag.message = "ROM size returned out of range";
      result.diagnostics.push_back(std::move(diag));
      result.success = false;
      return result;
    }
    result.rom_data.assign(rom_storage.get(),
                           rom_storage.get() + static_cast<size_t>(rom_length));
    result.rom_size = static_cast<int>(result.rom_data.size());

    result.wla_symbols = CopySymbolsFile("wla");
    if (!result.wla_symbols.empty()) {
      ParseWlaSourceMap(result.wla_symbols, &result.source_map);
    }
    if (options.capture_nocash_symbols) {
      result.nocash_symbols = CopySymbolsFile("nocash");
    }
  }

  return result;
}

std::string Assembler::CopySymbolsFile(std::string_view format) {
  const char* symbols = asar_getsymbolsfile(std::string(format).c_str());
  if (!symbols) {
    return {};
  }
  return std::string(symbols);
}

void Assembler::ParseWlaSourceMap(std::string_view content, SourceMap* map) {
  if (!map) {
    return;
  }
  map->files.clear();
  map->entries.clear();

  enum class Section {
    kNone,
    kSourceFiles,
    kAddrToLine,
  } section = Section::kNone;

  std::istringstream stream{std::string(content)};
  std::string line;
  while (std::getline(stream, line)) {
    std::string_view view = Trim(line);
    if (view.empty() || view.front() == ';') {
      continue;
    }

    if (view.front() == '[' && view.back() == ']') {
      if (view == "[source files]") {
        section = Section::kSourceFiles;
      } else if (view == "[addr-to-line mapping]") {
        section = Section::kAddrToLine;
      } else {
        section = Section::kNone;
      }
      continue;
    }

    if (section == Section::kSourceFiles) {
      std::istringstream line_stream{std::string(view)};
      std::string id_str;
      std::string crc_str;
      if (!(line_stream >> id_str >> crc_str)) {
        continue;
      }
      std::string path;
      std::getline(line_stream, path);
      path = std::string(Trim(path));

      uint32_t id = 0;
      uint32_t crc = 0;
      if (!ParseHex(id_str, &id) || !ParseHex(crc_str, &crc)) {
        continue;
      }

      SourceFile file;
      file.id = static_cast<int>(id);
      file.crc = crc;
      file.path = path;
      map->files.push_back(std::move(file));
      continue;
    }

    if (section == Section::kAddrToLine) {
      std::istringstream line_stream{std::string(view)};
      std::string addr_token;
      std::string file_line_token;
      if (!(line_stream >> addr_token >> file_line_token)) {
        continue;
      }

      auto addr_split = addr_token.find(':');
      auto file_split = file_line_token.find(':');
      if (addr_split == std::string::npos || file_split == std::string::npos) {
        continue;
      }

      uint32_t bank = 0;
      uint32_t offset = 0;
      uint32_t file_id = 0;
      uint32_t line_num = 0;
      if (!ParseHex(addr_token.substr(0, addr_split), &bank) ||
          !ParseHex(addr_token.substr(addr_split + 1), &offset) ||
          !ParseHex(file_line_token.substr(0, file_split), &file_id) ||
          !ParseHex(file_line_token.substr(file_split + 1), &line_num)) {
        continue;
      }

      SourceMapEntry entry;
      entry.address = (bank << 16) | (offset & 0xFFFF);
      entry.file_id = static_cast<int>(file_id);
      entry.line = static_cast<int>(line_num);
      map->entries.push_back(std::move(entry));
    }
  }
}

}  // namespace z3dk
