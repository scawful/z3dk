# Z3DK VS Code Extension

Z3DK provides Asar syntax highlighting, z3lsp language features, and Zelda 3 tooling helpers.

## Features
- Asar/65816/SPC700/SuperFX syntax highlighting
- z3lsp LSP integration (completion, symbols, semantic tokens)
- Commands for build/test and symbol export
- Z3DK Activity Bar view with quick actions
- Z3DK submenu in the View menu

## Quick Start (repo-local)
1. Build z3lsp:
   ```bash
   cd ~/src/hobby/z3dk
   mkdir -p build && cd build
   cmake ..
   cmake --build .
   ```
2. Install dependencies for the extension:
   ```bash
   cd ~/src/hobby/z3dk/extensions/vscode-z3dk
   npm install
   ```
3. Launch VS Code with the extension:
   ```bash
   code --extensionDevelopmentPath=~/src/hobby/z3dk/extensions/vscode-z3dk
   ```

## Settings
- `z3dk.serverPath`: path to `z3lsp` (auto-detects build outputs or PATH).
- `z3dk.serverArgs`: extra args for z3lsp.
- `z3dk.buildCommand`: command for building z3dk.
- `z3dk.testCommand`: command for running tests.
- `z3dk.romPath`: ROM path used for symbol export.
- `z3dk.yazePath`: yaze binary path.
- `z3dk.symbolFormat`: symbol format (`mesen` by default).
- `z3dk.symbolsPath`: override output symbol path.

## Commands
- `Z3DK: Run Tests`
- `Z3DK: Build`
- `Z3DK: Export Mesen Symbols (yaze)`
- `Z3DK: Restart Language Server`

## Notes
- This extension defaults to Asar semantics (z3asm compatibility) until z3dk stabilizes.
- Symbol export uses yaze to generate `.mlb` for Mesen2 debugging by default.
