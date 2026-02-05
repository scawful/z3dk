# Asar 2.0 Compatibility

Z3ASM aims to be source-compatible with Asar while adding tooling features.
To keep code portable across Asar 1.x and 2.0:

## Safe patterns
- **Comment-only tags:** `; @hook`, `; @watch`, `; @assert`, `; @abi`.
- **Macros that expand to Asar:** `%OOS_Hook` -> `org` + comment.
- **Standard Asar syntax** for all functional code paths.

## Avoid (when portability is required)
- New directives or syntax that only z3asm understands.
- Reliance on z3asm-specific emit behavior without fallback tooling.

## Migration strategy
- Keep z3asm features opt-in until Asar 2.0 behavior stabilizes.
- Prefer tool-side parsing of comments until z3asm emits are ubiquitous.
