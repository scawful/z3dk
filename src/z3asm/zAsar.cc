#include "zAsar.h"

#include <ctime>
#include <cstdio>

#include "asar.h"
#include "asar_math.h"
#include "assembleblock.h"
#include "interface-shared.h"
#include "macro.h"
#include "platform/file-helpers.h"
#include "virtualfile.h"

// zAsar API for emacs and lsp server
bool zAsar_assemble(const char* asm_path, const char* rom_path) {
  try {
    assemblefile(asm_path);
  } catch (errblock&) {
  }

  return !errored;
}

void zAsar_syntax_check(const char* asm_path, char* output_buffer,
                        size_t buffer_size) {
  try {
    assemblefile(asm_path);
  } catch (errblock&) {
  }

  if (errored) {
    snprintf(output_buffer, buffer_size, "Syntax error");
  } else {
    snprintf(output_buffer, buffer_size, "Syntax OK");
  }
}
