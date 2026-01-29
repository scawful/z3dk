#include "symbols.h"
#include "utils.h"
#include <fstream>
#include <sstream>
#include <algorithm>

namespace z3disasm {

void AddLabel(LabelIndex* index, uint32_t address, std::string label) {
  if (label.empty()) {
    return;
  }
  index->labels[address].push_back(std::move(label));
}

bool LoadSymbolsMlb(const std::filesystem::path& path, LabelIndex* index) {
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
    if (type != "SnesPrgRom" && type != "PRG" && type != "SnesWorkRam" &&
        type != "SnesSaveRam") {
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

bool LoadSymbolsSym(const std::filesystem::path& path, LabelIndex* index) {
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

bool LoadLabelsCsv(const std::filesystem::path& path, LabelIndex* index) {
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

bool LoadSymbols(const std::filesystem::path& path, LabelIndex* index) {
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

}  // namespace z3disasm
