#!/usr/bin/env python3
"""Tests for z3dk scaffold generator.

Validates that scaffold.py produces structurally correct 65816 ASM that
follows Oracle of Secrets conventions:
- Sprite/NPC: Long/Main split, JumpTableLocal dispatch, RTS in states
- Routine: correct RTL/RTS based on call type, PHB/PLB pairing
- Hook: correct SEP/REP setup and restore

These tests don't require a ROM or emulator — they validate the generated
ASM text output against known-correct patterns.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

import pytest

# Add z3dk scripts directory to path
SCRIPTS_DIR = Path(__file__).resolve().parent.parent.parent / "scripts"
sys.path.insert(0, str(SCRIPTS_DIR))

from scaffold import (
    generate_sprite,
    generate_npc,
    generate_routine,
    generate_hook,
    build_state_table,
    build_state_routines,
    build_preserve_push,
    build_preserve_pull,
    parse_bank,
)


# =============================================================================
# Helpers
# =============================================================================

def make_sprite_args(**overrides) -> argparse.Namespace:
    defaults = {
        "name": "TestEnemy",
        "states": "idle,chase,attack",
        "bank": 0x30,
        "namespace": "Oracle",
        "output": None,
    }
    defaults.update(overrides)
    return argparse.Namespace(**defaults)


def make_npc_args(**overrides) -> argparse.Namespace:
    defaults = {
        "name": "TestNPC",
        "reactions": 0,
        "has_follower": False,
        "namespace": "Oracle",
        "output": None,
    }
    defaults.update(overrides)
    return argparse.Namespace(**defaults)


def make_routine_args(**overrides) -> argparse.Namespace:
    defaults = {
        "name": "TestRoutine",
        "long": False,
        "bank": None,
        "preserve": None,
        "output": None,
    }
    defaults.update(overrides)
    return argparse.Namespace(**defaults)


def make_hook_args(**overrides) -> argparse.Namespace:
    defaults = {
        "name": "TestHook",
        "address": "0x02C0C3",
        "entry_state": "m8x8",
        "output": None,
    }
    defaults.update(overrides)
    return argparse.Namespace(**defaults)


def count_occurrences(text: str, pattern: str) -> int:
    """Count non-overlapping occurrences of pattern in text."""
    return len(re.findall(pattern, text))


# =============================================================================
# Tests: parse_bank
# =============================================================================

class TestParseBank:
    def test_decimal(self):
        assert parse_bank("48") == 48

    def test_hex_0x_prefix(self):
        assert parse_bank("0x30") == 0x30

    def test_hex_dollar_prefix(self):
        assert parse_bank("$30") == 0x30

    def test_hex_implicit(self):
        # "1E" can't be decimal, falls through to hex
        assert parse_bank("1E") == 0x1E

    def test_whitespace_stripped(self):
        assert parse_bank("  0x30  ") == 0x30


# =============================================================================
# Tests: Sprite scaffold
# =============================================================================

class TestSpriteScaffold:
    def test_long_main_split(self):
        """Sprite must have separate Long (JSL target) and Main (JSR target)."""
        result = generate_sprite(make_sprite_args())
        assert "TestEnemy_Long:" in result
        assert "TestEnemy_Main:" in result

    def test_long_wrapper_has_phb_plb(self):
        """Long entry must have PHB/PHK/PLB and matching PLB."""
        result = generate_sprite(make_sprite_args())
        # Find the Long block
        long_start = result.index("TestEnemy_Long:")
        long_end = result.index("TestEnemy_Main:")
        long_block = result[long_start:long_end]
        assert "PHB : PHK : PLB" in long_block
        assert "PLB" in long_block
        assert "RTL" in long_block

    def test_long_calls_main_via_jsr(self):
        """Long wrapper must call Main via JSR (short call), not JSL."""
        result = generate_sprite(make_sprite_args())
        long_start = result.index("TestEnemy_Long:")
        long_end = result.index("TestEnemy_Main:")
        long_block = result[long_start:long_end]
        assert "JSR TestEnemy_Main" in long_block
        assert "JSL TestEnemy_Main" not in long_block

    def test_main_has_jumptablelocal(self):
        """Main dispatch must use JumpTableLocal."""
        result = generate_sprite(make_sprite_args())
        main_start = result.index("TestEnemy_Main:")
        main_end = result.index("TestEnemy_State_idle:")
        main_block = result[main_start:main_end]
        assert "JSL JumpTableLocal" in main_block

    def test_main_has_no_phb_plb(self):
        """Main dispatch must NOT have PHB/PLB (that's Long's job)."""
        result = generate_sprite(make_sprite_args())
        main_start = result.index("TestEnemy_Main:")
        main_end = result.index("TestEnemy_State_idle:")
        main_block = result[main_start:main_end]
        assert "PHB" not in main_block
        assert "PLB" not in main_block

    def test_state_routines_use_rts(self):
        """State routines must use RTS (not RTL) — JumpTableLocal convention."""
        result = generate_sprite(make_sprite_args())
        states = ["idle", "chase", "attack"]
        for state in states:
            # Extract each state block
            state_label = f"TestEnemy_State_{state}:"
            assert state_label in result
            state_start = result.index(state_label)
            # Find the closing brace
            brace_depth = 0
            for i, ch in enumerate(result[state_start:], start=state_start):
                if ch == '{':
                    brace_depth += 1
                elif ch == '}':
                    brace_depth -= 1
                    if brace_depth == 0:
                        state_block = result[state_start:i + 1]
                        break
            assert "RTS" in state_block, f"State {state} must use RTS"
            assert "RTL" not in state_block, f"State {state} must NOT use RTL"

    def test_state_count_matches_dw_table(self):
        """Number of dw entries must match number of states."""
        args = make_sprite_args(states="a,b,c,d")
        result = generate_sprite(args)
        dw_count = count_occurrences(result, r"^\s+dw\s+", )
        # findall on multiline
        dw_count = len(re.findall(r"^\s+dw\s+", result, re.MULTILINE))
        assert dw_count == 4

    def test_namespace_wrapping(self):
        """Output must be wrapped in namespace block."""
        result = generate_sprite(make_sprite_args(namespace="Oracle"))
        assert "namespace Oracle {" in result
        assert "} ; namespace Oracle" in result

    def test_bank_in_header(self):
        """Bank number must appear in the header comment."""
        result = generate_sprite(make_sprite_args(bank=0x30))
        assert "$30" in result

    def test_custom_logic_markers(self):
        """Each state must have BEGIN/END CUSTOM LOGIC markers."""
        result = generate_sprite(make_sprite_args())
        assert count_occurrences(result, "BEGIN CUSTOM LOGIC") == 3
        assert count_occurrences(result, "END CUSTOM LOGIC") == 3

    def test_sprite_check_active(self):
        """Long wrapper should include Sprite_CheckActive guard."""
        result = generate_sprite(make_sprite_args())
        long_start = result.index("TestEnemy_Long:")
        long_end = result.index("TestEnemy_Main:")
        long_block = result[long_start:long_end]
        assert "Sprite_CheckActive" in long_block


# =============================================================================
# Tests: NPC scaffold
# =============================================================================

class TestNPCScaffold:
    def test_long_main_split(self):
        """NPC must have Long/Main split like sprites."""
        result = generate_npc(make_npc_args())
        assert "TestNPC_Long:" in result
        assert "TestNPC_Main:" in result

    def test_states_use_rts(self):
        """NPC states (Init, Idle, Dialogue) must use RTS."""
        result = generate_npc(make_npc_args())
        for label in ["TestNPC_Init:", "TestNPC_Idle:", "TestNPC_Dialogue:"]:
            assert label in result
        # Check each state block for RTS
        for state_name in ["Init", "Idle", "Dialogue"]:
            label = f"TestNPC_{state_name}:"
            start = result.index(label)
            brace_depth = 0
            for i, ch in enumerate(result[start:], start=start):
                if ch == '{':
                    brace_depth += 1
                elif ch == '}':
                    brace_depth -= 1
                    if brace_depth == 0:
                        block = result[start:i + 1]
                        break
            assert "RTS" in block, f"NPC state {state_name} must use RTS"

    def test_init_increments_action(self):
        """Init state should advance to Idle via INC SprAction."""
        result = generate_npc(make_npc_args())
        init_start = result.index("TestNPC_Init:")
        init_block = result[init_start:result.index("TestNPC_Idle:")]
        assert "INC.w SprAction, X" in init_block

    def test_reaction_table_generated(self):
        """Reaction table should appear when reactions > 0."""
        result = generate_npc(make_npc_args(reactions=3))
        assert "TestNPC_ReactionTable:" in result
        assert "crystal count >= 1" in result
        assert "crystal count >= 3" in result

    def test_no_reaction_table_when_zero(self):
        """No reaction table when reactions = 0."""
        result = generate_npc(make_npc_args(reactions=0))
        assert "ReactionTable" not in result

    def test_follower_check_present(self):
        """Follower check block should appear when has_follower is True."""
        result = generate_npc(make_npc_args(has_follower=True))
        assert "TestNPC_CheckFollower:" in result
        assert "FollowerIndicator" in result

    def test_no_follower_check_when_false(self):
        """No follower check when has_follower is False."""
        result = generate_npc(make_npc_args(has_follower=False))
        assert "CheckFollower" not in result


# =============================================================================
# Tests: Routine scaffold
# =============================================================================

class TestRoutineScaffold:
    def test_short_routine_uses_rts(self):
        """Short routine (JSR target) must use RTS."""
        result = generate_routine(make_routine_args(long=False))
        assert "RTS" in result
        assert "RTL" not in result

    def test_long_routine_uses_rtl(self):
        """Long routine (JSL target) must use RTL."""
        result = generate_routine(make_routine_args(long=True))
        assert "RTL" in result

    def test_bank_setup_when_specified(self):
        """PHB/PHK/PLB should appear when bank is specified."""
        result = generate_routine(make_routine_args(long=True, bank=0x30))
        assert "PHB : PHK : PLB" in result
        assert "PLB" in result

    def test_no_bank_setup_when_none(self):
        """No PHB/PLB when bank is None."""
        result = generate_routine(make_routine_args(long=True, bank=None))
        assert "PHB" not in result

    def test_preserve_axy_push_order(self):
        """Register preservation must push A, X, Y in that order."""
        result = generate_routine(make_routine_args(preserve="AXY"))
        push_section = result[:result.index("BEGIN CUSTOM LOGIC")]
        lines = [l.strip() for l in push_section.splitlines() if l.strip()]
        push_ops = [l for l in lines if l.startswith("PH")]
        assert push_ops == ["PHA", "PHX", "PHY"]

    def test_preserve_axy_pull_reverse_order(self):
        """Register restoration must pull Y, X, A (reverse of push)."""
        result = generate_routine(make_routine_args(preserve="AXY"))
        pull_section = result[result.index("END CUSTOM LOGIC"):]
        lines = [l.strip() for l in pull_section.splitlines() if l.strip()]
        pull_ops = [l for l in lines if l.startswith("PL") and l != "PLB"]
        assert pull_ops == ["PLY", "PLX", "PLA"]

    def test_preserve_partial(self):
        """Only specified registers should be preserved."""
        result = generate_routine(make_routine_args(preserve="AX"))
        assert "PHA" in result
        assert "PHX" in result
        assert "PHY" not in result

    def test_no_preserve_when_none(self):
        """No PHA/PHX/PHY when preserve is None."""
        result = generate_routine(make_routine_args(preserve=None))
        assert "PHA" not in result
        assert "PHX" not in result
        assert "PHY" not in result

    def test_width_comment_with_bank(self):
        """Width comment should mention bank when specified."""
        result = generate_routine(make_routine_args(bank=0x30))
        assert "bank $30" in result


# =============================================================================
# Tests: Hook scaffold
# =============================================================================

class TestHookScaffold:
    def test_m8x8_no_setup_needed(self):
        """m8x8 is the default state — minimal SEP/REP needed."""
        result = generate_hook(make_hook_args(entry_state="m8x8"))
        assert "TestHook:" in result
        assert "RTL" in result
        # m8x8 entry should have SEP #$30 setup
        assert "SEP #$30" in result

    def test_m16x16_rep_setup(self):
        """m16x16 entry needs REP #$30 and restore to SEP #$30."""
        result = generate_hook(make_hook_args(entry_state="m16x16"))
        assert "REP #$30" in result
        # Should restore to m8x8 at exit
        assert "SEP #$30" in result

    def test_m8x16_mixed_setup(self):
        """m8x16 needs SEP #$20 + REP #$10."""
        result = generate_hook(make_hook_args(entry_state="m8x16"))
        assert "SEP #$20" in result
        assert "REP #$10" in result

    def test_m16x8_mixed_setup(self):
        """m16x8 needs REP #$20 + SEP #$10."""
        result = generate_hook(make_hook_args(entry_state="m16x8"))
        assert "REP #$20" in result
        assert "SEP #$10" in result

    def test_address_in_header(self):
        """Hook address must appear in the header comment."""
        result = generate_hook(make_hook_args(address="0x02C0C3"))
        assert "$02C0C3" in result

    def test_address_formats(self):
        """Various address formats should produce the same output."""
        for addr_str in ["0x02C0C3", "$02C0C3", "02C0C3"]:
            result = generate_hook(make_hook_args(address=addr_str))
            assert "$02C0C3" in result

    def test_custom_logic_markers(self):
        """Hook must have BEGIN/END CUSTOM LOGIC markers."""
        result = generate_hook(make_hook_args())
        assert "BEGIN CUSTOM LOGIC" in result
        assert "END CUSTOM LOGIC" in result


# =============================================================================
# Tests: build_* helpers
# =============================================================================

class TestBuildHelpers:
    def test_state_table_entries(self):
        table = build_state_table("Foo", ["a", "b", "c"])
        lines = [l.strip() for l in table.strip().splitlines()]
        assert lines == ["dw Foo_State_a", "dw Foo_State_b", "dw Foo_State_c"]

    def test_state_routines_count(self):
        routines = build_state_routines("Foo", ["x", "y"])
        assert routines.count("Foo_State_x:") == 1
        assert routines.count("Foo_State_y:") == 1

    def test_preserve_push_empty(self):
        assert build_preserve_push(None) == ""
        assert build_preserve_push("") == ""

    def test_preserve_pull_empty(self):
        assert build_preserve_pull(None) == ""
        assert build_preserve_pull("") == ""

    def test_preserve_push_order(self):
        result = build_preserve_push("AXY")
        ops = [l.strip() for l in result.strip().splitlines()]
        assert ops == ["PHA", "PHX", "PHY"]

    def test_preserve_pull_reverse(self):
        result = build_preserve_pull("AXY")
        ops = [l.strip() for l in result.strip().splitlines()]
        assert ops == ["PLY", "PLX", "PLA"]
