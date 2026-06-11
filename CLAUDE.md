# CLAUDE.md

Purpose: Claude-specific routing for z3dk.

## Skills

Use `/skill-name` to invoke domain knowledge:

| Task | Skill |
|------|-------|
| z3asm assembler pipeline, opcodes | `/z3dk-toolchain` |
| z3lsp handlers, hover, completion | `/z3dk-toolchain` |
| Linter rules, diagnostics | `/z3dk-toolchain` |
| Mesen socket protocol | `/z3dk-toolchain` |
| SNES knowledge base, registers | `/z3dk-toolchain` |
| z3dk.toml config format | `/z3dk-toolchain` |
| ASM code quality, anti-patterns | `/oos-asm-quality` |
| z3disasm disassembler | `/z3dk-toolchain` |
| ALTTP address/label lookup | `/alttp-disasm-labels` |

### Quick Reference
- **Build**: `cmake --build build -j8`
- **Test**: `./tests/scripts/run_tests.sh` or `pytest -q tests/z3dk`
- **New z3asm source**: add to `ASAR_SHARED_SOURCE_FILES` in `src/z3asm/CMakeLists.txt`
- **New core source**: add to `z3dk-core` lib in `src/z3dk_core/CMakeLists.txt`
- **New LSP source**: add to `z3lsp-lib` in `src/z3lsp/CMakeLists.txt`

## Rules
1. Follow local `AGENTS.md` first when both exist.
2. Keep edits minimal, reversible, and task-scoped.
3. Prefer existing scripts/tools over ad-hoc commands.
4. Validate with build + tests when possible.
5. Never claim verification that was not actually run.
6. Escalate ambiguity or conflicting requirements quickly.

## Response Contract
- What changed
- How it was validated
- Remaining risks or unknowns
