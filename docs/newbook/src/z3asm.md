# Z3ASM Overview

Z3ASM is the Z3DK fork of Asar that adds project-aware configuration, structured
outputs, and IDE support while staying source-compatible with standard Asar.

## Highlights
- **Structured outputs:** JSON diagnostics, sourcemaps, hooks.json, symbols.
- **Project config:** `z3dk.toml` defines includes, emits, and defaults.
- **IDE integration:** `z3lsp` consumes structured diagnostics and source maps.
- **Hook metadata:** hooks.json enables disassembly and ABI analysis.
- **Annotations (comment-only):** `@watch`, `@assert`, `@abi` tags are parseable.

## Example: z3dk.toml
```toml
preset = "alttp"
mapper = "lorom"
rom_size = 0x200000
include_paths = ["."]
main = "Main.asm"

emit = [
  "diagnostics.json",
  "sourcemap.json",
  "hooks.json",
  "symbols.mlb",
  "annotations.json",
]
```

## Example: invoking z3asm
```bash
z3asm Main.asm game.sfc --emit=hooks.json --emit=annotations.json --emit=sourcemap.json
```

## Comment tags (Asar-safe)
These are ignored by Asar and can be interpreted by z3asm tools:
```
; @hook name=Overworld_SetCameraBounds kind=jsl target=NewOverworld_SetCameraBounds expected_m=16 expected_x=8
; @watch fmt=hex
; @assert MODE == $07
```

See also:
- **Differences vs Asar** (`z3asm-differences.md`)
- **Asar 2.0 compatibility** (`z3asm-compat.md`)
