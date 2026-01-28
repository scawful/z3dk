#!/usr/bin/env python3
"""
Oracle of Secrets Analyzer - Specialized Static Analysis

Extends z3dk static analyzer with Oracle-specific knowledge:
- Known hook patterns and expected states
- ALTTP vanilla routine conventions
- Common bug patterns (soft locks, screen issues, etc.)

Usage:
    python3 oracle_analyzer.py [path_to_rom]
    python3 oracle_analyzer.py --check-hooks
    python3 oracle_analyzer.py --find-mx-mismatches
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

# Import from static_analyzer
from static_analyzer import (
    RegisterState, HookInfo, AnalysisDiagnostic, AnalysisResult,
    StateTracker, parse_sym_file, snes_to_pc, pc_to_snes
)


# =============================================================================
# Oracle of Secrets Specific Knowledge
# =============================================================================

# Default paths
ORACLE_DIR = Path.home() / "src" / "hobby" / "oracle-of-secrets"
DEFAULT_ROM = ORACLE_DIR / "Roms" / "oos168x.sfc"
DEFAULT_SYM = ORACLE_DIR / "Roms" / "oos168x.sym"
DEFAULT_HOOKS = ORACLE_DIR / "hooks.json"

# Known critical hooks and their expected entry states
# Format: (address, name, expected_m, expected_x, note)
# NOTE: These are fallback hooks if hooks.json is not present
KNOWN_HOOKS = [
    # Core system hooks
    (0x008000, "Reset", True, True, "ROM entry point"),
    (0x0080C9, "NMI_Handler", True, True, "NMI entry - must be 8-bit"),

    # Overworld hooks
    (0x02C0C3, "Overworld_SetCameraBounds", True, True, "Camera calculation"),

    # Transition hooks
    (0x028BE7, "Sanctuary_Song_Disable", True, True, "Sanctuary song check"),
    (0x028364, "BedCutscene_ColorFix", True, True, "Color initialization"),

    # Music system
    (0x008891, "APU_SyncWait", True, True, "APU handshake - potential hang point"),

    # Sprite hooks
    (0x028A5B, "CheckForFollowerIntraroomTransition", True, True, "Follower transition"),
]

# Known problematic patterns
PROBLEMATIC_PATTERNS = [
    # Pattern: REP #$30 followed by JSL without SEP
    {
        'name': 'rep_without_sep',
        'description': 'REP #$30 before JSL without restoring state',
        'severity': 'warning',
        'opcodes': [0xC2, 0x30],  # REP #$30
    },
    # Pattern: PHA/PLA imbalance
    {
        'name': 'stack_imbalance',
        'description': 'Potential stack imbalance in routine',
        'severity': 'warning',
    },
]

# ALTTP vanilla routines that expect specific states
# NOTE: These are used as fallback if hooks.json doesn't specify expected state
VANILLA_STATE_EXPECTATIONS = {
    # Bank 00
    0x0080C9: RegisterState(m_flag=True, x_flag=True),   # NMI handler

    # Bank 01 (Dungeon)
    0x01C71B: RegisterState(m_flag=True, x_flag=True),   # RoomTag_GetHeartForPrize

    # Bank 02 (Overworld, Transitions)
    0x028364: RegisterState(m_flag=True, x_flag=True),   # Bed cutscene
    0x028A5B: RegisterState(m_flag=True, x_flag=True),   # Follower transition
    0x028BE7: RegisterState(m_flag=True, x_flag=True),   # Sanctuary song

    # Bank 09
    0x098823: RegisterState(m_flag=True, x_flag=True),   # Pendant chest position
}


# =============================================================================
# Oracle-Specific Analysis
# =============================================================================

@dataclass
class OracleAnalysisResult:
    """Extended analysis result with Oracle-specific info."""
    base_result: AnalysisResult
    hook_violations: list[dict]
    mx_mismatches: list[dict]
    potential_hangs: list[dict]
    color_issues: list[dict]

    def all_diagnostics(self):
        return (
            self.base_result.diagnostics +
            [AnalysisDiagnostic(**h) for h in self.hook_violations] +
            [AnalysisDiagnostic(**m) for m in self.mx_mismatches] +
            [AnalysisDiagnostic(**p) for p in self.potential_hangs] +
            [AnalysisDiagnostic(**c) for c in self.color_issues]
        )


def load_hooks_json(path: Path) -> tuple[list[tuple], dict[int, RegisterState]]:
    """Load hooks from hooks.json file."""
    hooks_list = []
    state_expectations = {}

    if not path.exists():
        return hooks_list, state_expectations

    with open(path) as f:
        data = json.load(f)

    for hook in data.get('hooks', []):
        addr_str = hook.get('address', '0')
        if addr_str.startswith('0x'):
            addr = int(addr_str, 16)
        elif addr_str.startswith('$'):
            addr = int(addr_str[1:], 16)
        else:
            addr = int(addr_str)

        name = hook.get('name', f'hook_{addr:06X}')
        exp_m = hook.get('expected_m')
        exp_x = hook.get('expected_x')
        note = hook.get('note', '')

        hooks_list.append((addr, name, exp_m == 8 if exp_m else None, exp_x == 8 if exp_x else None, note))

        if exp_m is not None or exp_x is not None:
            state_expectations[addr] = RegisterState(
                m_flag=exp_m == 8 if exp_m else None,
                x_flag=exp_x == 8 if exp_x else None
            )

    # Also load critical addresses
    for addr_info in data.get('critical_addresses', []):
        addr_str = addr_info.get('address', '0')
        if addr_str.startswith('0x'):
            addr = int(addr_str, 16)
        elif addr_str.startswith('$'):
            addr = int(addr_str[1:], 16)
        else:
            addr = int(addr_str)

        exp_m = addr_info.get('expected_m')
        exp_x = addr_info.get('expected_x')

        if exp_m is not None or exp_x is not None:
            state_expectations[addr] = RegisterState(
                m_flag=exp_m == 8 if exp_m else None,
                x_flag=exp_x == 8 if exp_x else None
            )

    return hooks_list, state_expectations


def analyze_oracle_rom(rom_path: Path, sym_path: Optional[Path] = None, hooks_path: Optional[Path] = None) -> OracleAnalysisResult:
    """Analyze an Oracle of Secrets ROM."""

    # Read ROM
    rom_data = rom_path.read_bytes()

    # Load symbols
    labels = {}
    if sym_path and sym_path.exists():
        labels = parse_sym_file(sym_path)

    # Load hooks from JSON if available, otherwise use built-in
    hooks_to_analyze = KNOWN_HOOKS
    state_expectations = dict(VANILLA_STATE_EXPECTATIONS)

    if hooks_path and hooks_path.exists():
        json_hooks, json_expectations = load_hooks_json(hooks_path)
        if json_hooks:
            hooks_to_analyze = json_hooks
        state_expectations.update(json_expectations)

    # Create tracker
    tracker = StateTracker(rom_data, 'lorom')
    tracker.labels = labels

    # Analyze from known hooks with expected states
    for addr, name, exp_m, exp_x, note in hooks_to_analyze:
        initial_state = RegisterState(
            m_flag=exp_m if exp_m is not None else True,
            x_flag=exp_x if exp_x is not None else True
        )
        tracker.analyze_from(addr, initial_state)

    # Build base result
    base_result = AnalysisResult(
        diagnostics=tracker.diagnostics,
        cross_refs=tracker.cross_refs,
        labels=labels,
        entry_states=tracker.visited,
    )

    # Oracle-specific checks
    hook_violations = check_hook_states(tracker, labels, state_expectations)
    mx_mismatches = find_mx_mismatches(rom_data, tracker, labels)
    potential_hangs = find_hang_points(rom_data, tracker, labels)
    color_issues = find_color_issues(rom_data, tracker, labels)

    return OracleAnalysisResult(
        base_result=base_result,
        hook_violations=hook_violations,
        mx_mismatches=mx_mismatches,
        potential_hangs=potential_hangs,
        color_issues=color_issues,
    )


def check_hook_states(tracker: StateTracker, labels: dict[int, str], state_expectations: dict[int, RegisterState]) -> list[dict]:
    """Check that hooks are entered with correct state."""
    violations = []

    for addr, expected_state in state_expectations.items():
        if addr not in tracker.visited:
            continue

        actual_state = tracker.visited[addr]
        label = labels.get(addr, f"${addr:06X}")

        issues = []
        if expected_state.m_flag is not None and actual_state.m_flag is not None:
            if expected_state.m_flag != actual_state.m_flag:
                exp = '8-bit' if expected_state.m_flag else '16-bit'
                act = '8-bit' if actual_state.m_flag else '16-bit'
                issues.append(f"M flag: expected {exp}, got {act}")

        if expected_state.x_flag is not None and actual_state.x_flag is not None:
            if expected_state.x_flag != actual_state.x_flag:
                exp = '8-bit' if expected_state.x_flag else '16-bit'
                act = '8-bit' if actual_state.x_flag else '16-bit'
                issues.append(f"X flag: expected {exp}, got {act}")

        if issues:
            violations.append({
                'severity': 'error',
                'message': f"State violation at {label}: " + "; ".join(issues),
                'address': addr,
                'context': {
                    'expected': str(expected_state),
                    'actual': str(actual_state),
                }
            })

    return violations


def find_mx_mismatches(rom_data: bytes, tracker: StateTracker, labels: dict[int, str]) -> list[dict]:
    """Find potential M/X flag mismatches at JSL call sites."""
    mismatches = []

    # Scan for JSL (0x22) instructions
    for pc in range(len(rom_data) - 3):
        if rom_data[pc] != 0x22:  # JSL
            continue

        src_addr = pc_to_snes(pc, 'lorom')
        target = rom_data[pc + 1] | (rom_data[pc + 2] << 8) | (rom_data[pc + 3] << 16)

        # Check if we have state info for both caller and callee
        if src_addr not in tracker.visited or target not in tracker.visited:
            continue

        caller_state = tracker.visited[src_addr]
        callee_state = tracker.visited[target]

        # Check for mismatches
        issues = []
        if caller_state.m_flag is not None and callee_state.m_flag is not None:
            if caller_state.m_flag != callee_state.m_flag:
                issues.append(f"M flag mismatch")

        if caller_state.x_flag is not None and callee_state.x_flag is not None:
            if caller_state.x_flag != callee_state.x_flag:
                issues.append(f"X flag mismatch")

        if issues:
            src_label = labels.get(src_addr, f"${src_addr:06X}")
            tgt_label = labels.get(target, f"${target:06X}")
            mismatches.append({
                'severity': 'warning',
                'message': f"JSL from {src_label} to {tgt_label}: " + ", ".join(issues),
                'address': src_addr,
                'context': {
                    'caller_state': str(caller_state),
                    'callee_state': str(callee_state),
                    'target': f"${target:06X}",
                }
            })

    return mismatches


def find_hang_points(rom_data: bytes, tracker: StateTracker, labels: dict[int, str]) -> list[dict]:
    """Find potential hang/soft-lock points."""
    hangs = []

    # Known hang patterns:
    # 1. Infinite loop waiting for APU response (LDA $2140 : CMP : BNE)
    # 2. Infinite loop without timeout counter
    # 3. JSL to non-existent address

    # Pattern: APU polling without timeout
    # LDA $2140 (AD 40 21) : CMP #$XX (C9 XX) : BNE rel (D0 XX)
    for pc in range(len(rom_data) - 6):
        if (rom_data[pc] == 0xAD and          # LDA abs
            rom_data[pc+1] == 0x40 and
            rom_data[pc+2] == 0x21 and        # $2140
            rom_data[pc+4] == 0xC9 and        # CMP #imm
            rom_data[pc+6] == 0xD0):          # BNE
            offset = rom_data[pc+7]
            if offset >= 0x80:
                offset -= 0x100
            # Check if branch goes backwards (potential infinite loop)
            if offset < 0:
                src_addr = pc_to_snes(pc, 'lorom')
                label = labels.get(src_addr, f"${src_addr:06X}")

                # Check if there's a timeout mechanism nearby
                # (simplified: just flag it as potential issue)
                hangs.append({
                    'severity': 'warning',
                    'message': f"Potential APU hang point at {label} - polling $2140 with backwards branch",
                    'address': src_addr,
                    'context': {
                        'pattern': 'apu_poll_loop',
                        'branch_offset': offset,
                    }
                })

    return hangs


def find_color_issues(rom_data: bytes, tracker: StateTracker, labels: dict[int, str]) -> list[dict]:
    """Find potential color/palette initialization issues."""
    issues = []

    # Check bed cutscene color initialization at $028364
    # Expected: LDA #$30, STA $9C, LDA #$50, STA $9D, LDA #$80, STA $9E
    bed_pc = snes_to_pc(0x028364, 'lorom')
    if bed_pc >= 0 and bed_pc < len(rom_data) - 10:
        # Check if the color values are reasonable
        # A9 30 85 9C A9 50 85 9D A9 80 85 9E
        expected = [0xA9, 0x30, 0x85, 0x9C, 0xA9, 0x50, 0x85, 0x9D, 0xA9, 0x80, 0x85, 0x9E]
        actual = list(rom_data[bed_pc:bed_pc+12])

        if actual != expected:
            issues.append({
                'severity': 'warning',
                'message': f"Bed cutscene color init at $028364 differs from expected",
                'address': 0x028364,
                'context': {
                    'expected': ' '.join(f'{b:02X}' for b in expected),
                    'actual': ' '.join(f'{b:02X}' for b in actual),
                }
            })

    # Check COLDATA register usage ($2132)
    # Scan for STA $2132 with potentially invalid values
    for pc in range(len(rom_data) - 2):
        if (rom_data[pc] == 0x8D and          # STA abs
            rom_data[pc+1] == 0x32 and
            rom_data[pc+2] == 0x21):          # $2132 (COLDATA)

            src_addr = pc_to_snes(pc, 'lorom')

            # Check preceding instruction for value being stored
            if pc >= 2 and rom_data[pc-2] == 0xA9:  # LDA #imm
                value = rom_data[pc-1]
                # COLDATA format: ccc-r-rrr (color select + intensity)
                # Valid color selects: 0x20 (R), 0x40 (G), 0x80 (B), combos
                if value == 0x00:
                    issues.append({
                        'severity': 'info',
                        'message': f"COLDATA at ${src_addr:06X} set to 0x00 (black/disabled)",
                        'address': src_addr,
                        'context': {'value': f'0x{value:02X}'}
                    })

    return issues


def scan_for_recent_changes(rom_path: Path, baseline_rom_path: Optional[Path] = None) -> list[dict]:
    """Compare ROMs to find recent changes that might cause issues."""
    changes = []

    if not baseline_rom_path or not baseline_rom_path.exists():
        return changes

    current = rom_path.read_bytes()
    baseline = baseline_rom_path.read_bytes()

    # Find changed regions
    min_len = min(len(current), len(baseline))
    diff_start = None
    diff_regions = []

    for i in range(min_len):
        if current[i] != baseline[i]:
            if diff_start is None:
                diff_start = i
        else:
            if diff_start is not None:
                diff_regions.append((diff_start, i))
                diff_start = None

    if diff_start is not None:
        diff_regions.append((diff_start, min_len))

    # Report significant changed regions
    for start, end in diff_regions:
        size = end - start
        if size > 4:  # Only report changes larger than a single instruction
            snes_start = pc_to_snes(start, 'lorom')
            changes.append({
                'severity': 'info',
                'message': f"Changed region: ${snes_start:06X} ({size} bytes)",
                'address': snes_start,
                'context': {
                    'pc_start': start,
                    'pc_end': end,
                    'size': size,
                }
            })

    return changes


# =============================================================================
# CLI
# =============================================================================

def main():
    parser = argparse.ArgumentParser(
        description="Oracle of Secrets Static Analyzer",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    parser.add_argument('rom', nargs='?', type=Path, default=DEFAULT_ROM,
                       help=f'ROM file (default: {DEFAULT_ROM})')
    parser.add_argument('--sym', type=Path, default=DEFAULT_SYM,
                       help=f'Symbol file (default: {DEFAULT_SYM})')
    parser.add_argument('--hooks', type=Path, default=DEFAULT_HOOKS,
                       help=f'Hooks manifest JSON (default: {DEFAULT_HOOKS})')
    parser.add_argument('--baseline', type=Path,
                       help='Baseline ROM for change detection')
    parser.add_argument('--check-hooks', action='store_true',
                       help='Check hook entry states')
    parser.add_argument('--find-mx', action='store_true',
                       help='Find M/X flag mismatches')
    parser.add_argument('--find-hangs', action='store_true',
                       help='Find potential hang points')
    parser.add_argument('--find-colors', action='store_true',
                       help='Find color/palette issues')
    parser.add_argument('--json', action='store_true',
                       help='Output as JSON')
    parser.add_argument('-v', '--verbose', action='store_true',
                       help='Verbose output')

    args = parser.parse_args()

    if not args.rom.exists():
        print(f"Error: ROM not found: {args.rom}", file=sys.stderr)
        return 1

    # Run analysis
    print(f"Analyzing: {args.rom}", file=sys.stderr)
    result = analyze_oracle_rom(
        args.rom,
        sym_path=args.sym if args.sym.exists() else None,
        hooks_path=args.hooks if args.hooks.exists() else None
    )

    # Collect diagnostics based on filters
    all_diags = []

    if args.check_hooks or not any([args.check_hooks, args.find_mx, args.find_hangs, args.find_colors]):
        all_diags.extend(result.hook_violations)

    if args.find_mx or not any([args.check_hooks, args.find_mx, args.find_hangs, args.find_colors]):
        all_diags.extend(result.mx_mismatches)

    if args.find_hangs or not any([args.check_hooks, args.find_mx, args.find_hangs, args.find_colors]):
        all_diags.extend(result.potential_hangs)

    if args.find_colors or not any([args.check_hooks, args.find_mx, args.find_hangs, args.find_colors]):
        all_diags.extend(result.color_issues)

    # Add change detection if baseline provided
    if args.baseline:
        changes = scan_for_recent_changes(args.rom, args.baseline)
        all_diags.extend(changes)

    # Output
    if args.json:
        output = {
            'rom': str(args.rom),
            'diagnostics': all_diags,
            'summary': {
                'hook_violations': len(result.hook_violations),
                'mx_mismatches': len(result.mx_mismatches),
                'potential_hangs': len(result.potential_hangs),
                'color_issues': len(result.color_issues),
            }
        }
        print(json.dumps(output, indent=2))
    else:
        print(f"\nOracle of Secrets Static Analysis")
        print("=" * 60)
        print(f"ROM: {args.rom}")
        print(f"Hooks checked: {len(KNOWN_HOOKS)}")
        print()

        # Group by severity
        errors = [d for d in all_diags if d.get('severity') == 'error']
        warnings = [d for d in all_diags if d.get('severity') == 'warning']
        infos = [d for d in all_diags if d.get('severity') == 'info']

        if errors:
            print(f"ERRORS ({len(errors)}):")
            print("-" * 60)
            for d in errors:
                addr = d.get('address', 0)
                print(f"  [ERROR] ${addr:06X}: {d['message']}")
                if args.verbose and 'context' in d:
                    for k, v in d['context'].items():
                        print(f"           {k}: {v}")
            print()

        if warnings:
            print(f"WARNINGS ({len(warnings)}):")
            print("-" * 60)
            for d in warnings:
                addr = d.get('address', 0)
                print(f"  [WARN] ${addr:06X}: {d['message']}")
                if args.verbose and 'context' in d:
                    for k, v in d['context'].items():
                        print(f"          {k}: {v}")
            print()

        if infos and args.verbose:
            print(f"INFO ({len(infos)}):")
            print("-" * 60)
            for d in infos:
                addr = d.get('address', 0)
                print(f"  [INFO] ${addr:06X}: {d['message']}")
            print()

        if not all_diags:
            print("No issues found!")

        # Summary
        print("=" * 60)
        print(f"Total: {len(errors)} errors, {len(warnings)} warnings, {len(infos)} info")

        if errors:
            return 1

    return 0


if __name__ == '__main__':
    sys.exit(main())
