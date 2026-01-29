# Z3DK Hook Manifest (hooks.json)

This file defines a lightweight format for describing ROM hooks so that tools
like `z3disasm` can annotate patched regions during disassembly.

## Generating Hooks

`z3asm` can emit a `hooks.json` file alongside other structured outputs:

```bash
z3asm my_patch.asm rom.sfc --emit=hooks.json
```

The emitted hooks are derived from the assembled write blocks with the best
available label + source map context.

## Format

```json
{
  "version": 1,
  "rom": {
    "path": "Roms/oos168x.sfc",
    "sha1": "optional"
  },
  "hooks": [
    {
      "name": "Overworld_SetCameraBounds",
      "address": "0x02C0C3",
      "size": 4,
      "kind": "jsl",
      "target": "NewOverworld_SetCameraBounds",
      "source": "Core/Overworld/Camera.asm:42",
      "note": "Replaces vanilla bounds logic"
    }
  ]
}
```

## Field Reference

- `version` (number): Manifest schema version (currently `1`).
- `rom.path` (string, optional): ROM filename for context.
- `rom.sha1` (string, optional): ROM hash for verification.
- `hooks` (array): List of hook entries.

Hook entry fields:

- `name` (string): Human-readable hook name.
- `address` (string|number): SNES address of the hook (24-bit hex).
- `size` (number, optional): Byte length of the hook region.
- `kind` (string, optional): Hook type (e.g., `jsl`, `jml`, `patch`).
- `target` (string, optional): Target routine/label name.
- `source` (string, optional): Source file and line for the hook.
- `note` (string, optional): Extra context for tooling.
- `expected_m` (number, optional): Expected M flag width at hook entry (8 or 16).
- `expected_x` (number, optional): Expected X flag width at hook entry (8 or 16).
- `expected_exit_m` (number, optional): Expected M width on routine exit (8 or 16).
- `expected_exit_x` (number, optional): Expected X width on routine exit (8 or 16).
- `module` (string, optional): High-level module name (derived from file path).
- `skip_abi` (bool, optional): Mark entry as data/placeholder or non-returning hook (e.g., `data`, `jmp`, `jml`, or anonymous `hook_XXXXXX`) to skip ABI checks.
- `abi_class` (string, optional): ABI convention hint (e.g., `long_entry` to signal the callee normalizes M/X internally).

## Annotations (Oracle generator)
The Oracle hooks generator can read inline annotations:
- `; @abi long_entry`
- `; @abi m8x8` / `; @abi m16x8` / `; @abi m8x16` / `; @abi m16x16`
- `; @no_return`

These are emitted into `hooks.json` as `abi_class`, `expected_m`, `expected_x`, or `skip_abi`.

Tools may use `expected_m` / `expected_x` to warn about register-width drift
when hooks return to vanilla code paths.

Tools should ignore unknown fields to allow extension.
