#include "utils.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <iomanip>

namespace z3disasm {

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

bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>* data) {
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

}  // namespace z3disasm
