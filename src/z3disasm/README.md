# z3disasm - Zelda 3 Smart Disassembler

This directory contains the implementation of the smart disassembler for the Zelda 3 Development Kit (z3dk).

## Modular Architecture

The disassembler has been refactored into several modules to improve maintainability:

- **`main.cc`**: Application entry point, bank iteration, and high-level disassembly logic.
- **`utils`**: Lower-level helper functions for string parsing (hex/int), path operations, and SNES address conversion.
- **`options`**: Command-line argument parsing and configuration management.
- **`symbols`**: Symbol and label indexing/management, supporting `.mlb`, `.sym`, and `.csv` formats.
- **`hooks`**: Hook manifest processing for identifying and documenting routine hijacks.
- **`formatter`**: Low-level instruction formatting and operand resolution using the symbol index.

## Build Information

z3disasm is built as part of the z3dk project. It depends on `libz3dk` for opcode metadata and memory mapping details.

To build specifically:
```bash
cmake --build build --target z3disasm
```

## Features

- **Bank-Level Extraction**: Splits ROM into re-assemblable `bank_XX.asm` files.
- **Symbol Integration**: Automatically replaces addresses with labels from provided symbol maps.
- **Automatic Flag Inference**: Inferred `M/X` register widths via `REP`, `SEP`, and `XCE` instructions to ensure correct operand sizing.
- **Hook Annotations**: Integrates with `hooks.json` to annotate known modification points.
