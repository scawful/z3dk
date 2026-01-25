#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "z3dk_core/assembler.h"
#include "z3dk_core/config.h"
#include "z3dk_core/emit.h"
#include "z3dk_core/lint.h"

namespace fs = std::filesystem;

namespace {

struct EmitTarget {
  enum class Kind {
    kDiagnostics,
    kSourceMap,
    kSymbolsWla,
    kSymbolsMlb,
    kLint,
    kHooks,
  } kind;
  std::string path;
};

struct CliOptions {
  std::string asm_path;
  std::string rom_path;
  std::string config_path;
  std::string symbols_format;
  std::string symbols_path;
  std::vector<std::string> include_paths;
  std::vector<std::pair<std::string, std::string>> defines;
  std::vector<EmitTarget> emits;
  int lint_m_width_bytes = 1;
  int lint_x_width_bytes = 1;
  bool lint_warn_unknown_width = true;
  bool lint_warn_branch_outside_bank = true;
  bool lint_warn_org_collision = true;
  bool show_help = false;
  bool show_version = false;
};

void PrintUsage(const char* name) {
  std::cout
      << "Usage: " << name << " [options] <asm_file> [rom_file]\n\n"
      << "Options:\n"
      << "  --config=<path>          Use z3dk.toml config file\n"
      << "  -I<path>, --include <p>  Add include search path\n"
      << "  -D<def>[=val], --define  Add define\n"
      << "  --symbols=<none|wla|nocash>\n"
      << "  --symbols-path=<file>    Override symbols output path\n"
      << "  --emit=<target>          Emit diagnostics/sourcemap/symbols outputs\n"
      << "                           Examples: --emit=diagnostics.json\n"
      << "                                     --emit=sourcemap.json\n"
      << "                                     --emit=symbols.mlb\n"
      << "                                     --emit=lint.json\n"
      << "                                     --emit=hooks.json\n"
      << "  --lint-m-width=<8|16>    Default M width for lint (bytes)\n"
      << "  --lint-x-width=<8|16>    Default X width for lint (bytes)\n"
      << "  --lint-no-unknown-width  Disable M/X unknown width warnings\n"
      << "  --lint-no-branch         Disable branch-outside-bank warnings\n"
      << "  --lint-no-org            Disable ORG collision warnings\n"
      << "  --version                Show version\n"
      << "  --help                   Show this message\n";
}

std::optional<std::pair<std::string, std::string>> ParseDefine(const std::string& text) {
  if (text.empty()) {
    return std::nullopt;
  }
  auto pos = text.find('=');
  if (pos == std::string::npos) {
    return std::make_pair(text, std::string());
  }
  return std::make_pair(text.substr(0, pos), text.substr(pos + 1));
}

std::string Basename(const std::string& path) {
  fs::path p(path);
  return p.filename().string();
}

std::string FileStem(const std::string& path) {
  fs::path p(path);
  return p.stem().string();
}

std::string FileExtension(const std::string& path) {
  fs::path p(path);
  return p.extension().string();
}

std::optional<EmitTarget::Kind> ParseEmitKind(const std::string& kind,
                                              const std::string& path) {
  if (kind == "diagnostics") {
    return EmitTarget::Kind::kDiagnostics;
  }
  if (kind == "sourcemap" || kind == "source-map") {
    return EmitTarget::Kind::kSourceMap;
  }
  if (kind == "symbols") {
    if (FileExtension(path) == ".mlb") {
      return EmitTarget::Kind::kSymbolsMlb;
    }
    return EmitTarget::Kind::kSymbolsWla;
  }
  if (kind == "symbols-wla") {
    return EmitTarget::Kind::kSymbolsWla;
  }
  if (kind == "symbols-mlb") {
    return EmitTarget::Kind::kSymbolsMlb;
  }
  if (kind == "symbols-auto") {
    if (FileExtension(path) == ".mlb") {
      return EmitTarget::Kind::kSymbolsMlb;
    }
    return EmitTarget::Kind::kSymbolsWla;
  }
  if (kind == "lint") {
    return EmitTarget::Kind::kLint;
  }
  if (kind == "hooks") {
    return EmitTarget::Kind::kHooks;
  }
  return std::nullopt;
}

bool ParseEmitTarget(const std::string& value, EmitTarget* target,
                     std::string* error) {
  if (value.empty()) {
    if (error) {
      *error = "--emit value is empty";
    }
    return false;
  }

  std::string kind;
  std::string path;
  auto colon = value.find(':');
  if (colon != std::string::npos) {
    kind = value.substr(0, colon);
    path = value.substr(colon + 1);
  } else {
    path = value;
    std::string base = Basename(value);
    auto dot = base.find('.');
    if (dot != std::string::npos) {
      kind = base.substr(0, dot);
    } else {
      kind = "symbols-auto";
    }
  }

  auto kind_value = ParseEmitKind(kind, path);
  if (!kind_value.has_value()) {
    if (error) {
      *error = "Unknown emit target: " + kind;
    }
    return false;
  }
  target->kind = *kind_value;
  target->path = path;
  return true;
}

bool ParseArgs(int argc, const char* argv[], CliOptions* options,
               std::string* error) {
  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i]);
    if (arg == "--help" || arg == "-h") {
      options->show_help = true;
      return true;
    }
    if (arg == "--version") {
      options->show_version = true;
      return true;
    }
    if (arg.rfind("--config=", 0) == 0) {
      options->config_path = arg.substr(std::string("--config=").size());
      continue;
    }
    if (arg.rfind("--symbols=", 0) == 0) {
      options->symbols_format = arg.substr(std::string("--symbols=").size());
      continue;
    }
    if (arg.rfind("--symbols-path=", 0) == 0) {
      options->symbols_path = arg.substr(std::string("--symbols-path=").size());
      continue;
    }
    if (arg.rfind("--emit=", 0) == 0) {
      EmitTarget target;
      std::string emit_error;
      if (!ParseEmitTarget(arg.substr(std::string("--emit=").size()), &target,
                           &emit_error)) {
        if (error) {
          *error = emit_error;
        }
        return false;
      }
      options->emits.push_back(target);
      continue;
    }
    if (arg.rfind("--lint-m-width=", 0) == 0) {
      std::string value = arg.substr(std::string("--lint-m-width=").size());
      if (value == "16") {
        options->lint_m_width_bytes = 2;
      } else {
        options->lint_m_width_bytes = 1;
      }
      continue;
    }
    if (arg.rfind("--lint-x-width=", 0) == 0) {
      std::string value = arg.substr(std::string("--lint-x-width=").size());
      if (value == "16") {
        options->lint_x_width_bytes = 2;
      } else {
        options->lint_x_width_bytes = 1;
      }
      continue;
    }
    if (arg == "--lint-no-unknown-width") {
      options->lint_warn_unknown_width = false;
      continue;
    }
    if (arg == "--lint-no-branch") {
      options->lint_warn_branch_outside_bank = false;
      continue;
    }
    if (arg == "--lint-no-org") {
      options->lint_warn_org_collision = false;
      continue;
    }
    if (arg.rfind("-I", 0) == 0) {
      options->include_paths.push_back(arg.substr(2));
      continue;
    }
    if (arg == "--include") {
      if (i + 1 >= argc) {
        if (error) {
          *error = "--include requires a path";
        }
        return false;
      }
      options->include_paths.push_back(argv[++i]);
      continue;
    }
    if (arg.rfind("-D", 0) == 0) {
      auto def = ParseDefine(arg.substr(2));
      if (def.has_value()) {
        options->defines.push_back(*def);
      }
      continue;
    }
    if (arg == "--define") {
      if (i + 1 >= argc) {
        if (error) {
          *error = "--define requires a value";
        }
        return false;
      }
      auto def = ParseDefine(argv[++i]);
      if (def.has_value()) {
        options->defines.push_back(*def);
      }
      continue;
    }

    if (!arg.empty() && arg[0] == '-') {
      if (error) {
        *error = "Unknown option: " + arg;
      }
      return false;
    }

    if (options->asm_path.empty()) {
      options->asm_path = arg;
    } else if (options->rom_path.empty()) {
      options->rom_path = arg;
    } else {
      if (error) {
        *error = "Too many positional arguments";
      }
      return false;
    }
  }

  return true;
}

