#include "options.h"
#include "utils.h"
#include <iostream>

namespace z3disasm {

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

}  // namespace z3disasm
