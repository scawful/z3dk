#!/usr/bin/env python3
"""
z3dk Scaffold Generator - Generate ASM scaffolds from templates.

Produces correct structural 65816 ASM for the WLA-DX/Asar assembler so that
AI agents (or humans) can fill in only the custom logic sections.

Usage:
    python3 scaffold.py sprite --name "MyEnemy" --states idle,chase,attack --bank 0x30
    python3 scaffold.py npc --name "MyNPC" --reactions 3 --has-follower
    python3 scaffold.py routine --name "MyRoutine" --long --bank 30 --preserve "AXY"
    python3 scaffold.py hook --address 0x02C0C3 --name "MyOverworldHook" --entry-state "m8x8"
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Optional

TEMPLATES_DIR = Path(__file__).resolve().parent.parent / "templates"


def parse_bank(value: str) -> int:
    """Parse a bank number from decimal or hex (0x1E, $1E, 1E all accepted)."""
    v = value.strip()
    if v.startswith("0x") or v.startswith("0X"):
        return int(v, 16)
    if v.startswith("$"):
        return int(v[1:], 16)
    # Try decimal first, then hex if it contains a-f
    try:
        return int(v, 10)
    except ValueError:
        return int(v, 16)

# Width state descriptions for hooks
ENTRY_STATE_COMMENTS = {
    "m8x8": "M=8-bit (A/memory), X=8-bit (index)",
    "m8x16": "M=8-bit (A/memory), X=16-bit (index)",
    "m16x8": "M=16-bit (A/memory), X=8-bit (index)",
    "m16x16": "M=16-bit (A/memory), X=16-bit (index)",
}

# SEP/REP opcodes for register width setup
# SEP #$XX sets bits (switches to 8-bit), REP #$XX clears bits (switches to 16-bit)
# bit 5 = M (accumulator/memory), bit 4 = X (index)
ENTRY_WIDTH_SETUP = {
    "m8x8": "  SEP #$30            ; M=8, X=8\n",
    "m8x16": "  SEP #$20            ; M=8\n  REP #$10            ; X=16\n",
    "m16x8": "  REP #$20            ; M=16\n  SEP #$10            ; X=8\n",
    "m16x16": "  REP #$30            ; M=16, X=16\n",
}

# Restore to default m8x8 after hook logic (only needed if entry != m8x8)
EXIT_WIDTH_RESTORE = {
    "m8x8": "",
    "m8x16": "  SEP #$30            ; restore M=8, X=8\n",
    "m16x8": "  SEP #$30            ; restore M=8, X=8\n",
    "m16x16": "  SEP #$30            ; restore M=8, X=8\n",
}


def load_template(name: str) -> str:
    """Load a .asm.tmpl file from the templates directory."""
    path = TEMPLATES_DIR / f"{name}.asm.tmpl"
    if not path.exists():
        print(f"Error: Template not found: {path}", file=sys.stderr)
        sys.exit(1)
    return path.read_text()


# =============================================================================
# Sprite scaffold
# =============================================================================

def build_state_table(name: str, states: list[str]) -> str:
    """Build the dw jump table entries for sprite states."""
    lines = []
    for state in states:
        lines.append(f"  dw {name}_State_{state}")
    return "\n".join(lines)


def build_state_routines(name: str, states: list[str]) -> str:
    """Build individual state routine stubs for a sprite.

    State routines use RTS (not RTL) because JumpTableLocal consumes the
    JSL return address.  The state's RTS pops the 2-byte address pushed by
    the JSR in the Long wrapper, returning control to Long's PLB + RTL.
    """
    routines = []
    for state in states:
        routine = (
            f"; --- State: {state} ---\n"
            f"{name}_State_{state}:\n"
            f"{{\n"
            f"  ; === BEGIN CUSTOM LOGIC ===\n"
            f"  ; TODO: Add {state} behavior here\n"
            f"  ; A = 8-bit, X/Y = 8-bit, DBR = current bank\n"
            f"  ; Available: SprTimerA-D ($0DF0-$0F50,X), SprAction ($0D80,X)\n"
            f"  ; === END CUSTOM LOGIC ===\n"
            f"\n"
            f"  RTS\n"
            f"}}"
        )
        routines.append(routine)
    return "\n\n".join(routines)


def generate_sprite(args: argparse.Namespace) -> str:
    """Generate a sprite scaffold from the template."""
    template = load_template("sprite")
    states = [s.strip() for s in args.states.split(",")]

    replacements = {
        "name": args.name,
        "bank": f"{args.bank:02X}",
        "namespace": args.namespace,
        "state_table": build_state_table(args.name, states),
        "state_routines": build_state_routines(args.name, states),
    }

    return template.format(**replacements)


# =============================================================================
# NPC scaffold
# =============================================================================

def build_reaction_table(name: str, count: int) -> str:
    """Build crystal-count reaction entries for an NPC."""
    if count <= 0:
        return ""

    lines = [
        f"; Crystal-count reaction table",
        f"{name}_ReactionTable:",
        f"{{",
    ]
    for i in range(count):
        lines.append(f"  ; Reaction {i}: crystal count >= {i + 1}")
        lines.append(f"  ; === BEGIN CUSTOM LOGIC ===")
        lines.append(f"  ; TODO: Reaction when player has {i + 1}+ crystals")
        lines.append(f"  ; === END CUSTOM LOGIC ===")
        if i < count - 1:
            lines.append(f"")
    lines.append(f"}}")
    return "\n".join(lines)


def build_follower_check(name: str, has_follower: bool) -> str:
    """Build follower interaction check block."""
    if not has_follower:
        return ""

    return (
        f"\n"
        f"; Follower interaction check\n"
        f"{name}_CheckFollower:\n"
        f"{{\n"
        f"  ; === BEGIN CUSTOM LOGIC ===\n"
        f"  ; TODO: Check if NPC should respond to active follower\n"
        f"  ; Read FollowerIndicator ($7EF3CC) to check follower type\n"
        f"  ; === END CUSTOM LOGIC ===\n"
        f"\n"
        f"  RTS\n"
        f"}}"
    )


def generate_npc(args: argparse.Namespace) -> str:
    """Generate an NPC scaffold from the template."""
    template = load_template("npc")

    replacements = {
        "name": args.name,
        "namespace": args.namespace,
        "reaction_table": build_reaction_table(args.name, args.reactions),
        "follower_check": build_follower_check(args.name, args.has_follower),
    }

    return template.format(**replacements)


# =============================================================================
# Routine scaffold
# =============================================================================

def build_preserve_push(preserve: Optional[str]) -> str:
    """Build PHA/PHX/PHY sequence for register preservation."""
    if not preserve:
        return ""

    ops = []
    p = preserve.upper()
    if "A" in p:
        ops.append("  PHA")
    if "X" in p:
        ops.append("  PHX")
    if "Y" in p:
        ops.append("  PHY")

    return "\n".join(ops) + "\n\n" if ops else ""


def build_preserve_pull(preserve: Optional[str]) -> str:
    """Build PLA/PLX/PLY sequence (reverse order) for register restoration."""
    if not preserve:
        return ""

    ops = []
    p = preserve.upper()
    # Pull in reverse order of push
    if "Y" in p:
        ops.append("  PLY")
    if "X" in p:
        ops.append("  PLX")
    if "A" in p:
        ops.append("  PLA")

    return "\n" + "\n".join(ops) + "\n" if ops else ""


def generate_routine(args: argparse.Namespace) -> str:
    """Generate a routine scaffold from the template."""
    template = load_template("routine")

    is_long = args.long
    return_instruction = "RTL" if is_long else "RTS"
    routine_type = "Long (JSL target)" if is_long else "Short (JSR target)"

    # Bank setup only makes sense for long routines
    if args.bank is not None:
        bank_setup = "  PHB : PHK : PLB\n\n"
        bank_restore = "\n  PLB\n"
    else:
        bank_setup = ""
        bank_restore = ""

    # Width comment depends on context
    if args.bank is not None:
        width_comment = f"DBR = bank ${args.bank:02X}, A/X/Y = 8-bit assumed"
    else:
        width_comment = "A/X/Y = caller-defined widths"

    replacements = {
        "name": args.name,
        "routine_type": routine_type,
        "return_instruction": return_instruction,
        "bank_setup": bank_setup,
        "bank_restore": bank_restore,
        "preserve_push": build_preserve_push(args.preserve),
        "preserve_pull": build_preserve_pull(args.preserve),
        "width_comment": width_comment,
    }

    return template.format(**replacements)


# =============================================================================
# Hook scaffold
# =============================================================================

def generate_hook(args: argparse.Namespace) -> str:
    """Generate a hook scaffold from the template."""
    template = load_template("hook")

    # Parse address (accept 0x prefix or bare hex)
    addr_str = args.address
    if addr_str.startswith("0x") or addr_str.startswith("0X"):
        address = int(addr_str, 16)
    elif addr_str.startswith("$"):
        address = int(addr_str[1:], 16)
    else:
        address = int(addr_str, 16)

    entry_state = args.entry_state
    entry_state_comment = ENTRY_STATE_COMMENTS.get(
        entry_state, f"Unknown state: {entry_state}"
    )
    entry_width_setup = ENTRY_WIDTH_SETUP.get(entry_state, "")
    exit_width_restore = EXIT_WIDTH_RESTORE.get(entry_state, "")

    replacements = {
        "name": args.name,
        "address": f"{address:06X}",
        "entry_state_comment": entry_state_comment,
        "entry_width_setup": entry_width_setup,
        "exit_width_restore": exit_width_restore,
    }

    return template.format(**replacements)


# =============================================================================
# CLI
# =============================================================================

def main() -> None:
    parser = argparse.ArgumentParser(
        prog="scaffold",
        description="z3dk scaffold generator - produce 65816 ASM from templates",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    # --- sprite ---
    sp = subparsers.add_parser("sprite", help="Generate a sprite scaffold")
    sp.add_argument("--name", required=True, help="Sprite name (e.g. MyEnemy)")
    sp.add_argument(
        "--states",
        required=True,
        help="Comma-separated state names (e.g. idle,chase,attack)",
    )
    sp.add_argument("--bank", type=parse_bank, default=0x30, help="ROM bank number (default: 0x30, hex ok: 0x30 or $30)")
    sp.add_argument("--namespace", default="Oracle", help="ASM namespace (default: Oracle)")
    sp.add_argument("-o", "--output", help="Output file path (default: stdout)")

    # --- npc ---
    np = subparsers.add_parser("npc", help="Generate an NPC scaffold")
    np.add_argument("--name", required=True, help="NPC name (e.g. MyNPC)")
    np.add_argument(
        "--reactions",
        type=int,
        default=0,
        help="Number of crystal-count reaction entries (default: 0)",
    )
    np.add_argument(
        "--has-follower",
        action="store_true",
        help="Include follower interaction check",
    )
    np.add_argument("--namespace", default="Oracle", help="ASM namespace (default: Oracle)")
    np.add_argument("-o", "--output", help="Output file path (default: stdout)")

    # --- routine ---
    rp = subparsers.add_parser("routine", help="Generate a routine scaffold")
    rp.add_argument("--name", required=True, help="Routine name (e.g. MyRoutine)")
    rp.add_argument(
        "--long",
        action="store_true",
        help="Use RTL (JSL target) instead of RTS (JSR target)",
    )
    rp.add_argument("--bank", type=parse_bank, default=None, help="Include PHB/PHK/PLB bank setup (hex ok: 0x1E or $1E)")
    rp.add_argument(
        "--preserve",
        default=None,
        help='Registers to push/pull: "A", "AX", "AXY", etc.',
    )
    rp.add_argument("-o", "--output", help="Output file path (default: stdout)")

    # --- hook ---
    hp = subparsers.add_parser("hook", help="Generate a hook scaffold")
    hp.add_argument("--name", required=True, help="Hook label name")
    hp.add_argument(
        "--address",
        required=True,
        help="SNES address being hooked (hex, e.g. 0x02C0C3 or $02C0C3)",
    )
    hp.add_argument(
        "--entry-state",
        default="m8x8",
        choices=["m8x8", "m8x16", "m16x8", "m16x16"],
        help="Expected register width at hook entry (default: m8x8)",
    )
    hp.add_argument("-o", "--output", help="Output file path (default: stdout)")

    args = parser.parse_args()

    generators = {
        "sprite": generate_sprite,
        "npc": generate_npc,
        "routine": generate_routine,
        "hook": generate_hook,
    }

    result = generators[args.command](args)

    if args.output:
        out_path = Path(args.output)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(result)
        print(f"Scaffold written to {out_path}", file=sys.stderr)
    else:
        print(result)


if __name__ == "__main__":
    main()
