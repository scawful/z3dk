#include "z3dk_core/emit.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace z3dk {
namespace {

std::string EscapeJson(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  return out;
}

void AppendDiagnosticJson(const Diagnostic& diag, std::ostringstream* out) {
  *out << "{\"message\":\"" << EscapeJson(diag.message) << "\"";
  if (!diag.filename.empty()) {
    *out << ",\"file\":\"" << EscapeJson(diag.filename) << "\"";
  }
  if (diag.line > 0) {
    *out << ",\"line\":" << diag.line;
  }
  if (diag.column > 0) {
    *out << ",\"column\":" << diag.column;
  }
  if (!diag.raw.empty()) {
    *out << ",\"raw\":\"" << EscapeJson(diag.raw) << "\"";
  }
  *out << "}";
}

}  // namespace

std::string DiagnosticsToJson(const AssembleResult& result) {
  std::ostringstream out;
  out << "{\"version\":1,\"success\":" << (result.success ? "true" : "false");

  out << ",\"errors\":[";
  bool first = true;
  for (const auto& diag : result.diagnostics) {
    if (diag.severity != DiagnosticSeverity::kError) {
      continue;
    }
    if (!first) {
      out << ',';
    }
    AppendDiagnosticJson(diag, &out);
    first = false;
  }
  out << "]";

  out << ",\"warnings\":[";
  first = true;
  for (const auto& diag : result.diagnostics) {
    if (diag.severity != DiagnosticSeverity::kWarning) {
      continue;
    }
    if (!first) {
      out << ',';
    }
    AppendDiagnosticJson(diag, &out);
    first = false;
  }
  out << "]";

  out << "}";
  return out.str();
}

std::string DiagnosticsListToJson(const std::vector<Diagnostic>& diagnostics,
                                  bool success) {
  std::ostringstream out;
  out << "{\"version\":1,\"success\":" << (success ? "true" : "false");

  out << ",\"errors\":[";
  bool first = true;
  for (const auto& diag : diagnostics) {
    if (diag.severity != DiagnosticSeverity::kError) {
      continue;
    }
    if (!first) {
      out << ',';
    }
    AppendDiagnosticJson(diag, &out);
    first = false;
  }
  out << "]";

  out << ",\"warnings\":[";
  first = true;
  for (const auto& diag : diagnostics) {
    if (diag.severity != DiagnosticSeverity::kWarning) {
      continue;
    }
    if (!first) {
      out << ',';
    }
    AppendDiagnosticJson(diag, &out);
    first = false;
  }
  out << "]";

  out << "}";
  return out.str();
}

std::string HooksToJson(const AssembleResult& result,
                        const std::string& rom_path) {
  std::unordered_map<uint32_t, std::string> label_index;
  label_index.reserve(result.labels.size());
  for (const auto& label : result.labels) {
    if (label.name.empty()) {
      continue;
    }
    if (label_index.find(label.address) == label_index.end()) {
      label_index[label.address] = label.name;
    }
  }

  std::unordered_map<int, std::string> file_index;
  for (const auto& file : result.source_map.files) {
    file_index[file.id] = file.path;
  }

  std::vector<SourceMapEntry> entries = result.source_map.entries;
  std::sort(entries.begin(), entries.end(),
            [](const SourceMapEntry& a, const SourceMapEntry& b) {
              if (a.address != b.address) {
                return a.address < b.address;
              }
              return a.line < b.line;
            });

  auto find_source = [&](uint32_t address) -> std::string {
    if (entries.empty()) {
      return std::string();
    }
    auto it = std::upper_bound(entries.begin(), entries.end(), address,
                               [](uint32_t addr, const SourceMapEntry& entry) {
                                 return addr < entry.address;
                               });
    if (it == entries.begin()) {
      return std::string();
    }
    --it;
    auto file_it = file_index.find(it->file_id);
    if (file_it == file_index.end()) {
      return std::string();
    }
    std::ostringstream out;
    out << file_it->second;
    if (it->line > 0) {
      out << ":" << it->line;
    }
    return out.str();
  };

  std::ostringstream out;
  out << "{\"version\":1";
  if (!rom_path.empty()) {
    out << ",\"rom\":{\"path\":\"" << EscapeJson(rom_path) << "\"}";
  }
  out << ",\"hooks\":[";

  bool first = true;
  for (const auto& block : result.written_blocks) {
    if (block.num_bytes <= 0) {
      continue;
    }
    uint32_t address = static_cast<uint32_t>(block.snes_offset);
    uint32_t size = static_cast<uint32_t>(block.num_bytes);
    std::string name;
    auto label_it = label_index.find(address);
    if (label_it != label_index.end()) {
      name = label_it->second;
    }
    std::string source = find_source(address);

    if (!first) {
      out << ',';
    }
    first = false;
    out << "{\"address\":\"0x";
    out << std::hex << std::uppercase;
    out.width(6);
    out.fill('0');
    out << address;
    out << std::dec << "\"";
    out << ",\"size\":" << size;
    out << ",\"kind\":\"patch\"";
    if (!name.empty()) {
      out << ",\"name\":\"" << EscapeJson(name) << "\"";
    }
    if (!source.empty()) {
      out << ",\"source\":\"" << EscapeJson(source) << "\"";
    }
    out << "}";
  }
  out << "]}";
  return out.str();
}

std::string SourceMapToJson(const SourceMap& map) {
  std::ostringstream out;
  out << "{\"version\":1";

  out << ",\"files\":[";
  bool first = true;
  for (const auto& file : map.files) {
    if (!first) {
      out << ',';
    }
    out << "{\"id\":" << file.id
        << ",\"crc\":\"0x";
    out << std::hex << std::uppercase << file.crc << std::dec;
    out << "\",\"path\":\"" << EscapeJson(file.path) << "\"}";
    first = false;
  }
  out << "]";

  out << ",\"entries\":[";
  first = true;
  for (const auto& entry : map.entries) {
    if (!first) {
      out << ',';
    }
    out << "{\"address\":\"0x";
    out << std::hex << std::uppercase << entry.address << std::dec;
    out << "\",\"file_id\":" << entry.file_id
        << ",\"line\":" << entry.line << "}";
    first = false;
  }
  out << "]";

  out << "}";
  return out.str();
}

std::string SymbolsToMlb(const std::vector<Label>& labels) {
  std::vector<Label> sorted = labels;
  std::sort(sorted.begin(), sorted.end(), [](const Label& a, const Label& b) {
    if (a.address != b.address) {
      return a.address < b.address;
    }
    return a.name < b.name;
  });

  std::ostringstream out;
  for (const auto& label : sorted) {
    out << "PRG:" << std::hex << std::uppercase << label.address << std::dec
        << ":" << label.name << "\n";
  }
  return out.str();
}

bool WriteTextFile(const std::string& path, std::string_view contents,
                   std::string* error) {
  std::ofstream file(path, std::ios::binary);
  if (!file.is_open()) {
    if (error) {
      *error = "Unable to write file: " + path;
    }
    return false;
  }
  file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (!file.good()) {
    if (error) {
      *error = "Failed to write file: " + path;
    }
    return false;
  }
  return true;
}

}  // namespace z3dk
