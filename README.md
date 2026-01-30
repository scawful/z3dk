# Z3DK (Zelda 3 Development Kit)

Z3DK is a modern toolchain for SNES development, specializing in *The Legend of Zelda: A Link to the Past* (Zelda 3) ROM hacking. It is a hard fork of [Asar](https://github.com/RPGHacker/asar) (v1.81+) that adds structured output, a language server, and deep integration for modern IDEs.

**Status:** Active Development (Alpha)

## Core Components

| Tool | Description |
| :--- | :--- |
| **z3asm** | Enhanced Asar assembler with TOML config, JSON diagnostics, and source maps. |
| **z3disasm** | Smart ROM disassembler that uses symbols to generate re-assemblable `bank_XX.asm` files. |
| **z3lsp** | Language Server Protocol implementation for VS Code, Neovim, and Emacs. |
| **libz3dk** | C++ API for embedding the assembler/disassembler into other tools (like [YAZE](../yaze)). |

## Quick Start

### 1. Installation

**Build from Source (macOS/Linux):**
```bash
git clone https://github.com/scawful/z3dk.git
cd z3dk
mkdir build && cd build
cmake ..
make
```

**Executables:**
- `build/bin/z3asm`
- `build/bin/z3disasm`
- `build/bin/z3lsp`

### 2. Project Setup (`z3dk.toml`)
Create a `z3dk.toml` in your project root to configure the assembler. This replaces the need for complex CLI flags.

```toml
# z3dk.toml
preset = "alttp"         # Sets sane defaults for Zelda 3
mapper = "lorom"         # Memory mapping
rom_size = 0x200000      # 2MB
include_paths = ["src", "include"]
symbols = "wla"          # Generates .sym file
warn_unused_symbols = true
prohibited_memory_ranges = [
  "$7E0000-$7E01FF: SRAM scratchpad"
]
lsp_log_enabled = true
lsp_log_path = "z3lsp.log"

# Define compilation targets
emit = [
  "diagnostics.json",    # For VS Code problem matchers
  "symbols.mlb",         # For Mesen2 debugging
  "hooks.json"           # For Z3DK hook tracking
]
```

`prohibited_memory_ranges` accepts inclusive SNES address ranges. You can use `$` or `0x` prefixes and add an
optional reason after `:` (used in diagnostics). `lsp_log_enabled` toggles z3lsp JSON/error logging, and
`lsp_log_path` overrides the default temp log location (relative paths resolve to the config directory).

**Main file discovery:** If you do not create a `z3dk.toml`, the LSP still picks a main candidate by convention:
any root-level file named `Main.asm`, `*_main.asm`, or `*-main.asm` (e.g. `Oracle_main.asm`, `Meadow_main.asm`).
For single-entry projects, a minimal `z3dk.toml` with `main = "Main.asm"` and `include_paths = ["."]` is enough.

**Include paths:** When all `incsrc` paths are relative to the project root (or the main file’s directory),
`include_paths = ["."]` is sufficient. Subdirs like `Music/`, `Engine/`, `Sprites/` are then found from the root.

### 3. Project layout examples

- **Oracle of Secrets:** `z3dk.toml` with `main = "Oracle_main.asm"`, `include_paths = [".", "Core", "Sprites", ...]`.
  Entry point and modules live under named dirs; LSP and assembler use the configured main.
- **Poltergeist (AllHallows Eve):** Flat root with `Main.asm` and `include_paths = ["."]`. All `incsrc` paths
  (e.g. `Music/...`, `Engine/...`) are relative to the root; a minimal `z3dk.toml` with `main = "Main.asm"`
  and `include_paths = ["."]` is enough for LSP and assembly.

### 4. Usage

**Assembly:**
```bash
# Uses settings from z3dk.toml if present
z3asm src/main.asm game.sfc
```
CLI output is quiet by default. Pass `--summary` to show the result/summary line items, or `--no-summary` to force them off.

**Disassembly:**
```bash
# Disassemble bank $02 with symbols
z3disasm --rom game.sfc --symbols game.mlb --bank-start 02 --bank-end 02 --out src/
```

**IDE Integration:**
- **VS Code:** Install the extension in `extensions/vscode-z3dk`.
- **Neovim:** Configure `lspconfig` to use `z3lsp`.

## Features over Vanilla Asar

1.  **Structured Output:** Emits JSON for errors, warnings, and labels. No more parsing stdout.
2.  **Mesen2 Integration:** Native support for `.mlb` symbol files for source-level debugging in Mesen2.
3.  **Project-Aware:** `z3dk.toml` allows defining the project environment (defines, includes) in one place.
4.  **Language Server:** `z3lsp` provides Go-to-Definition, Hover info, and real-time diagnostics.
5.  **Hook Management:** Explicit support for defining and tracking hooks (hijacks) in the ROM.

## License

Z3DK is open-source software.
- Original Asar code is licensed under the GPL/LGPL/WTFPL (see `docs/legacy/`).
- New Z3DK components are licensed under the MIT License.

## Credits

- **Asar Team:** Alcaro, RPGHacker, randomdude999, and contributors for the rock-solid base.
- **Scawful:** Z3DK fork maintainer.
