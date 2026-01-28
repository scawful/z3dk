# z3lsp Phase 2: Advanced Features Implementation

This document details the implementation of advanced Language Server Protocol properties for `z3lsp`, enabling a richer development experience for SNES/ASM programming.

## Implemented Features

### 1. Macro Signature Help
**Capability:** `textDocument/signatureHelp`
- **Description:** Provides parameter hints when typing macro calls.
- **Trigger:** Typing `(` or `,` after a macro name.
- **Example:** `MyMacro(` displays `MyMacro(param1, param2)`.
- **Implementation:**
    - Modified `SymbolEntry` to store parameters.
    - Updated `ParseFileText` to extract parameter names from definitions.
    - Added LSP handler to find macro invocations and return signature info.

### 2. Inlay Hints for Addresses
**Capability:** `textDocument/inlayHint`
- **Description:** Displays resolved label names next to raw hexadecimal addresses.
- **Example:** `LDA $7E0010` shows as `LDA $7E0010 <<LinkDirection>>`.
- **Implementation:**
    - Scans document text for hex numbers (`$xxxx`).
    - Resolves addresses using `z3dk`'s assembly result.
    - Returns `InlayHint` objects for matching labels.

### 3. Find All References
**Capability:** `textDocument/references`
- **Description:** Finds all occurrences of a symbol across the entire workspace.
- **Usage:** "Find All References" on any word.
- **Implementation:**
    - Identifies the token at the cursor.
    - Scans all `.asm`, `.s`, `.inc`, `.a` files in the workspace root.
    - Returns matches with location info.

### 4. New Mesen Integration Commands
**Commands:**
- `mesen.stepInstruction`: Steps the emulator forward by one instruction.
- `mesen.showCpuState`: dumps the current CPU state (via `GAMESTATE`) to the output.
- **Usage:** Bind these commands to keys in VS Code `keybindings.json`.

## Verification

Since the build environment has some sandbox constraints, these features are implemented in `src/z3lsp/main.cc`.

### Manual Testing Steps
1.  **Build LSP:** `cmake --build build --target z3lsp` (ensure standard environment).
2.  **Configure VS Code:** Point `z3dk.lspPath` to the new binary.
3.  **Open an ASM File:**
    - Type `MyMacro(`. Verify tooltip.
    - Scroll to code with `$ADDR`. Verify `<<Label>>` hints appear.
    - Right-click a label -> "Find All References". Verify distinct file results.
4.  **Connect Mesen:**
    - Run `mesen-run`.
    - Run "Mesen: Step Instruction" command from Command Palette.
