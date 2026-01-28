# GEMINI.md - z3dk Build Instructions

_Extends: ~/AGENTS.md, ~/GEMINI.md_

Build and test instructions for the Zelda3 Development Kit (z3dk).

## Build Commands

This project uses CMake for certain tools but primarily relies on shell scripts for testing and assembly.

### Regression Tests

```bash
cd ~/src/hobby/z3dk
./run_tests.sh
```

```bash
mkdir -p build && cd build
cmake ..

# Build all tools
make

# Build specific tools
make z3asm      # Assembler
make z3disasm   # Disassembler
make z3lsp      # Language Server
```

### LSP Support

To use the Language Server, ensure `build/bin/z3lsp` is built. It supports standard LSP features (Diagnostics, Go to Definition, Rename).

## Documentation

- **README**: Full usage details are in [README.md](README.md).
- **Context**: Project-specific knowledge is in `.context/`.
