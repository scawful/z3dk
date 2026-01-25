#pragma once

// zAsar LSP server API

#include <cstddef>
#include <cstdint>

extern "C" {
  bool zAsar_assemble(const char* asm_path, const char *rom_path);
  void zAsar_syntax_check(const char* asm_path, char* output_buffer, size_t buffer_size);
}
