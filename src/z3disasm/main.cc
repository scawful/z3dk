#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "nlohmann/json.hpp"
#include "z3dk_core/opcode_table.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct HookEntry {
  uint32_t address = 0;
  uint32_t size = 0;
  std::string name;
  std::string kind;
  std::string target;
  std::string source;
  std::string note;
};

using HookMap = std::unordered_map<uint32_t, std::vector<HookEntry>>;

struct LabelIndex {
  std::unordered_map<uint32_t, std::vector<std::string>> labels;
};

struct Options {
  fs::path rom_path;
  fs::path symbols_path;
  fs::path labels_path;
  fs::path hooks_path;
  bool hooks_auto = false;
  fs::path out_dir;
  int m_width_bytes = 1;
  int x_width_bytes = 1;
  int bank_start = 0;
  int bank_end = -1;
  bool lorom = true;
};

void PrintUsage(const char* name) {
  std::cout << "Usage: " << name
            << " --rom <path> --out <dir> [options]\n\n"
            << "Options:\n"
            << "  --rom <path>         ROM file to disassemble\n"
            << "  --symbols <path>     Optional .sym/.mlb symbols file\n"
            << "  --labels <path>      Optional label map (.csv/.sym/.mlb)\n"
      << "  --hooks [path]       Optional hooks.json manifest (defaults to hooks.json near ROM)\n"
            << "  --out <dir>          Output directory for bank_XX.asm\n"
            << "  --bank-start <hex>   First bank to emit (default 0)\n"
            << "  --bank-end <hex>     Last bank to emit (default last bank)\n"
            << "  --m-width <8|16>     Default M width (bytes inferred via REP/SEP)\n"
            << "  --x-width <8|16>     Default X width (bytes inferred via REP/SEP)\n"
            << "  --mapper <lorom>     Mapper (lorom only for now)\n"
            << "  -h, --help           Show help\n";
}

bool StartsWith(std::string_view text, std::string_view prefix) {
  return text.substr(0, prefix.size()) == prefix;
}

std::string Trim(std::string_view text) {
  size_t start = 0;
  while (start < text.size() &&
         std::isspace(static_cast<unsigned char>(text[start]))) {
    ++start;
  }
  size_t end = text.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  return std::string(text.substr(start, end - start));
}

std::optional<uint32_t> ParseHex(std::string_view text) {
  std::string value = Trim(text);
  if (value.empty()) {
    return std::nullopt;
  }
  if (StartsWith(value, "0x") || StartsWith(value, "0X")) {
    value = value.substr(2);
  }
  uint32_t out = 0;
  std::istringstream stream(value);
  stream >> std::hex >> out;
  if (!stream.fail()) {
    return out;
  }
  return std::nullopt;
}

std::optional<int> ParseInt(std::string_view text) {
  std::string value = Trim(text);
  if (value.empty()) {
    return std::nullopt;
  }
  try {
    size_t idx = 0;
    int out = std::stoi(value, &idx, 0);
    if (idx == value.size()) {
      return out;
    }
  } catch (...) {
  }
  return std::nullopt;
}

std::string Hex(uint32_t value, int width) {
  std::ostringstream out;
  out << '$' << std::uppercase << std::hex;
  out.width(width);
  out.fill('0');
  out << value;
  return out.str();
}

bool ReadFile(const fs::path& path, std::vector<uint8_t>* data) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return false;
  }
  file.seekg(0, std::ios::end);
  std::streamsize size = file.tellg();
  if (size <= 0) {
    return false;
  }
  file.seekg(0, std::ios::beg);
  data->resize(static_cast<size_t>(size));
  file.read(reinterpret_cast<char*>(data->data()), size);
  return file.good();
}

uint32_t PcToSnesLoRom(uint32_t pc) {
  uint32_t bank = pc / 0x8000;
  uint32_t addr = pc % 0x8000;
  return (bank << 16) | (addr + 0x8000);
}

void AddLabel(LabelIndex* index, uint32_t address, std::string label) {
  if (label.empty()) {
    return;
  }
  index->labels[address].push_back(std::move(label));
}

