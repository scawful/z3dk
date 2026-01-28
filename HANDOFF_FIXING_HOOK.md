# Handoff: Fixing the HOOK Directive and ALTTP Features

## Current Status
- **Build Status**: `z3asm`, `z3disasm`, and `z3lsp` build successfully.
- **Test Status**: 108/117 tests passing.
- **Primary Failure**: `tests/z3dk_alttp_features.asm`
  - `error: (Ebroken_macro_declaration): Broken macro declaration.`
  - `error: (Ehook_too_few_args): Too few args passed to hook.`

## Problem Description
The `HOOK` directive is implemented as a "pseudo-directive" that leverages Asar's internal macro system to capture assembly blocks.

1.  **Capturing**: In `src/z3asm/main.cpp`, when `hook <addr>, <type>` is encountered, it calls `startmacro("___z3dk_hook_at_XXXXXX")`.
2.  **Ending**: When `endhook` is encountered, it calls `endmacro(true)` and then `assembleblock("hook_internal ___z3dk_hook_at_XXXXXX, XXXXXX, type")`.
3.  **Patching**: In `src/z3asm/assembleblock.cpp`, `hook_internal` is supposed to:
    - Enter `freecode`.
    - Define a local label for the implementation.
    - Call the captured macro.
    - Insert a return (`RTL`/`RTS`) if applicable.
    - Patch the `target_addr` with a jump (`JSL`/`JML`/etc.) to the implementation.

### Issues Identified
- **`Ebroken_macro_declaration`**: Asar's macro system expects a specific signature. `startmacro` in `main.cpp` might be passing a name that doesn't look like a valid macro declaration (e.g., missing parentheses or having invalid characters).
- **Argument Parsing**: `hook_internal` in `assembleblock.cpp` has been through multiple iterations using `qsplit` and `strip_whitespace`. It currently expects `par` to contain `macro_name, address, type`.
- **Include Paths**: `src/stdlib/alttp/all.asm` was temporarily patched with absolute paths to resolve recursion issues during testing. This should be made portable (relative) once the core logic works.

## Recommendations for Next Agent
1.  **Investigate `startmacro`**: Check `src/z3asm/macro.cpp` or the Asar documentation. Does the name passed to `startmacro` need to be `name()`?
    - Try changing `startmacro(macro_name)` to `startmacro(string(macro_name) + "()")`.
2.  **Debug `hook_internal` Parsing**: Use `printf` debugging in `assembleblock.cpp` to see exactly what `num_params` and `params` contain when `hook_internal` is called.
3.  **Fix `Ehook_too_few_args`**: This error often occurs when `assembleblock` is called with a string that it then fails to split correctly into the expected `word` array.
4.  **Standard Library**: Once `HOOK` works, revert the absolute paths in `src/stdlib/alttp/all.asm` and ensure the test runner passes the correct include directories via `-I`.

## Relevant Files
- `src/z3asm/main.cpp`: Line logic for `hook`/`endhook`.
- `src/z3asm/assembleblock.cpp`: Patching logic for `hook_internal`.
- `src/z3asm/libstr.cpp`: String utilities (I added `strip_suffix` and fixed `char_props`).
- `tests/z3dk_alttp_features.asm`: The test case to fix.
