# Agentic Profile & Context Strategy

_Extends: [unified_agent_protocol.md](file:///Users/scawful/.context/memory/unified_agent_protocol.md)_

For all AI agents working on **z3dk** (~/src/hobby/z3dk):

1.  **Read First:** Check `GEMINI.md` for build and test instructions.
2.  **Context:** Use the `.context/` folder structure.
3.  **Build:** Use `run_tests.sh` to verify disassembly integrity.
4.  **LSP:** key tool: `z3lsp`. Supports Rename, Jump to Def, and error suppression for includes.
5.  **Purpose:** This repo is used for Zelda3 Disassembly and Development Kit tools.

## Static Analysis

### oracle_analyzer.py

Oracle-specific static analysis extending `static_analyzer.py`. Key features:
- **KNOWN_HOOKS**: Critical 65816 routines with expected M/X flag state at entry.
  - Includes `JumpTableLocal` ($008781) which requires X=16-bit for correct PLY of JSL return address.
- **`--check-hooks`**: Validates register state at hook entry points against expectations.
- **`--find-mx`**: Scans all JSL call sites for M/X flag mismatches between caller and callee.

### Python Tests

```bash
# M/X flag analysis tests (no ROM required)
pytest tests/test_mx_flag_analysis.py -v

# Label/struct generation tests
pytest tests/test_label_structs.py -v

# LSP integration tests
pytest tests/test_z3lsp_rules.py -v
```
