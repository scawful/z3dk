#!/usr/bin/env python3
"""Tests for z3asm emitted outputs (hooks.json + annotations.json)."""
from __future__ import annotations

import json
import os
import pathlib
import shutil
import subprocess
import tempfile

import pytest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]


def find_z3asm() -> pathlib.Path | None:
    """Resolve z3asm binary: Z3ASM env, then build/bin/z3asm, then PATH."""
    exe = os.environ.get("Z3ASM")
    if exe:
        return pathlib.Path(exe)
    candidates = [
        REPO_ROOT / "build" / "bin" / "z3asm",
        REPO_ROOT / "build" / "src" / "z3asm" / "z3asm",
        REPO_ROOT / "build" / "src" / "z3asm" / "bin" / "z3asm",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return pathlib.Path(shutil.which("z3asm")) if shutil.which("z3asm") else None


@pytest.fixture(scope="module")
def z3asm_path() -> pathlib.Path:
    exe = find_z3asm()
    if not exe:
        pytest.skip("z3asm not found (build z3dk or set Z3ASM)")
    return exe


def _write_file(path: pathlib.Path, contents: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(contents)


def test_emit_hooks_and_annotations(z3asm_path: pathlib.Path) -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        asm_path = root / "main.asm"
        rom_path = root / "out.sfc"
        hooks_path = root / "hooks.json"
        annotations_path = root / "annotations.json"
        sourcemap_path = root / "sourcemap.json"

        _write_file(
            asm_path,
            "lorom\n"
            "org $008000 : JSL HookTarget ; @hook name=TestHook kind=jsl target=HookTarget expected_m=8 expected_x=8\n"
            "HookTarget:\n"
            "  RTL\n"
            "TestVar = $7E0010 ; @watch fmt=hex\n"
            "; @assert A == $00\n"
            "; @abi long_entry\n",
        )

        rom_path.write_bytes(b"\x00" * 0x200000)

        cmd = [
            str(z3asm_path),
            str(asm_path),
            str(rom_path),
            "--emit=hooks.json",
            "--emit=annotations.json",
            "--emit=sourcemap.json",
        ]
        subprocess.check_call(cmd, cwd=root)

        hooks = json.loads(hooks_path.read_text())
        hooks_list = hooks.get("hooks", [])
        target = next((h for h in hooks_list if h.get("address") == "0x808000"), None)
        assert target is not None, "missing hook entry for $808000"
        assert target.get("name") == "TestHook"
        assert target.get("kind") == "jsl"
        assert target.get("target") == "HookTarget"
        assert target.get("expected_m") == 8
        assert target.get("expected_x") == 8

        annotations = json.loads(annotations_path.read_text())
        ann_list = annotations.get("annotations", [])
        watch = next((a for a in ann_list if a.get("type") == "watch"), None)
        assert watch is not None, "missing @watch annotation"
        assert watch.get("label") == "TestVar"
        assert watch.get("address") == "0x7E0010"
        assert watch.get("format") == "hex"

        assert any(a.get("type") == "assert" for a in ann_list), "missing @assert annotation"
        assert any(a.get("type") == "abi" for a in ann_list), "missing @abi annotation"

        assert sourcemap_path.exists(), "sourcemap.json missing"
