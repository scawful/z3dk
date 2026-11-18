#!/usr/bin/env python3
"""Tests for the peephole lint rules (lint.json output).

unindexed-slot-table, forced-width-truncation, spawn-result-unchecked.
"""
from __future__ import annotations

import json
import pathlib
import subprocess
import tempfile

import pytest

from test_emit_outputs import find_z3asm

HEADER = (
    "lorom\n"
    "SprXSpeed = $0D50\n"
    "SprMiscF = $0EC0\n"
    "SprCachedX = $0FD8\n"
    "Sprite_SpawnDynamically = $1DF65D\n"
    "org $1DF65D\n"
    "  RTL\n"
    "org $008000\n"
    "Entry:\n"
    "  SEP #$30\n"
)


@pytest.fixture(scope="module")
def z3asm_path() -> pathlib.Path:
    exe = find_z3asm()
    if not exe:
        pytest.skip("z3asm not found (build z3dk or set Z3ASM)")
    return exe


def run_lint(z3asm: pathlib.Path, body: str, toml: str = 'preset = "alttp"\n',
             header: str = HEADER):
    """Assemble HEADER+body; return [(line, message)] for rule warnings."""
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        (root / "z3dk.toml").write_text(toml)
        asm = root / "main.asm"
        asm.write_text(header + body + "  RTL\n")
        lint = root / "lint.json"
        proc = subprocess.run(
            [str(z3asm), f"--config={root / 'z3dk.toml'}", f"--emit={lint}",
             str(asm), str(root / "out.sfc")],
            capture_output=True, text=True)
        assert proc.returncode == 0, proc.stdout + proc.stderr
        data = json.loads(lint.read_text())
    return [(w["line"], w["message"]) for w in data["warnings"]
            if w["message"].startswith("[")]


def lines_for(hits, rule):
    return [line for line, msg in hits if msg.startswith(f"[{rule}]")]


BODY_LINE = HEADER.count("\n") + 1  # first body line number


# --- unindexed-slot-table -------------------------------------------------

def test_slot_table_positive(z3asm_path):
    hits = run_lint(z3asm_path, "  LDA SprXSpeed\n  STA.w $0EC0\n")
    assert lines_for(hits, "unindexed-slot-table") == [BODY_LINE, BODY_LINE + 1]
    assert "SprXSpeed" in hits[0][1]


def test_slot_table_negative(z3asm_path):
    body = ("  LDA SprXSpeed, X\n"
            "  STA SprMiscF, Y\n"
            "  LDA SprCachedX\n"          # singleton, outside slot ranges
            "  LDA $0D50, X : STA $10\n"
            "  STA SprMiscF ; lint: allow unindexed-slot-table\n")
    assert lines_for(run_lint(z3asm_path, body), "unindexed-slot-table") == []


def test_slot_table_exempt_and_disable(z3asm_path):
    body = "  LDA SprXSpeed\n  LDA SprMiscF\n"
    hits = run_lint(z3asm_path, body,
                    'preset = "alttp"\nslot_table_exempt = ["$0D50"]\n')
    assert lines_for(hits, "unindexed-slot-table") == [BODY_LINE + 1]
    hits = run_lint(z3asm_path, body,
                    'preset = "alttp"\nwarn_unindexed_slot_table = false\n')
    assert lines_for(hits, "unindexed-slot-table") == []


def test_slot_table_ranges_from_config(z3asm_path):
    # Without a preset, only configured ranges are checked.
    body = "  LDA SprXSpeed\n  LDA $0700\n"
    assert run_lint(z3asm_path, body, "") == []
    hits = run_lint(z3asm_path, body, 'slot_table_ranges = ["$0700-$0710"]\n')
    assert lines_for(hits, "unindexed-slot-table") == [BODY_LINE + 1]


# --- forced-width-truncation ---------------------------------------------

def test_forced_width_positive(z3asm_path):
    hits = run_lint(z3asm_path, "  LDA.b SprXSpeed\n  STA.b $0D40\n")
    assert lines_for(hits, "forced-width-truncation") == [BODY_LINE, BODY_LINE + 1]
    assert "$50" in hits[0][1]


def test_forced_width_negative(z3asm_path):
    body = ("  LDA.b $10\n"
            "  LDA.b #$FF\n"
            "  LDA.w SprXSpeed\n"
            "  LDA.b $7E0010\n"           # high bytes dropped on purpose: fits
            "  LDA.b SprXSpeed ; lint: allow forced-width-truncation\n")
    assert lines_for(run_lint(z3asm_path, body), "forced-width-truncation") == []


# --- spawn-result-unchecked ----------------------------------------------

def test_spawn_unchecked_positive(z3asm_path):
    body = ("  LDA #$01\n"
            "  JSL Sprite_SpawnDynamically\n"
            "  LDA #$02\n"
            "  STA $0D00, Y\n")
    hits = run_lint(z3asm_path, body)
    assert lines_for(hits, "spawn-result-unchecked") == [BODY_LINE + 1]


@pytest.mark.parametrize("test_op", ["BMI .skip", "BCS .skip", "BPL .ok",
                                     "CPY #$FF : BEQ .skip"])
def test_spawn_unchecked_negative_tested(z3asm_path, test_op):
    body = ("  JSL Sprite_SpawnDynamically\n"
            f"  {test_op}\n"
            ".ok\n"
            "  STA $0D00, Y\n"
            ".skip\n")
    assert lines_for(run_lint(z3asm_path, body), "spawn-result-unchecked") == []


def test_spawn_unchecked_negative_suppressed_and_unused(z3asm_path):
    body = ("  JSL Sprite_SpawnDynamically ; lint: allow spawn-unchecked\n"
            "  STA $0D00, Y\n"
            "  JSL Sprite_SpawnDynamically\n"   # result never used via ,Y
            "  RTL\n")
    assert lines_for(run_lint(z3asm_path, body), "spawn-result-unchecked") == []


NAMESPACED_HEADER = (
    "lorom\n"
    "namespace Game\n"
    "SprXSpeed = $0D50\n"
    "Sprite_SpawnDynamically = $1DF65D\n"
    "org $1DF65D\n"
    "  RTL\n"
    "org $008000\n"
    "Entry:\n"
    "  SEP #$30\n"
)


def test_namespaced_labels_resolve_by_suffix(z3asm_path):
    # Labels are Game_SprXSpeed / Game_Sprite_SpawnDynamically; source text
    # names them without the namespace prefix.
    body = (
        "  LDA.b SprXSpeed\n"                 # line 10: K2
        "  JSL Sprite_SpawnDynamically\n"     # line 11: K3
        "  STA.w $0D50, Y\n"
        "namespace off\n"
    )
    hits = run_lint(z3asm_path, body, header=NAMESPACED_HEADER)
    assert lines_for(hits, "forced-width-truncation") == [10]
    assert lines_for(hits, "spawn-result-unchecked") == [11]