bool LoadSymbolsMlb(const fs::path& path, LabelIndex* index) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return false;
  }
  std::string line;
  while (std::getline(file, line)) {
    std::string_view view(line);
    if (view.empty() || view.front() == ';' || view.front() == '#') {
      continue;
    }
    std::string cleaned = Trim(view);
    if (cleaned.empty()) {
      continue;
    }
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= cleaned.size()) {
      size_t pos = cleaned.find(':', start);
      if (pos == std::string::npos) {
        parts.push_back(cleaned.substr(start));
        break;
      }
      parts.push_back(cleaned.substr(start, pos - start));
      start = pos + 1;
    }
    if (parts.size() < 3) {
      continue;
    }
    std::string type = parts[0];
    if (type != "SnesPrgRom" && type != "PRG") {
      continue;
    }
    auto addr = ParseHex(parts[1]);
    if (!addr.has_value()) {
      continue;
    }
    std::string label = parts[2];
    if (!label.empty() && label.front() == ':') {
      label.erase(label.begin());
    }
    AddLabel(index, *addr, label);
  }
  return true;
}

bool LoadSymbolsSym(const fs::path& path, LabelIndex* index) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return false;
  }
  std::string line;
  bool in_labels = false;
  while (std::getline(file, line)) {
    std::string cleaned = Trim(line);
    if (cleaned.empty() || cleaned.front() == ';') {
      continue;
    }
    if (cleaned.front() == '[') {
      in_labels = (cleaned == "[labels]");
      continue;
    }
    if (!in_labels) {
      continue;
    }
    std::istringstream stream(cleaned);
    std::string addr_token;
    std::string label_token;
    if (!(stream >> addr_token >> label_token)) {
      continue;
    }
    auto colon = addr_token.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    auto bank = ParseHex(addr_token.substr(0, colon));
    auto addr = ParseHex(addr_token.substr(colon + 1));
    if (!bank.has_value() || !addr.has_value()) {
      continue;
    }
    uint32_t address = ((*bank & 0xFF) << 16) | (*addr & 0xFFFF);
    if (!label_token.empty() && label_token.front() == ':') {
      label_token.erase(label_token.begin());
    }
    AddLabel(index, address, label_token);
  }
  return true;
}

bool LoadLabelsCsv(const fs::path& path, LabelIndex* index) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return false;
  }
  std::string line;
  bool header = true;
  while (std::getline(file, line)) {
    if (header) {
      header = false;
      continue;
    }
    std::string cleaned = Trim(line);
    if (cleaned.empty()) {
      continue;
    }
    std::vector<std::string> columns;
    std::string current;
    bool in_quotes = false;
    for (size_t i = 0; i < cleaned.size(); ++i) {
      char ch = cleaned[i];
      if (ch == '"') {
        if (in_quotes && i + 1 < cleaned.size() && cleaned[i + 1] == '"') {
          current.push_back('"');
          ++i;
          continue;
        }
        in_quotes = !in_quotes;
        continue;
      }
      if (ch == ',' && !in_quotes) {
        columns.push_back(Trim(current));
        current.clear();
        continue;
      }
      current.push_back(ch);
    }
    columns.push_back(Trim(current));
    if (columns.size() < 2) {
      continue;
    }
    std::string addr_token = columns[0];
    std::string label = columns[1];
    if (!addr_token.empty() && addr_token.front() == '"') {
      addr_token.erase(addr_token.begin());
    }
    if (!addr_token.empty() && addr_token.back() == '"') {
      addr_token.pop_back();
    }
    if (!label.empty() && label.front() == '"') {
      label.erase(label.begin());
    }
    if (!label.empty() && label.back() == '"') {
      label.pop_back();
    }
    if (addr_token == "address" || addr_token == "Address") {
      continue;
    }
    if (!addr_token.empty() && addr_token.front() == '$') {
      addr_token.erase(addr_token.begin());
    }
    auto colon = addr_token.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    auto bank = ParseHex(addr_token.substr(0, colon));
    auto addr = ParseHex(addr_token.substr(colon + 1));
    if (!bank.has_value() || !addr.has_value()) {
      continue;
    }
    uint32_t address = ((*bank & 0xFF) << 16) | (*addr & 0xFFFF);
    AddLabel(index, address, label);
  }
  return true;
}

bool LoadSymbols(const fs::path& path, LabelIndex* index) {
  if (path.empty()) {
    return true;
  }
  auto ext = path.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  if (ext == ".csv") {
    return LoadLabelsCsv(path, index);
  }
  if (ext == ".mlb") {
    return LoadSymbolsMlb(path, index);
  }
  if (ext == ".sym") {
    return LoadSymbolsSym(path, index);
  }
  return false;
}

std::optional<uint32_t> ParseJsonAddress(const json& value) {
  if (value.is_number_unsigned()) {
    return value.get<uint32_t>();
  }
  if (value.is_string()) {
    return ParseHex(value.get<std::string>());
  }
  return std::nullopt;
}

