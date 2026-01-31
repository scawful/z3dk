#!/usr/bin/env python3
"""Asar syntax compatibility tests for z3asm.

Runs z3asm on minimal Asar-compatible snippets and checks assembly success
and (optionally) output ROM bytes. If ASAR_EXE is set, can compare z3asm
output with upstream Asar for the same snippet.

Snippets live in tests/asar_compat/snippets/; each test case is a main .asm
file. Include paths are the snippet directory so incsrc works.

Usage:
  pytest tests/asar_compat/test_asar_syntax.py -v
  Z3ASM=/path/to/z3asm pytest tests/asar_compat/test_asar_syntax.py -v
  ASAR_EXE=/path/to/asar pytest tests/asar_compat/test_asar_syntax.py -v  # optional compare
"""

from __future__ import annotations

import os
import subprocess
import tempfile
from pathlib import Path

import pytest

# Repo root = parent of tests/
REPO_ROOT = Path(__file__).resolve().parent.parent.parent
SNIPPETS_DIR = Path(__file__).resolve().parent / "snippets"


def find_z3asm() -> Path | None:
    """Resolve z3asm binary: Z3ASM env, then build/bin/z3asm, then PATH."""
    exe = os.environ.get("Z3ASM")
    if exe:
        p = Path(exe)
        if p.is_file():
            return p
    for candidate in (
        REPO_ROOT / "build" / "bin" / "z3asm",
        REPO_ROOT / "build" / "src" / "z3asm" / "z3asm",
        REPO_ROOT / "build" / "src" / "z3asm" / "bin" / "z3asm",
    ):
        if candidate.exists():
            return candidate
    import shutil
    return Path(shutil.which("z3asm")) if shutil.which("z3asm") else None


def find_asar_exe() -> Path | None:
    """Optional upstream Asar binary for comparison."""
    exe = os.environ.get("ASAR_EXE")
    if exe:
        p = Path(exe)
        if p.is_file():
            return p
    import shutil
    return Path(shutil.which("asar")) if shutil.which("asar") else None


def run_z3asm(main_asm: Path, cwd: Path, rom_path: Path, extra_args: list[str] | None = None) -> tuple[int, bytes]:
    """Run z3asm on main_asm; return (returncode, rom_bytes)."""
    args = [str(find_z3asm()), str(main_asm), str(rom_path)]
    if extra_args:
        args[1:1] = extra_args
    result = subprocess.run(
        args,
        cwd=str(cwd),
        capture_output=True,
        timeout=10,
    )
    rom_bytes = rom_path.read_bytes() if rom_path.exists() else b""
    return result.returncode, rom_bytes


def run_asar(main_asm: Path, cwd: Path, rom_path: Path) -> tuple[int, bytes]:
    """Run upstream Asar on main_asm; return (returncode, rom_bytes)."""
    result = subprocess.run(
        [str(find_asar_exe()), str(main_asm), str(rom_path)],
        cwd=str(cwd),
        capture_output=True,
        timeout=10,
    )
    rom_bytes = rom_path.read_bytes() if rom_path.exists() else b""
    return result.returncode, rom_bytes


# Test cases: (main_asm_basename, expected_rom_hex_or_none).
# expected is first N bytes at PC 0 (LoROM $8000) or None to only require exit 0.
CASES = [
    ("org_db.asm", "42"),
    ("incsrc_main.asm", "11"),
    ("math.asm", "02 06 09"),  # 1+1=2, 2*3=6, (1+2)*3=9
    ("defines.asm", "42"),
    ("labels.asm", "00 01"),
    ("dw_dl.asm", "34 12 56 34 12"),  # dw $1234 LE, dl $123456 LE
    # pad: org $8000, db $01, pad $8005, db $02 — pad fills with 0; exact end varies
    ("pad.asm", "01 00 00 00"),
]


@pytest.fixture(scope="module")
def z3asm_path():
    p = find_z3asm()
    if p is None:
        pytest.skip("z3asm not found (build with cmake && make z3asm or set Z3ASM)")
    return p


@pytest.mark.parametrize("main_name,expected_hex", CASES)
def test_asar_syntax_z3asm(z3asm_path, main_name: str, expected_hex: str):
    """Assemble snippet with z3asm and assert success and expected bytes at $8000."""
    main_asm = SNIPPETS_DIR / main_name
    if not main_asm.exists():
        pytest.skip(f"snippet {main_name} not found")
    cwd = SNIPPETS_DIR
    with tempfile.TemporaryDirectory(prefix="asar_compat_") as tmp:
        rom_path = Path(tmp) / "out.sfc"
        # z3asm often needs an existing ROM or creates one; lorom default size
        code, rom = run_z3asm(main_asm, cwd, rom_path)
        assert code == 0, f"z3asm failed: {main_name}"
        expected_bytes = bytes.fromhex(expected_hex.replace(" ", ""))
        if expected_bytes:
            got = rom[: len(expected_bytes)]
            assert got == expected_bytes, (
                f"ROM mismatch for {main_name}: got {got.hex()}, expected {expected_bytes.hex()}"
            )


@pytest.mark.parametrize("main_name,expected_hex", CASES)
def test_asar_syntax_compare_upstream(z3asm_path, main_name: str, expected_hex: str):
    """If ASAR_EXE is set, compare z3asm ROM with upstream Asar ROM."""
    asar_exe = find_asar_exe()
    if asar_exe is None:
        pytest.skip("ASAR_EXE not set; skipping upstream comparison")
    main_asm = SNIPPETS_DIR / main_name
    if not main_asm.exists():
        pytest.skip(f"snippet {main_name} not found")
    cwd = SNIPPETS_DIR
    with tempfile.TemporaryDirectory(prefix="asar_compat_") as tmp:
        rom_z3 = Path(tmp) / "z3asm.sfc"
        rom_asar = Path(tmp) / "asar.sfc"
        code_z3, rom_z3_b = run_z3asm(main_asm, cwd, rom_z3)
        code_asar, rom_asar_b = run_asar(main_asm, cwd, rom_asar)
        if code_asar != 0:
            pytest.skip(f"upstream Asar failed on {main_name}")
        assert code_z3 == 0, f"z3asm failed on {main_name}"
        # Compare min length (either may pad differently)
        n = min(len(rom_z3_b), len(rom_asar_b))
        assert rom_z3_b[:n] == rom_asar_b[:n], (
            f"z3asm vs Asar ROM mismatch for {main_name} (first {n} bytes)"
        )
