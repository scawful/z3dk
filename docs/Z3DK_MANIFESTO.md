# Z3DK: Zelda 3 Development Kit

**The "LLVM" of SNES Hacking.**

Z3DK is a modernization of the `zAsar` assembler, reimagined as a modular toolchain for professional-grade SNES development. It is designed to power the next generation of Zelda 3 hacks, randomizers, and decompilations.

## The Architecture

Z3DK decomposes the monolithic assembler into three reusable components:

### 1. z3asm (The Assembler)
*   **Role:** High-performance 65816 assembler.
*   **Based on:** zAsar (which is based on Asar).
*   **Changes:**
    *   Decoupled from file I/O (can assemble from memory).
    *   Structured error reporting (JSON/Protobuf output).
    *   Source map generation for source-level debugging.

### 1.1. libz3dk-core (The Engine)
*   **Role:** Shared C++ API for z3asm, z3lsp, and tools.
*   **Capabilities:**
    *   Session-based assembly (in-memory + filesystem-backed).
    *   Structured diagnostics, labels, defines, written blocks, and mapper data.
    *   Source map extraction from WLA-style addr-to-line mapping.
    *   Deterministic JSON emitters for toolchains and editors.

### 2. z3lsp (The Language Server)
*   **Role:** Provides IDE intelligence (VS Code, Neovim).
*   **Features:**
    *   **Go to Definition:** Jump to labels, defines, and macros.
    *   **Hover:** See memory values, instruction cycle counts, and macro expansions.
    *   **Diagnostics:** Real-time error checking without building.
    *   **Completion:** Suggest valid opcodes and labels.

### 3. z3link (The Linker/Symbols)
*   **Role:** Manages the global namespace and ROM layout.
*   **Features:**
    *   **Symbol Map:** Reads/Writes `.sym`, `.mlb` (Mesen), and debug symbols.
    *   **Relocation:** Handles free space logic and bank management.

## Roadmap

### Phase 1: Foundation
- [ ] Introduce `libz3dk-core` C++ API (session + in-memory assembly).
- [ ] Add `z3dk.toml` config (includes/defines/mapper/ROM sizing).
- [ ] Emit structured outputs (diagnostics.json, sourcemap.json, symbols.*).
- [ ] Refactor `z3asm` CLI to route through `libz3dk-core`.

### Phase 2: LSP + Editors
- [x] Implement `z3lsp` stdio server (initialize, completion, symbols, semantic tokens).
- [x] Provide editor recipes for VS Code, Emacs/Spacemacs, and Neovim.
- [ ] Add source-map aware diagnostics + label navigation.

### Phase 3: Emulator & Tooling Integration
- [ ] Export WLA `.sym` with Asar header for Mesen2 compatibility.
- [ ] Export Mesen `.mlb` labels for debugger symbol import.
- [ ] Wire yaze to `libz3dk` and structured outputs.

### Phase 4: z3link
- [ ] Section layout manifest (bank layout + free space).
- [ ] Relocation model + symbol linking.