bool LoadHooks(const fs::path& path, HookMap* hooks, std::string* error) {
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
    (*hooks)[hook.address].push_back(std::move(hook));
  }
  return true;
}

std::string FormatOperand(const z3dk::OpcodeInfo& info, const uint8_t* data,
                          uint32_t snes, int m_width, int x_width,
                          const LabelIndex& labels) {
  auto label_for = [&](uint32_t address) -> std::optional<std::string> {
    auto it = labels.labels.find(address);
    if (it == labels.labels.end() || it->second.empty()) {
      uint32_t mirror = address ^ 0x800000;
      it = labels.labels.find(mirror);
      if (it == labels.labels.end() || it->second.empty()) {
        return std::nullopt;
      }
    }
    return it->second.front();
  };

  switch (info.mode) {
    case z3dk::AddrMode::kImmediate8:
      return std::string("#") + Hex(data[0], 2);
    case z3dk::AddrMode::kImmediate16:
      return std::string("#") +
             Hex(static_cast<uint32_t>(data[0] | (data[1] << 8)), 4);
    case z3dk::AddrMode::kImmediateM: {
      int width = std::max(1, m_width);
      uint32_t value = data[0];
      if (width == 2) {
        value |= static_cast<uint32_t>(data[1] << 8);
      }
      return std::string("#") + Hex(value, width * 2);
    }
    case z3dk::AddrMode::kImmediateX: {
      int width = std::max(1, x_width);
      uint32_t value = data[0];
      if (width == 2) {
        value |= static_cast<uint32_t>(data[1] << 8);
      }
      return std::string("#") + Hex(value, width * 2);
    }
    case z3dk::AddrMode::kRelative8: {
      int8_t rel = static_cast<int8_t>(data[0]);
      uint32_t target = (snes & 0xFF0000) |
                        static_cast<uint16_t>((snes + 2 + rel) & 0xFFFF);
      if (auto label = label_for(target)) {
        return *label;
      }
      return Hex(target, 6);
    }
    case z3dk::AddrMode::kRelative16: {
      int16_t rel = static_cast<int16_t>(data[0] | (data[1] << 8));
      uint32_t target = (snes & 0xFF0000) |
                        static_cast<uint16_t>((snes + 3 + rel) & 0xFFFF);
      if (auto label = label_for(target)) {
        return *label;
      }
      return Hex(target, 6);
    }
    case z3dk::AddrMode::kDirectPage:
      return Hex(data[0], 2);
    case z3dk::AddrMode::kDirectPageX:
      return Hex(data[0], 2) + ",X";
    case z3dk::AddrMode::kDirectPageY:
      return Hex(data[0], 2) + ",Y";
    case z3dk::AddrMode::kDirectPageIndirect:
      return "(" + Hex(data[0], 2) + ")";
    case z3dk::AddrMode::kDirectPageIndexedIndirect:
      return "(" + Hex(data[0], 2) + ",X)";
    case z3dk::AddrMode::kDirectPageIndirectIndexedY:
      return "(" + Hex(data[0], 2) + "),Y";
    case z3dk::AddrMode::kDirectPageIndirectLong:
      return "[" + Hex(data[0], 2) + "]";
    case z3dk::AddrMode::kDirectPageIndirectLongY:
      return "[" + Hex(data[0], 2) + "],Y";
    case z3dk::AddrMode::kStackRelative:
      return Hex(data[0], 2) + ",S";
    case z3dk::AddrMode::kStackRelativeIndirectY:
      return "(" + Hex(data[0], 2) + ",S),Y";
    case z3dk::AddrMode::kAbsolute: {
      uint32_t value = data[0] | (data[1] << 8);
      uint32_t addr = (snes & 0xFF0000) | value;
      if (auto label = label_for(addr)) {
        return *label;
      }
      return Hex(value, 4);
    }
    case z3dk::AddrMode::kAbsoluteX: {
      uint32_t value = data[0] | (data[1] << 8);
      uint32_t addr = (snes & 0xFF0000) | value;
      if (auto label = label_for(addr)) {
        return *label + ",X";
      }
      return Hex(value, 4) + ",X";
    }
    case z3dk::AddrMode::kAbsoluteY: {
      uint32_t value = data[0] | (data[1] << 8);
      uint32_t addr = (snes & 0xFF0000) | value;
      if (auto label = label_for(addr)) {
        return *label + ",Y";
      }
      return Hex(value, 4) + ",Y";
    }
    case z3dk::AddrMode::kAbsoluteLong: {
      uint32_t value = data[0] | (data[1] << 8) | (data[2] << 16);
      if (auto label = label_for(value)) {
        return *label;
      }
      return Hex(value, 6);
    }
    case z3dk::AddrMode::kAbsoluteLongX: {
      uint32_t value = data[0] | (data[1] << 8) | (data[2] << 16);
      if (auto label = label_for(value)) {
        return *label + ",X";
      }
      return Hex(value, 6) + ",X";
    }
    case z3dk::AddrMode::kAbsoluteIndirect: {
      uint32_t value = data[0] | (data[1] << 8);
      return "(" + Hex(value, 4) + ")";
    }
    case z3dk::AddrMode::kAbsoluteIndexedIndirect: {
      uint32_t value = data[0] | (data[1] << 8);
      return "(" + Hex(value, 4) + ",X)";
    }
    case z3dk::AddrMode::kAbsoluteIndirectLong: {
      uint32_t value = data[0] | (data[1] << 8);
      return "[" + Hex(value, 4) + "]";
    }
    case z3dk::AddrMode::kBlockMove: {
      std::string dest = Hex(data[0], 2);
      std::string src = Hex(data[1], 2);
      return dest + "," + src;
    }
    case z3dk::AddrMode::kImplied:
    default:
      return "";
  }
}

