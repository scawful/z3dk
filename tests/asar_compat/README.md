# Asar syntax compatibility tests

Tests that **z3asm** accepts common Asar directives and syntax so patches written for [Asar](https://github.com/RPGHacker/asar) (or older Asar versions) continue to assemble with z3dk.

## Running

From the z3dk repo root:

```bash
# With z3asm on PATH or in build (auto-detected)
pytest tests/asar_compat/test_asar_syntax.py -v

# Explicit z3asm path
Z3ASM=/path/to/z3asm pytest tests/asar_compat/test_asar_syntax.py -v

# Optional: compare z3asm output with upstream Asar (skipped if ASAR_EXE unset)
ASAR_EXE=/path/to/asar pytest tests/asar_compat/test_asar_syntax.py -v
```

z3asm is auto-detected from `build/bin/z3asm` or `build/src/z3asm/bin/z3asm` when run from the repo root.

## Coverage

| Snippet        | Directives / syntax              | Purpose                          |
|----------------|-----------------------------------|----------------------------------|
| org_db.asm     | `lorom`, `org`, `db`             | Basic LoROM + byte output        |
| incsrc_main.asm| `incsrc`                          | File include                     |
| math.asm       | Math in operands `1+1`, `(1+2)*3`| PEMDAS, parentheses              |
| defines.asm    | `!x = $42`, `db !x`              | Define substitution             |
| labels.asm     | `label:`, `.sub:`                | Labels and local labels         |
| dw_dl.asm      | `dw`, `dl`                        | 16- and 24-bit little-endian     |
| pad.asm        | `pad $addr`                       | Fill until address (0-fill)      |

Reference: Asar manual in `docs/newbook/` (math, defines, includes, program-counter, etc.).

## Adding tests

1. Add a new `.asm` file under `snippets/` using only Asar-compatible syntax.
2. Add a row to `CASES` in `test_asar_syntax.py`: `("file.asm", "expected_hex_first_bytes")`.
3. Expected hex is the first N bytes at PC 0 (LoROM $8000) after assembly; use space-separated for readability.

## Versions

- **z3asm** is a hard fork of Asar (v1.81+). These tests lock in behavior for common syntax.
- **Upstream Asar** (e.g. 1.91): set `ASAR_EXE` to compare ROM output; tests are skipped if not set.
- Differences (e.g. `pad` end boundary) are documented in test comments or this README.