bool ReadFile(const fs::path& path, std::vector<uint8_t>* data,
              std::string* error) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    if (error) {
      *error = "Unable to read file: " + path.string();
    }
    return false;
  }
  data->assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  return true;
}

bool WriteBinaryFile(const fs::path& path, const std::vector<uint8_t>& data,
                     std::string* error) {
  std::ofstream file(path, std::ios::binary);
  if (!file.is_open()) {
    if (error) {
      *error = "Unable to write file: " + path.string();
    }
    return false;
  }
  file.write(reinterpret_cast<const char*>(data.data()),
             static_cast<std::streamsize>(data.size()));
  if (!file.good()) {
    if (error) {
      *error = "Failed to write file: " + path.string();
    }
    return false;
  }
  return true;
}

std::string DefaultSymbolsPath(const CliOptions& options) {
  if (!options.symbols_path.empty()) {
    return options.symbols_path;
  }
  if (!options.rom_path.empty()) {
    fs::path path(options.rom_path);
    return path.replace_extension(".sym").string();
  }
  fs::path path(options.asm_path);
  return path.replace_extension(".sym").string();
}

std::vector<std::pair<std::string, std::string>> MergeDefines(
    const std::vector<std::string>& config_defs,
    const std::vector<std::pair<std::string, std::string>>& cli_defs) {
  std::vector<std::pair<std::string, std::string>> out;
  out.reserve(config_defs.size() + cli_defs.size());
  for (const auto& def : config_defs) {
    auto parsed = ParseDefine(def);
    if (parsed.has_value()) {
      out.push_back(*parsed);
    }
  }
  out.insert(out.end(), cli_defs.begin(), cli_defs.end());
  return out;
}