void EmitHookComment(std::ostream& out, const HookEntry& hook) {
  out << "; HOOK";
  if (!hook.name.empty()) {
    out << " " << hook.name;
  }
  if (!hook.kind.empty()) {
    out << " [" << hook.kind << "]";
  }
  if (!hook.target.empty()) {
    out << " -> " << hook.target;
  }
  if (!hook.source.empty()) {
    out << " (" << hook.source << ")";
  }
  if (hook.size > 0) {
    out << " size=" << hook.size;
  }
  if (!hook.note.empty()) {
    out << " ; " << hook.note;
  }
  out << "\n";
}

bool ParseArgs(int argc, const char* argv[], Options* options) {
  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i]);
    if (arg == "--help" || arg == "-h") {
      return false;
    }
    if (arg == "--rom" && i + 1 < argc) {
      options->rom_path = argv[++i];
      continue;
    }
    if (arg == "--symbols" && i + 1 < argc) {
      options->symbols_path = argv[++i];
      continue;
    }
    if (arg == "--labels" && i + 1 < argc) {
      options->labels_path = argv[++i];
      continue;
    }
    if (arg.rfind("--labels=", 0) == 0) {
      options->labels_path = arg.substr(std::string("--labels=").size());
      continue;
    }
    if (arg == "--hooks") {
      if (i + 1 < argc && argv[i + 1][0] != '-') {
        options->hooks_path = argv[++i];
      } else {
        options->hooks_auto = true;
      }
      continue;
    }
    if (arg.rfind("--hooks=", 0) == 0) {
      options->hooks_path = arg.substr(std::string("--hooks=").size());
      continue;
    }
    if (arg == "--out" && i + 1 < argc) {
      options->out_dir = argv[++i];
      continue;
    }
    if (arg == "--bank-start" && i + 1 < argc) {
      auto value = ParseHex(argv[++i]);
      if (value.has_value()) {
        options->bank_start = static_cast<int>(*value);
      }
      continue;
    }
    if (arg == "--bank-end" && i + 1 < argc) {
      auto value = ParseHex(argv[++i]);
      if (value.has_value()) {
        options->bank_end = static_cast<int>(*value);
      }
      continue;
    }
    if (arg == "--m-width" && i + 1 < argc) {
      auto value = ParseInt(argv[++i]);
      if (value.has_value()) {
        options->m_width_bytes = (*value == 16) ? 2 : 1;
      }
      continue;
    }
    if (arg == "--x-width" && i + 1 < argc) {
      auto value = ParseInt(argv[++i]);
      if (value.has_value()) {
        options->x_width_bytes = (*value == 16) ? 2 : 1;
      }
      continue;
    }
    if (arg == "--mapper" && i + 1 < argc) {
      std::string mapper = argv[++i];
      options->lorom = (mapper == "lorom");
      continue;
    }
  }
  return true;
}

}  // namespace

