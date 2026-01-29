# z3lsp - Zelda 3 Language Server

This directory contains the implementation of the Language Server Protocol (LSP) for the Zelda 3 Development Kit (z3dk).

## Modular Architecture

The language server is split into several modules to improve maintainability and testability:

- **`main.cc`**: Entry point and high-level LSP message routing.
- **`logging`**: Centralized logging system for the language server.
- **`utils`**: General-purpose helper functions for string, path, and shell operations.
- **`state`**: Persistent data structures for document and workspace management.
- **`project_graph`**: Dependency tracking between files to support workspace-wide analysis.
- **`mesen_client`**: Integration with the Mesen2 emulator via Unix sockets for live debugging features.
- **`lsp_transport`**: Low-level JSON-RPC protocol handling.
- **`parser`**: ASM-specific parsing, symbol extraction, and workspace indexing.

## Build Information

z3lsp is built as part of the main z3dk project. It depends on `libz3dk` for core assembler functionality.

To build specifically:
```bash
cmake --build build --target z3lsp
```
