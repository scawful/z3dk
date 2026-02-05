# Z3ASM vs Asar (Differences)

This page summarizes Z3ASM behavior that is not present in stock Asar.

## New capabilities
- **Project config (`z3dk.toml`)** drives includes, emits, and defaults.
- **Structured outputs** (JSON diagnostics, sourcemap, hooks.json).
- **LSP integration** via `z3lsp` (live diagnostics, jump-to-definition).
- **Hook tracking** with explicit metadata fields.
- **Annotation tags** (`@watch`, `@assert`, `@abi`) for tooling.

## Behavioral notes
- Z3ASM preserves Asar syntax; extensions are additive and mostly comment-based.
- Output verbosity can be controlled via z3asm flags and toml settings.

## Compatibility guidance
- Keep extensions comment-only or macro-based for maximum portability.
- Avoid z3asm-only directives if Asar 1.x/2.x builds are required.