int main(int argc, const char* argv[]) {
  Options options;
  if (!ParseArgs(argc, argv, &options)) {
    PrintUsage(argv[0]);
    return 0;
  }
  if (options.rom_path.empty() || options.out_dir.empty()) {
    PrintUsage(argv[0]);
    return 1;
  }
  if (!options.lorom) {
    std::cerr << "Only lorom mapper is supported right now\n";
    return 1;
  }

  if (options.hooks_path.empty()) {
    fs::path candidate = options.rom_path.parent_path() / "hooks.json";
    if (options.hooks_auto || fs::exists(candidate)) {
      options.hooks_path = candidate;
    }
  }

  std::vector<uint8_t> rom;
  if (!ReadFile(options.rom_path, &rom)) {
    std::cerr << "Failed to read ROM: " << options.rom_path << "\n";
    return 1;
  }
  if (rom.size() % 0x8000 == 512) {
    rom.erase(rom.begin(), rom.begin() + 512);
  }
  if (rom.empty()) {
    std::cerr << "ROM is empty after header stripping\n";
    return 1;
  }

  LabelIndex labels;
  if (!options.labels_path.empty() &&
      !LoadSymbols(options.labels_path, &labels)) {
    std::cerr << "Failed to load labels: " << options.labels_path << "\n";
    return 1;
  }
  if (!options.symbols_path.empty() &&
      !LoadSymbols(options.symbols_path, &labels)) {
    std::cerr << "Failed to load symbols: " << options.symbols_path << "\n";
    return 1;
  }

  HookMap hooks;
  std::string hook_error;
  if (!LoadHooks(options.hooks_path, &hooks, &hook_error)) {
    std::cerr << hook_error << "\n";
    return 1;
  }

  fs::create_directories(options.out_dir);

  int total_banks = static_cast<int>(rom.size() / 0x8000);
  int bank_start = std::max(0, options.bank_start);
  int bank_end = options.bank_end >= 0 ? options.bank_end : (total_banks - 1);
  bank_end = std::min(bank_end, total_banks - 1);

  for (int bank = bank_start; bank <= bank_end; ++bank) {
    fs::path out_path = options.out_dir / ("bank_" + Hex(bank, 2).substr(1) + ".asm");
    std::ofstream out(out_path);
    if (!out.is_open()) {
      std::cerr << "Failed to write " << out_path << "\n";
      return 1;
    }

    uint32_t bank_pc = static_cast<uint32_t>(bank * 0x8000);
    uint32_t bank_end_pc = bank_pc + 0x8000;
    if (bank_end_pc > rom.size()) {
      bank_end_pc = static_cast<uint32_t>(rom.size());
    }

    uint32_t snes_base = PcToSnesLoRom(bank_pc);
    out << "; bank " << Hex(bank, 2) << "\n";
    out << "org " << Hex(snes_base, 6) << "\n\n";

    int m_width = std::max(1, options.m_width_bytes);
    int x_width = std::max(1, options.x_width_bytes);

    for (uint32_t pc = bank_pc; pc < bank_end_pc;) {
      uint32_t snes = PcToSnesLoRom(pc);

      auto label_it = labels.labels.find(snes);
      if (label_it == labels.labels.end()) {
        label_it = labels.labels.find(snes ^ 0x800000);
      }
      if (label_it != labels.labels.end()) {
        for (const auto& label : label_it->second) {
          out << label << ":\n";
        }
      }

      auto hook_it = hooks.find(snes);
      if (hook_it == hooks.end()) {
        hook_it = hooks.find(snes ^ 0x800000);
      }
      if (hook_it != hooks.end()) {
        for (const auto& hook : hook_it->second) {
          EmitHookComment(out, hook);
        }
      }

      uint8_t opcode = rom[pc];
      const auto& info = z3dk::GetOpcodeInfo(opcode);
      int operand_size = z3dk::OperandSizeBytes(info.mode, m_width, x_width);
      if (pc + 1 + operand_size > bank_end_pc) {
        out << "  db " << Hex(opcode, 2) << "\n";
        ++pc;
        continue;
      }

      std::string operand;
      if (operand_size > 0) {
        operand = FormatOperand(info, &rom[pc + 1], snes, m_width, x_width,
                                labels);
      }

      out << "  " << info.mnemonic;
      if (!operand.empty()) {
        out << " " << operand;
      }
      out << "\n";

      if (std::string(info.mnemonic) == "REP" && operand_size == 1) {
        uint8_t mask = rom[pc + 1];
        if (mask & 0x20) {
          m_width = 2;
        }
        if (mask & 0x10) {
          x_width = 2;
        }
      } else if (std::string(info.mnemonic) == "SEP" && operand_size == 1) {
        uint8_t mask = rom[pc + 1];
        if (mask & 0x20) {
          m_width = 1;
        }
        if (mask & 0x10) {
          x_width = 1;
        }
      } else if (std::string(info.mnemonic) == "XCE") {
        m_width = 1;
        x_width = 1;
      }

      pc += 1 + operand_size;
    }
  }

  return 0;
}