std::vector<std::string> ResolveIncludePaths(
    const std::vector<std::string>& paths, const fs::path& base_dir) {
  std::vector<std::string> out;
  out.reserve(paths.size());
  for (const auto& path : paths) {
    fs::path resolved = path;
    if (!resolved.is_absolute() && !base_dir.empty()) {
      resolved = base_dir / resolved;
    }
    out.push_back(resolved.lexically_normal().string());
  }
  return out;
}

std::string ResolveConfigPath(const std::string& path, const fs::path& base_dir) {
  if (path.empty()) {
    return path;
  }
  fs::path resolved(path);
  if (!resolved.is_absolute() && !base_dir.empty()) {
    resolved = base_dir / resolved;
  }
  return resolved.lexically_normal().string();
}

}  // namespace

int main(int argc, const char* argv[]) {
  CliOptions options;
  std::string error;
  bool interactive_mode = false;
  if (argc == 1) {
    interactive_mode = true;
    std::cout << "Enter patch name: " << std::flush;
    if (!std::getline(std::cin, options.asm_path)) {
      return 1;
    }
    std::cout << "Enter ROM name: " << std::flush;
    if (!std::getline(std::cin, options.rom_path)) {
      return 1;
    }
  } else {
    if (!ParseArgs(argc, argv, &options, &error)) {
      std::cerr << error << "\n";
      PrintUsage(argv[0]);
      return 1;
    }

    if (options.show_help) {
      PrintUsage(argv[0]);
      return 0;
    }

    if (options.show_version) {
      std::cout << "z3asm (Z3DK)\n";
      return 0;
    }
  }

  if (options.asm_path.empty()) {
    std::cerr << "Missing asm_file argument\n";
    PrintUsage(argv[0]);
    return 1;
  }
  if (!options.symbols_format.empty() && options.symbols_format != "none" &&
      options.symbols_format != "wla" && options.symbols_format != "nocash") {
    std::cerr << "Unsupported symbols format: " << options.symbols_format << "\n";
    return 1;
  }

  fs::path asm_path = fs::absolute(options.asm_path);
  if (!fs::exists(asm_path)) {
    std::cerr << "ASM file not found: " << asm_path.string() << "\n";
    return 1;
  }
  fs::path asm_dir = asm_path.parent_path();

  std::string config_path = options.config_path;
  if (config_path.empty()) {
    fs::path candidate = asm_dir / "z3dk.toml";
    if (fs::exists(candidate)) {
      config_path = candidate.string();
    } else if (fs::exists("z3dk.toml")) {
      config_path = fs::absolute("z3dk.toml").string();
    }
  }

  z3dk::Config config;
  if (!config_path.empty()) {
    std::string config_error;
    config = z3dk::LoadConfigFile(config_path, &config_error);
    if (!config_error.empty()) {
      std::cerr << config_error << "\n";
      return 1;
    }
  }

  fs::path config_dir;
  if (!config_path.empty()) {
    config_dir = fs::path(config_path).parent_path();
  }

  if (options.symbols_format.empty() && config.symbols_format.has_value()) {
    options.symbols_format = *config.symbols_format;
  }
  if (options.symbols_path.empty() && config.symbols_path.has_value()) {
    options.symbols_path = ResolveConfigPath(*config.symbols_path, config_dir);
  }
  if (!options.symbols_format.empty() && options.symbols_format != "none" &&
      options.symbols_format != "wla" && options.symbols_format != "nocash") {
    std::cerr << "Unsupported symbols format: " << options.symbols_format << "\n";
    return 1;
  }

  if (!config.emits.empty()) {
    for (const auto& emit_value : config.emits) {
      EmitTarget target;
      std::string emit_error;
      if (!ParseEmitTarget(emit_value, &target, &emit_error)) {
        std::cerr << emit_error << "\n";
        return 1;
      }
      if (!config_dir.empty()) {
        fs::path emit_path(target.path);
        if (!emit_path.is_absolute()) {
          target.path = (config_dir / emit_path).lexically_normal().string();
        }
      }
      options.emits.push_back(target);
    }
  }

  std::vector<std::string> include_paths =
      ResolveIncludePaths(config.include_paths, config_dir);
  include_paths.push_back(asm_dir.string());
  std::vector<std::string> cli_include_paths =
      ResolveIncludePaths(options.include_paths, fs::current_path());
  include_paths.insert(include_paths.end(), cli_include_paths.begin(),
                       cli_include_paths.end());

  std::vector<std::pair<std::string, std::string>> defines =
      MergeDefines(config.defines, options.defines);
  if (config.mapper.has_value()) {
    defines.emplace_back("z3dk_mapper", *config.mapper);
  }

  std::vector<uint8_t> rom_data;
  if (!options.rom_path.empty()) {
    if (fs::exists(options.rom_path)) {
      if (!ReadFile(options.rom_path, &rom_data, &error)) {
        std::cerr << error << "\n";
        return 1;
      }
    }
  } else if (config.rom_size.has_value()) {
    rom_data.resize(static_cast<size_t>(*config.rom_size), 0);
  }

  z3dk::AssembleOptions assemble_options;
  assemble_options.patch_path = asm_path.string();
  assemble_options.rom_data = std::move(rom_data);
  assemble_options.include_paths = std::move(include_paths);
  assemble_options.defines = std::move(defines);
  fs::path exe_dir = fs::absolute(argv[0]).parent_path();
  fs::path default_std_includes = exe_dir / "stdincludes.txt";
  if (fs::exists(default_std_includes)) {
    assemble_options.std_includes_path = default_std_includes.string();
  }
  fs::path default_std_defines = exe_dir / "stddefines.txt";
  if (fs::exists(default_std_defines)) {
    assemble_options.std_defines_path = default_std_defines.string();
  }
  if (config.std_includes_path.has_value()) {
    fs::path std_includes = *config.std_includes_path;
    if (!std_includes.is_absolute() && !config_dir.empty()) {
      std_includes = config_dir / std_includes;
    }
    assemble_options.std_includes_path = std_includes.lexically_normal().string();
  }
  if (config.std_defines_path.has_value()) {
    fs::path std_defines = *config.std_defines_path;
    if (!std_defines.is_absolute() && !config_dir.empty()) {
      std_defines = config_dir / std_defines;
    }
    assemble_options.std_defines_path = std_defines.lexically_normal().string();
  }
  assemble_options.capture_nocash_symbols =
      options.symbols_format == "nocash";

  z3dk::Assembler assembler;
  z3dk::AssembleResult result = assembler.Assemble(assemble_options);

  for (const auto& diag : result.diagnostics) {
    if (!diag.raw.empty()) {
      std::cerr << diag.raw << "\n";
      continue;
    }
    const char* level = diag.severity == z3dk::DiagnosticSeverity::kError
                            ? "error"
                            : "warning";
    std::cerr << diag.filename;
    if (diag.line > 0) {
      std::cerr << ":" << diag.line;
    }
    std::cerr << ": " << level << ": " << diag.message << "\n";
  }

  for (const auto& print : result.prints) {
    std::cout << print << "\n";
  }

  if (result.success) {
    if (!options.rom_path.empty()) {
      if (!WriteBinaryFile(options.rom_path, result.rom_data, &error)) {
        std::cerr << error << "\n";
        return 1;
      }
    }

    if (!options.symbols_format.empty() && options.symbols_format != "none") {
      std::string sym_path = DefaultSymbolsPath(options);
      std::string symbols;
      if (options.symbols_format == "wla") {
        symbols = result.wla_symbols;
      } else if (options.symbols_format == "nocash") {
        symbols = result.nocash_symbols;
      }
      if (!symbols.empty()) {
        if (!z3dk::WriteTextFile(sym_path, symbols, &error)) {
          std::cerr << error << "\n";
          return 1;
        }
      } else {
        std::cerr << "No symbols generated.\n";
      }
    }
  }

  std::optional<z3dk::LintResult> lint_result;
  auto should_emit = [&](EmitTarget::Kind kind) {
    if (result.success) {
      return true;
    }
    return kind == EmitTarget::Kind::kDiagnostics ||
           kind == EmitTarget::Kind::kLint ||
           kind == EmitTarget::Kind::kHooks;
  };

  for (const auto& emit : options.emits) {
    if (!should_emit(emit.kind)) {
      continue;
    }
    std::string contents;
    switch (emit.kind) {
      case EmitTarget::Kind::kDiagnostics:
        contents = z3dk::DiagnosticsToJson(result);
        break;
      case EmitTarget::Kind::kSourceMap:
        contents = z3dk::SourceMapToJson(result.source_map);
        break;
      case EmitTarget::Kind::kSymbolsWla:
        contents = result.wla_symbols;
        break;
      case EmitTarget::Kind::kSymbolsMlb:
        contents = z3dk::SymbolsToMlb(result.labels);
        break;
      case EmitTarget::Kind::kLint: {
        if (!lint_result.has_value()) {
          z3dk::LintOptions lint_options;
          lint_options.default_m_width_bytes = options.lint_m_width_bytes;
          lint_options.default_x_width_bytes = options.lint_x_width_bytes;
          lint_options.warn_unknown_width = options.lint_warn_unknown_width;
          lint_options.warn_branch_outside_bank =
              options.lint_warn_branch_outside_bank;
          lint_options.warn_org_collision = options.lint_warn_org_collision;
          lint_result = z3dk::RunLint(result, lint_options);
        }
        bool lint_success = lint_result->success() && result.success;
        contents = z3dk::DiagnosticsListToJson(lint_result->diagnostics,
                                               lint_success);
        break;
      }
      case EmitTarget::Kind::kHooks:
        contents = z3dk::HooksToJson(result, options.rom_path);
        break;
    }
    if (!z3dk::WriteTextFile(emit.path, contents, &error)) {
      std::cerr << error << "\n";
      return 1;
    }
  }

  if (interactive_mode) {
    std::cout << "Assembling completed without problems.\n";
  }

  return result.success ? 0 : 1;
}
