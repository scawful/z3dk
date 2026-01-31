# CLAUDE.md

_Extends: ~/AGENTS.md, ~/CLAUDE.md_

**Z3DK** - Zelda 3 Development Kit. Hard fork of Asar with structured output, LSP, and IDE integration.

## Build & Test

```bash
# Build all tools
cd ~/src/hobby/z3dk
mkdir -p build && cd build && cmake .. && make -j8

# Build specific tools
make z3asm       # 65816/SPC700/SuperFX assembler
make z3disasm    # Disassembler (re-assemblable output)
make z3lsp       # Language Server Protocol server

# Regression tests (assembler)
cd ~/src/hobby/z3dk
./run_tests.sh

# Python tests (no ROM required)
pytest tests/z3dk/test_mx_flag_analysis.py -v    # M/X flag analysis
pytest tests/z3dk/test_label_structs.py -v       # Label generation
pytest tests/z3dk/test_z3lsp_rules.py -v         # LSP rules
pytest tests/asar_compat/test_asar_syntax.py -v  # Asar syntax compat (needs z3asm in build or Z3ASM)
```

## Architecture

| Component | Path | Purpose |
|-----------|------|---------|
| z3asm | `src/z3asm/` | Assembler binary (65816, SPC700, SuperFX) |
| z3disasm | `src/z3disasm/` | Disassembler with symbol-aware output |
| z3lsp | `src/z3lsp/` | Language Server (diagnostics, go-to-def, rename) |
| libz3dk | `src/z3dk_core/` | Shared C++ API (session-based assembly, structured diagnostics) |
| stdlib | `src/stdlib/` | Standard library macros including ALTTP-specific |

## Static Analysis Scripts

Located in `scripts/`, these are used by oracle-of-secrets for build-time validation.

| Script | Purpose |
|--------|---------|
| `static_analyzer.py` | Generic 65816 M/X flag tracking, stack balance analysis |
| `oracle_analyzer.py` | Oracle-specific hooks: KNOWN_HOOKS with expected register state, `--check-hooks`, `--find-mx` |
| `oracle_validate.py` | ROM validation (room headers, object pointers, Tile16 integrity) |
| `generate_label_indexes.py` | Generates searchable label indexes (CSV + JSON) for agent lookup |

### Key Concepts

- **StateTracker**: Traces 65816 control flow tracking M/X flag state through SEP/REP/PHP/PLP
- **KNOWN_HOOKS**: List of routines with expected register widths at entry (e.g., JumpTableLocal at $008781 requires 8-bit Y; 16-bit Y causes stack underflow)
- **find_mx_mismatches**: Scans JSL call sites for M/X flag mismatches between caller and callee
- **Analysis order matters**: When using `find_mx_mismatches`, analyze target hooks BEFORE callers so expected states are recorded before caller traces overwrite them

## Cross-Project Integration

### Oracle of Secrets (`~/src/hobby/oracle-of-secrets/`)

- Primary consumer of z3dk tools
- Build script (`build_rom.sh`) invokes `oracle_analyzer.py --check-hooks --find-mx`
- `hooks.json` defines hook manifest with register expectations
- Symbol files (`.mlb`, `.sym`) generated for Mesen2 debugger

### YAZE (`~/src/hobby/yaze/`)

- `scripts/ai/asm_tuner.py` uses z3asm for syntax validation
- libz3dk planned for embedded assembly in yaze editor

### Mesen2-OOS (`~/src/hobby/mesen2-oos/`)

- z3lsp connects via `MESEN2_SOCKET_PATH` for live debugging
- Emits `.mlb` symbol files consumed by Mesen2 debugger

## Configuration

- `z3dk.toml` - Project root config (presets, mapper, ROM size, includes, symbols, diagnostics)
- `hooks.json` - Hook manifest with M/X flag state expectations
- Symbol formats: `.mlb` (Mesen2), `.sym` (WLA compatible)

**Main file without config:** Projects without `z3dk.toml` still get a main candidate from root-level
`Main.asm`, `*_main.asm`, or `*-main.asm` (see `IsMainFileName` in `z3lsp/utils.cc`). The LSP uses that
when an opened file is an include so assembly runs from the main file.

**Oracle-specific vs generic:** The LSP’s missing-label suppression includes heuristics for `Oracle_`
prefix and suffix-after-underscore (oracle-of-secrets). Other projects (e.g. poltergeist) rely on the
full assembly symbol table once the main file is assembled; no code change is required if `z3dk.toml`
points at the real main file.

## 65816 reference (M/X flags)

- **P register:** Bit 4 (0x10) = X flag (1 = 8-bit index X/Y), bit 5 (0x20) = M flag (1 = 8-bit accumulator). SEP sets bits → 8-bit; REP clears bits → 16-bit. See `~/src/hobby/docs/reference/65816_instruction_set.md` and SNES dev manuals.
- **PLY/PHY:** Size depends on X flag: X=8-bit → 1 byte, X=16-bit → 2 bytes. Mismatched push/pull width causes stack corruption (e.g. JumpTableLocal softlock).

## Pitfalls

1. **Analysis order**: `StateTracker.analyze_from(target)` must run before `analyze_from(caller)` when using `find_mx_mismatches` — caller JSL traces into target and overwrites recorded state
2. **HOOK directive**: Macro parsing for `HOOK` is still experimental; some tests may fail
3. **Build path**: Use `build/` directory, not in-source builds
