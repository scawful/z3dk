---
description: Build and test z3dk components
---
# z3dk Development Workflow

This workflow describes how to build and verify the z3dk tools (z3asm, z3disasm, z3lsp).

## Build Instructions

// turbo
1. Build the project using CMake:
   ```bash
   mkdir -p build && cd build
   cmake ..
   make -j$(sysctl -n hw.ncpu)
   ```

## Verification

2. Run the main regression test suite:
   ```bash
   ./run_tests.sh
   ```

3. Test specific ALTTP features in z3asm:
   ```bash
   ./build/src/z3asm/bin/z3asm tests/z3dk_alttp_features.asm dummy.sfc
   ```

## LSP Debugging

4. Run the Language Server with debug logging:
   ```bash
   ./build/bin/z3lsp --log-level=debug
   ```
