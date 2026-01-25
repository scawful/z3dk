# Z3DK VS Code Extension

Z3DK provides Asar syntax highlighting, z3lsp language features, and Zelda 3 tooling helpers.

## Features
- Asar/65816/SPC700/SuperFX syntax highlighting
- z3lsp LSP integration (completion, symbols, semantic tokens)
- Commands for build/test and symbol export
- Z3DK Activity Bar view with quick actions
- Dashboard view with integration status (yaze, Mesen2, ROM paths, USDASM, models, Continue)
- Emulator launchers + disassembly lab controls
- Compact status bar indicators (Z3DK, LSP, ROM/Symbols)
- Editor context menu actions + CodeLens when z3lsp is active
- Inline label/org annotations in Asar files (toggleable)
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
- `z3dk.devWorkspacePath`: path to the multi-root dev workspace.
- `z3dk.modelCatalogPath`: path to the Zelda model catalog.
- `z3dk.modelPortfolioPath`: path to the AFS model portfolio.
- `z3dk.continueConfigPath`: path to Continue config.yaml.
- `z3dk.continueConfigTsPath`: path to Continue config.ts.
- `z3dk.oracleRoot`: path to oracle-of-secrets repo.
- `z3dk.yazeRoot`: path to yaze repo.
- `z3dk.mesenRoot`: path to mesen2-oos repo.
- `z3dk.mesenPath`: path to the Mesen2-OOS executable or .app bundle.
- `z3dk.mesenArgs`: launch arguments for Mesen2-OOS (supports `${rom}` + `${symbols}`).
- `z3dk.yazeLaunchArgs`: launch arguments for yaze (supports `${rom}` + `${symbols}`).
- `z3dk.usdasmRoot`: path to the USDASM disassembly.
- `z3dk.usdasmGlob`: glob for USDASM label search.
- `z3dk.disasmCommand`: command to export USDASM-style disassembly (supports `${rom}`, `${symbols}`, `${output}`, `${usdasm}`).
- `z3dk.disasmOutputPath`: output path for disassembly exports.
- `z3dk.enableCodeLens`: show Z3DK CodeLens actions in Asar files.
- `z3dk.editorAnnotations`: show inline label/org tags in Asar files.

## Commands
- `Z3DK: Run Tests`
- `Z3DK: Build`
- `Z3DK: Export Mesen Symbols (yaze)`
- `Z3DK: Restart Language Server`
- `Z3DK: Dashboard`
- `Z3DK: Workspace`
- `Z3DK: Model Catalog`
- `Z3DK: Model Portfolio`
- `Z3DK: Continue YAML`
- `Z3DK: Continue TS`
- `Z3DK: AFS Scratchpad`
- `Z3DK: Add AFS Contexts`
- `Z3DK: Oracle Repo`
- `Z3DK: Yaze Repo`
- `Z3DK: Mesen2 Repo`
- `Z3DK: Mesen2`
- `Z3DK: yaze`
- `Z3DK: ROM Folder`
- `Z3DK: Hack Disasm`
- `Z3DK: USDASM Search`
- `Z3DK: USDASM Root`

## Notes
- This extension defaults to Asar semantics (z3asm compatibility) until z3dk stabilizes.
- Symbol export uses yaze to generate `.mlb` for Mesen2 debugging by default.
- Hack disassembly export will auto-detect `z3disasm` build outputs if `z3dk.disasmCommand` is unset.
