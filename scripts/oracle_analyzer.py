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
import re
import subprocess
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

# Import from static_analyzer
from static_analyzer import (
    RegisterState, HookInfo, AnalysisDiagnostic, AnalysisResult,
    StateTracker, parse_sym_file, snes_to_pc, pc_to_snes
)

LONG_ENTRY_RE = re.compile(r'(FullLongEntry|_LongEntry|_Long)$')


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

    # Jump table dispatch: PLY pops 1 byte when X=8-bit, 2 when X=16-bit.
    # Caller and routine must agree so push/pull match; Oracle convention is X=8-bit here.
    (0x008781, "JumpTableLocal", None, True,
     "Stack table dispatch - REQUIRES X=8-bit for correct PLY (1-byte pull)"),

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

# Known cross-module hook targets that intentionally live outside the hook module.
# Keep this list explicit and small; new entries should be justified in code review.
HOOK_TARGET_OWNER_ALLOWLIST = {
    "Graphics_Transfer",
    "PutRollerBeneathLink",
    "DontTeleportWithoutFlippers",
    "Palette_ArmorAndGloves",
}

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
    hooks: list[tuple]
    hook_violations: list[dict]
    mx_mismatches: list[dict]
    potential_hangs: list[dict]
    color_issues: list[dict]
    hook_state_drift: list[dict]
    hook_abi_issues: list[dict]
    width_stack_imbalances: list[dict] = field(default_factory=list)
    sprite_table_overflow: list[dict] = field(default_factory=list)
    hook_target_owner_issues: list[dict] = field(default_factory=list)
    phb_plb_issues: list[dict] = field(default_factory=list)
    jsl_target_issues: list[dict] = field(default_factory=list)
    rtl_rts_issues: list[dict] = field(default_factory=list)

    def all_diagnostics(self):
        return (
            self.base_result.diagnostics +
            [AnalysisDiagnostic(**h) for h in self.hook_violations] +
            [AnalysisDiagnostic(**m) for m in self.mx_mismatches] +
            [AnalysisDiagnostic(**p) for p in self.potential_hangs] +
            [AnalysisDiagnostic(**c) for c in self.color_issues] +
            [AnalysisDiagnostic(**d) for d in self.hook_state_drift] +
            [AnalysisDiagnostic(**a) for a in self.hook_abi_issues] +
            [AnalysisDiagnostic(**w) for w in self.width_stack_imbalances] +
            [AnalysisDiagnostic(**s) for s in self.sprite_table_overflow] +
            [AnalysisDiagnostic(**h) for h in self.hook_target_owner_issues] +
            [AnalysisDiagnostic(**p) for p in self.phb_plb_issues] +
            [AnalysisDiagnostic(**j) for j in self.jsl_target_issues] +
            [AnalysisDiagnostic(**r) for r in self.rtl_rts_issues]
        )


def load_hooks_json(path: Path) -> tuple[list[tuple], dict[int, RegisterState], dict[int, RegisterState], dict[int, dict]]:
    """Load hooks from hooks.json file."""
    hooks_list = []
    state_expectations = {}
    exit_expectations = {}
    hook_meta = {}

    data_label_re = re.compile(
        r'^(Pool_|RoomData|RoomDataTiles|RoomDataObjects|OverworldMap|DungeonMap|'
        r'Map16|Map32|Tile16|Tile32|Gfx|GFX|Pal|Palette|BG|OAM|Msg|Text|Font|'
        r'Sfx|Sound|Table|Tables|Data|Buffer|Lookup|LUT|Offset|Offsets|Index|'
        r'Indices|Pointer|Pointers|Ptrs|Ptr|List|Lists|Array|Arrays|Tiles|Tilemap|'
        r'TileMap|Map|Maps)'
    )
    data_label_suffixes = (
        '_data', '_table', '_tables', '_tiles', '_tilemap', '_map', '_maps',
        '_gfx', '_pal', '_palettes', '_pointers', '_ptrs', '_ptr', '_lut',
    )

    def _is_data_label(label: Optional[str]) -> bool:
        if not label:
            return False
        if label.startswith('$'):
            return False
        if data_label_re.match(label):
            return True
        lower = label.lower()
        if lower.startswith('oracle_pos') and re.search(r'pos\d_', lower):
            return True
        for suffix in data_label_suffixes:
            if lower.endswith(suffix):
                return True
        return False

    def _coerce_width(value: Optional[object]) -> Optional[int]:
        if value is None:
            return None
        if isinstance(value, str):
            try:
                return int(value, 0)
            except ValueError:
                return None
        if isinstance(value, bool):
            return 8 if value else 16
        if isinstance(value, (int, float)):
            return int(value)
        return None

    if not path.exists():
        return hooks_list, state_expectations, exit_expectations, hook_meta

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
        exp_m = _coerce_width(hook.get('expected_m'))
        exp_x = _coerce_width(hook.get('expected_x'))
        exp_exit_m = _coerce_width(hook.get('expected_exit_m'))
        exp_exit_x = _coerce_width(hook.get('expected_exit_x'))
        note = hook.get('note', '')

        hooks_list.append((addr, name, exp_m == 8 if exp_m else None, exp_x == 8 if exp_x else None, note))
        hook_meta[addr] = {
            'kind': hook.get('kind', 'patch'),
            'module': hook.get('module', ''),
            'skip_abi': bool(hook.get('skip_abi')) or _is_data_label(name) or _is_data_label(hook.get('target')),
            'name': name,
            'abi_class': hook.get('abi_class', ''),
            'target': hook.get('target'),
            'source': hook.get('source', ''),
        }

        if exp_m is not None or exp_x is not None:
            state_expectations[addr] = RegisterState(
                m_flag=exp_m == 8 if exp_m else None,
                x_flag=exp_x == 8 if exp_x else None
            )
        if exp_exit_m is not None or exp_exit_x is not None:
            exit_expectations[addr] = RegisterState(
                m_flag=exp_exit_m == 8 if exp_exit_m else None,
                x_flag=exp_exit_x == 8 if exp_exit_x else None
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

        exp_m = _coerce_width(addr_info.get('expected_m'))
        exp_x = _coerce_width(addr_info.get('expected_x'))
        exp_exit_m = _coerce_width(addr_info.get('expected_exit_m'))
        exp_exit_x = _coerce_width(addr_info.get('expected_exit_x'))

        if exp_m is not None or exp_x is not None:
            state_expectations[addr] = RegisterState(
                m_flag=exp_m == 8 if exp_m else None,
                x_flag=exp_x == 8 if exp_x else None
            )
        if exp_exit_m is not None or exp_exit_x is not None:
            exit_expectations[addr] = RegisterState(
                m_flag=exp_exit_m == 8 if exp_exit_m else None,
                x_flag=exp_exit_x == 8 if exp_exit_x else None
            )

    return hooks_list, state_expectations, exit_expectations, hook_meta


def parse_wla_sym_source_map(path: Path) -> tuple[dict[int, str], dict[int, set[int]]]:
    """Parse [source files] and [addr-to-line mapping] sections from a WLA .sym file."""
    module_to_file: dict[int, str] = {}
    addr_to_modules: dict[int, set[int]] = defaultdict(set)

    if not path.exists():
        return module_to_file, addr_to_modules

    section = ""
    with path.open(encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith(";"):
                continue
            if line.startswith("[") and line.endswith("]"):
                section = line.lower()
                continue

            if section == "[source files]":
                m = re.match(r"^([0-9A-Fa-f]{4})\s+[0-9A-Fa-f]{8}\s+(.+)$", line)
                if not m:
                    continue
                module_id = int(m.group(1), 16)
                module_to_file[module_id] = m.group(2).strip()
                continue

            if section == "[addr-to-line mapping]":
                m = re.match(
                    r"^([0-9A-Fa-f]{2}):([0-9A-Fa-f]{4})\s+([0-9A-Fa-f]{4}):([0-9A-Fa-f]{8})$",
                    line,
                )
                if not m:
                    continue
                bank = int(m.group(1), 16)
                offs = int(m.group(2), 16)
                module_id = int(m.group(3), 16)
                addr = (bank << 16) | offs
                addr_to_modules[addr].add(module_id)

    return module_to_file, dict(addr_to_modules)


def _parse_hook_address(addr_raw: object) -> Optional[int]:
    if addr_raw is None:
        return None
    addr_s = str(addr_raw).strip()
    if not addr_s:
        return None
    try:
        if addr_s.startswith("0x"):
            return int(addr_s, 16)
        if addr_s.startswith("$"):
            return int(addr_s[1:], 16)
        return int(addr_s, 0)
    except ValueError:
        return None


def _build_name_to_addrs(labels: dict[int, str]) -> dict[str, set[int]]:
    name_to_addrs: dict[str, set[int]] = defaultdict(set)
    for addr, label in labels.items():
        variants = {label, label.lower()}
        if label.startswith("Oracle_"):
            stripped = label[len("Oracle_"):]
            variants.add(stripped)
            variants.add(stripped.lower())
        for name in variants:
            name_to_addrs[name].add(addr)
    return dict(name_to_addrs)


def _is_numeric_target_name(target: str) -> bool:
    if not target:
        return False
    if target.startswith("$") or target.startswith("0x"):
        return True
    try:
        int(target, 0)
        return True
    except ValueError:
        return False


def check_hook_target_ownership(
    hooks_path: Optional[Path],
    sym_path: Optional[Path],
    labels: dict[int, str],
) -> list[dict]:
    """Validate that hook target routines resolve into expected module ownership.

    This catches silent overwrite scenarios where a hook still resolves symbolically
    but the bytes at the target address are owned by an unrelated source module.
    """
    if hooks_path is None or sym_path is None or not hooks_path.exists() or not sym_path.exists() or not labels:
        return []

    module_to_file, addr_to_modules = parse_wla_sym_source_map(sym_path)
    if not module_to_file or not addr_to_modules:
        return []

    name_to_addrs = _build_name_to_addrs(labels)
    issues: list[dict] = []
    seen: set[tuple[int, str, str]] = set()

    with hooks_path.open(encoding="utf-8", errors="replace") as f:
        hooks_data = json.load(f)

    for hook in hooks_data.get("hooks", []):
        kind = str(hook.get("kind", "")).lower()
        if kind not in {"jsl", "jsr", "jml", "jmp"}:
            continue

        hook_name = str(hook.get("name", ""))
        if hook_name in HOOK_TARGET_OWNER_ALLOWLIST:
            continue

        target = str(hook.get("target", "")).strip()
        if not target or target.startswith(".") or _is_numeric_target_name(target):
            continue

        module = str(hook.get("module", "")).strip()
        if not module:
            continue

        hook_addr = _parse_hook_address(hook.get("address"))
        if hook_addr is None:
            continue

        target_addrs = name_to_addrs.get(target) or name_to_addrs.get(target.lower())
        if not target_addrs:
            continue

        owner_roots: set[str] = set()
        owner_files: set[str] = set()
        resolved_addrs: list[int] = []

        for addr in sorted(target_addrs):
            module_ids = addr_to_modules.get(addr)
            if not module_ids:
                continue
            resolved_addrs.append(addr)
            for module_id in module_ids:
                source_file = module_to_file.get(module_id)
                if not source_file:
                    continue
                owner_files.add(source_file)
                owner_roots.add(source_file.split("/", 1)[0])

        if not resolved_addrs:
            continue

        if module in owner_roots:
            continue

        key = (hook_addr, hook_name, target)
        if key in seen:
            continue
        seen.add(key)

        hook_label = labels.get(hook_addr, f"${hook_addr:06X}")
        issues.append({
            "severity": "error",
            "message": (
                f"Hook target ownership mismatch at {hook_label}: "
                f"target '{target}' resolves outside module '{module}' "
                f"(owners: {', '.join(sorted(owner_roots))})"
            ),
            "address": hook_addr,
            "context": {
                "hook_name": hook_name,
                "hook_kind": kind,
                "hook_module": module,
                "target": target,
                "resolved_target_addrs": [f"${a:06X}" for a in resolved_addrs[:8]],
                "owner_files": sorted(owner_files)[:8],
                "allowlist_hint": "If this cross-module target is intentional, add the hook name to HOOK_TARGET_OWNER_ALLOWLIST.",
            },
        })

    return issues


def _read_vector(rom_data: bytes, vector_addr: int, mapper: str = 'lorom') -> Optional[int]:
    """Read a 16-bit vector and return a SNES address in bank $00."""
    pc = snes_to_pc(vector_addr, mapper)
    if pc < 0 or pc + 1 >= len(rom_data):
        return None
    lo = rom_data[pc]
    hi = rom_data[pc + 1]
    target = lo | (hi << 8)
    if target == 0x0000:
        return None
    return target  # Bank $00


def analyze_from_vectors(rom_data: bytes, labels: dict[int, str], mapper: str = 'lorom') -> StateTracker:
    """Analyze ROM starting from reset/NMI/IRQ vectors (bank $00)."""
    tracker = StateTracker(rom_data, mapper)
    tracker.labels = labels

    # Emulation mode vectors in bank $00
    vector_addrs = (0x00FFFC, 0x00FFFA, 0x00FFEE)  # RESET, NMI, IRQ/BRK
    entry_points = []
    for vec in vector_addrs:
        target = _read_vector(rom_data, vec, mapper)
        if target is not None:
            entry_points.append(target)

    # Fallback if vectors are missing/unreadable
    if not entry_points:
        entry_points = [0x008000, 0x0080C9]  # Reset, NMI (ALTTP convention)

    for entry in entry_points:
        tracker.analyze_from(entry, RegisterState(m_flag=True, x_flag=True))

    tracker.finalize_call_graph()
    return tracker


def compare_hook_entry_states(
    patched_rom: bytes,
    baseline_rom: bytes,
    hooks: list[tuple],
    labels: dict[int, str],
    mapper: str = 'lorom'
) -> list[dict]:
    """Compare entry state at hook addresses between baseline and patched ROMs."""
    drift = []
    base_tracker = analyze_from_vectors(baseline_rom, labels, mapper)
    patch_tracker = analyze_from_vectors(patched_rom, labels, mapper)

    for addr, name, _exp_m, _exp_x, _note in hooks:
        base_state = base_tracker.visited.get(addr)
        patch_state = patch_tracker.visited.get(addr)

        if base_state is None or patch_state is None:
            continue

        issues = []
        if base_state.m_flag is not None and patch_state.m_flag is not None:
            if base_state.m_flag != patch_state.m_flag:
                issues.append("M flag drift")

        if base_state.x_flag is not None and patch_state.x_flag is not None:
            if base_state.x_flag != patch_state.x_flag:
                issues.append("X flag drift")

        if issues:
            label = labels.get(addr, f"${addr:06X}")
            drift.append({
                'severity': 'warning',
                'message': f"Hook state drift at {label} ({name}): " + ", ".join(issues),
                'address': addr,
                'context': {
                    'baseline_state': str(base_state),
                    'patched_state': str(patch_state),
                }
            })

    return drift


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
    exit_expectations: dict[int, RegisterState] = {}
    hook_meta: dict[int, dict] = {}

    if hooks_path and hooks_path.exists():
        json_hooks, json_expectations, json_exit_expectations, json_meta = load_hooks_json(hooks_path)
        if json_hooks:
            hooks_to_analyze = json_hooks
        state_expectations.update(json_expectations)
        exit_expectations.update(json_exit_expectations)
        hook_meta.update(json_meta)

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
        return_states=tracker.return_states,
    )

    # Oracle-specific checks
    hook_violations = check_hook_states(tracker, labels, state_expectations, hook_meta)
    mx_mismatches = find_mx_mismatches(rom_data, tracker, labels, hook_meta)
    potential_hangs = find_hang_points(rom_data, tracker, labels)
    color_issues = find_color_issues(rom_data, tracker, labels)
    hook_state_drift = []
    hook_abi_issues = check_hook_abi(tracker, labels, hooks_to_analyze, exit_expectations, hook_meta)
    width_stack_imbalances = find_width_dependent_stack_imbalance(rom_data, tracker, labels, hook_meta)
    sprite_table_overflow = find_sprite_table_overflow(rom_data)
    hook_target_owner_issues = check_hook_target_ownership(hooks_path, sym_path, labels)
    phb_plb_issues = check_phb_plb_pairing(rom_data, tracker, labels)
    jsl_target_issues = check_jsl_targets(rom_data, labels)
    rtl_rts_issues = check_rtl_rts_consistency(tracker, labels)

    return OracleAnalysisResult(
        base_result=base_result,
        hooks=hooks_to_analyze,
        hook_violations=hook_violations,
        mx_mismatches=mx_mismatches,
        potential_hangs=potential_hangs,
        color_issues=color_issues,
        hook_state_drift=hook_state_drift,
        hook_abi_issues=hook_abi_issues,
        width_stack_imbalances=width_stack_imbalances,
        sprite_table_overflow=sprite_table_overflow,
        hook_target_owner_issues=hook_target_owner_issues,
        phb_plb_issues=phb_plb_issues,
        jsl_target_issues=jsl_target_issues,
        rtl_rts_issues=rtl_rts_issues,
    )


def check_hook_states(
    tracker: StateTracker,
    labels: dict[int, str],
    state_expectations: dict[int, RegisterState],
    hook_meta: dict[int, dict] | None = None,
) -> list[dict]:
    """Check that hooks are entered with correct state."""
    violations = []

    for addr, expected_state in state_expectations.items():
        meta = (hook_meta or {}).get(addr, {})
        if meta.get('skip_abi'):
            continue
        # Skip data/patch hooks unless skip_abi is explicitly False
        if meta.get('kind') in ('data', 'jmp', 'jml'):
            continue
        if meta.get('kind') == 'patch' and meta.get('skip_abi') is not False:
            continue
        name = meta.get('name', '')
        if name.startswith('hook_') or name.startswith('.') or name.startswith('$'):
            continue
        label = labels.get(addr, f"${addr:06X}")

        # Prefer validating actual call sites into the hook when available.
        call_refs = [r for r in tracker.cross_refs
                     if r.to_addr == addr and r.kind in ('jsl', 'jsr')]
        if call_refs:
            for ref in call_refs:
                caller_state = tracker.visited.get(ref.from_addr)
                if not caller_state:
                    continue
                issues = []
                if expected_state.m_flag is not None and caller_state.m_flag is not None:
                    if expected_state.m_flag != caller_state.m_flag:
                        exp = '8-bit' if expected_state.m_flag else '16-bit'
                        act = '8-bit' if caller_state.m_flag else '16-bit'
                        issues.append(f"M flag: expected {exp}, got {act}")

                if expected_state.x_flag is not None and caller_state.x_flag is not None:
                    if expected_state.x_flag != caller_state.x_flag:
                        exp = '8-bit' if expected_state.x_flag else '16-bit'
                        act = '8-bit' if caller_state.x_flag else '16-bit'
                        issues.append(f"X flag: expected {exp}, got {act}")

                if issues:
                    src_label = labels.get(ref.from_addr, f"${ref.from_addr:06X}")
                    violations.append({
                        'severity': 'error',
                        'message': f"Hook entry state mismatch at {label} (call from {src_label}): "
                                   + "; ".join(issues),
                        'address': ref.from_addr,
                        'context': {
                            'hook': label,
                            'expected': str(expected_state),
                            'actual': str(caller_state),
                        }
                    })
            continue

        # Fallback: use the tracked state at the hook entry.
        if addr not in tracker.visited:
            continue

        actual_state = tracker.visited[addr]
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


def check_hook_abi(
    tracker: StateTracker,
    labels: dict[int, str],
    hooks: list[tuple],
    exit_expectations: dict[int, RegisterState],
    hook_meta: dict[int, dict] | None = None,
) -> list[dict]:
    """Validate hook ABI expectations at return points."""
    issues = []

    for addr, name, _exp_m, _exp_x, _note in hooks:
        meta = (hook_meta or {}).get(addr, {})
        if meta.get('skip_abi') or meta.get('kind') in ('data', 'patch', 'jmp', 'jml'):
            continue
        name = meta.get('name', '')
        if name.startswith('hook_') or name.startswith('.') or name.startswith('$'):
            continue
        label = labels.get(addr, f"${addr:06X}")
        return_states = tracker.return_states.get(addr, [])
        if not return_states:
            issues.append({
                'severity': 'warning',
                'message': f"Hook '{name}' has no recorded return states (ABI exit not verified)",
                'address': addr,
                'context': {'hook': label},
            })
            continue

        expected_exit = exit_expectations.get(addr)

        for ret_addr, ret_state in return_states:
            if ret_state.m_flag is None:
                issues.append({
                    'severity': 'warning',
                    'message': f"Hook '{name}' exit M flag unknown at ${ret_addr:06X}",
                    'address': ret_addr,
                    'context': {'hook': label, 'state': str(ret_state)},
                })
            if ret_state.x_flag is None:
                issues.append({
                    'severity': 'warning',
                    'message': f"Hook '{name}' exit X flag unknown at ${ret_addr:06X}",
                    'address': ret_addr,
                    'context': {'hook': label, 'state': str(ret_state)},
                })

            if expected_exit:
                exit_issues = []
                if expected_exit.m_flag is not None and ret_state.m_flag is not None:
                    if expected_exit.m_flag != ret_state.m_flag:
                        exp = '8-bit' if expected_exit.m_flag else '16-bit'
                        act = '8-bit' if ret_state.m_flag else '16-bit'
                        exit_issues.append(f"M flag: expected {exp}, got {act}")
                if expected_exit.x_flag is not None and ret_state.x_flag is not None:
                    if expected_exit.x_flag != ret_state.x_flag:
                        exp = '8-bit' if expected_exit.x_flag else '16-bit'
                        act = '8-bit' if ret_state.x_flag else '16-bit'
                        exit_issues.append(f"X flag: expected {exp}, got {act}")
                if exit_issues:
                    issues.append({
                        'severity': 'error',
                        'message': f"Hook '{name}' exit state mismatch at ${ret_addr:06X}: " + "; ".join(exit_issues),
                        'address': ret_addr,
                        'context': {
                            'hook': label,
                            'expected': str(expected_exit),
                            'actual': str(ret_state),
                        }
                    })

            # ABI push/pull pairing checks for DB/DP/P
            local_ops = [op for op in ret_state.stack_ops
                         if op.opcode not in ('JSR', 'JSL', 'RTS', 'RTL', 'RTI')]
            if local_ops:
                counts = {}
                for op in local_ops:
                    counts[op.opcode] = counts.get(op.opcode, 0) + 1
                pairs = [
                    ("PHB", "PLB"),
                    ("PHD", "PLD"),
                    ("PHP", "PLP"),
                ]
                for push, pull in pairs:
                    pushes = counts.get(push, 0)
                    pulls = counts.get(pull, 0)
                    if pushes != pulls:
                        issues.append({
                            'severity': 'warning',
                            'message': (
                                f"Hook '{name}' exit {push}/{pull} imbalance at ${ret_addr:06X}: "
                                f"pushes={pushes}, pulls={pulls}"
                            ),
                            'address': ret_addr,
                            'context': {'hook': label, 'pair': f"{push}/{pull}"},
                        })

    return issues


def find_mx_mismatches(
    rom_data: bytes,
    tracker: StateTracker,
    labels: dict[int, str],
    hook_meta: dict[int, dict] | None = None,
) -> list[dict]:
    """Find potential M/X flag mismatches at JSL call sites."""
    mismatches = []
    hook_meta = hook_meta or {}
    data_label_re = re.compile(
        r'^(Pool_|RoomData|RoomDataTiles|RoomDataObjects|OverworldMap|DungeonMap|'
        r'Map16|Map32|Tile16|Tile32|Gfx|GFX|Pal|Palette|BG|OAM|Msg|Text|Font|'
        r'Sfx|Sound|Table|Tables|Data|Buffer|Lookup|LUT|Offset|Offsets|Index|'
        r'Indices|Pointer|Pointers|Ptrs|Ptr|List|Lists|Array|Arrays|Tiles|Tilemap|'
        r'TileMap|Map|Maps)'
    )
    data_label_suffixes = (
        '_data', '_table', '_tables', '_tiles', '_tilemap', '_map', '_maps',
        '_gfx', '_pal', '_palettes', '_pointers', '_ptrs', '_ptr', '_lut',
    )

    def _is_data_label(label: Optional[str]) -> bool:
        if not label:
            return False
        if label.startswith('$'):
            return False
        if data_label_re.match(label):
            return True
        lower = label.lower()
        if lower.startswith('oracle_pos') and re.search(r'pos\d_', lower):
            return True
        for suffix in data_label_suffixes:
            if lower.endswith(suffix):
                return True
        return False

    # Scan for JSL (0x22) instructions
    for pc in range(len(rom_data) - 3):
        if rom_data[pc] != 0x22:  # JSL
            continue

        src_addr = pc_to_snes(pc, 'lorom')
        target = rom_data[pc + 1] | (rom_data[pc + 2] << 8) | (rom_data[pc + 3] << 16)

        # Skip non-ROM targets and data hooks
        if snes_to_pc(target, 'lorom') < 0:
            continue
        meta = hook_meta.get(target, {})
        if meta.get('skip_abi') or meta.get('kind') == 'data' or meta.get('abi_class') == 'long_entry':
            continue

        # Check if we have state info for both caller and callee
        if src_addr not in tracker.visited or target not in tracker.visited:
            continue

        caller_state = tracker.visited[src_addr]
        callee_state = tracker.visited[target]

        tgt_label = labels.get(target, f"${target:06X}")
        if _is_data_label(tgt_label):
            continue
        if LONG_ENTRY_RE.search(tgt_label):
            continue

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
    # LDA $2140 (AD 40 21, 3B) : CMP #$XX (C9 XX, 2B) : BNE rel (D0 XX, 2B)
    for pc in range(len(rom_data) - 7):
        if (rom_data[pc] == 0xAD and          # LDA abs
            rom_data[pc+1] == 0x40 and
            rom_data[pc+2] == 0x21 and        # $2140
            rom_data[pc+4] == 0xC9 and        # CMP #imm (pc+3=C9)
            rom_data[pc+5] == 0xD0):          # BNE opcode
            offset = rom_data[pc+6]           # BNE relative offset
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


def find_width_dependent_stack_imbalance(
    rom_data: bytes,
    tracker: StateTracker,
    labels: dict[int, str],
    hook_meta: dict[int, dict] | None = None,
) -> list[dict]:
    """Find push/pull pairs where M/X width differs between the push and pull.

    This catches the critical bug pattern where:
      - PHY pushes 2 bytes (X=16-bit) but PLY pulls 1 byte (X=8-bit), or vice versa
      - PHA pushes 2 bytes (M=16-bit) but PLA pulls 1 byte (M=8-bit), or vice versa

    These cause stack misalignment that corrupts return addresses.

    Also checks for TCS (opcode 0x1B) reached with A in 16-bit mode holding
    a value outside the valid stack page, which would redirect SP.
    """
    issues = []

    # Width-dependent push/pull pairs: (push_opcode, pull_opcode, flag)
    # flag = 'm' means width depends on M flag, 'x' means X flag
    width_pairs = [
        (0x48, 0x68, 'm', 'PHA', 'PLA'),  # A
        (0xDA, 0xFA, 'x', 'PHX', 'PLX'),  # X
        (0x5A, 0x7A, 'x', 'PHY', 'PLY'),  # Y
    ]

    # Collect all visited addresses with their states, keyed by routine
    # We check within each routine's return paths
    for routine_start, return_list in tracker.return_states.items():
        for ret_addr, ret_state in return_list:
            if not ret_state.stack_ops:
                continue

            # Walk through stack ops looking for width-dependent push/pull pairs
            # where the M/X flag differs between the push and the corresponding pull
            push_stack: dict[str, list[tuple]] = {}  # opcode -> [(addr, m_flag, x_flag)]

            for op in ret_state.stack_ops:
                if op.opcode in ('JSR', 'JSL', 'RTS', 'RTL', 'RTI'):
                    continue

                # Check if this is a width-dependent push
                for push_op, pull_op, flag, push_name, pull_name in width_pairs:
                    if op.opcode == push_name:
                        # Record the state at the push
                        push_state = tracker.visited.get(op.addr)
                        if push_state:
                            if push_name not in push_stack:
                                push_stack[push_name] = []
                            push_stack[push_name].append((
                                op.addr,
                                push_state.m_flag,
                                push_state.x_flag,
                            ))

                    elif op.opcode == pull_name:
                        # Match with last push of same type
                        if push_name in push_stack and push_stack[push_name]:
                            push_addr, push_m, push_x = push_stack[push_name].pop()
                            pull_state = tracker.visited.get(op.addr)
                            if not pull_state:
                                continue

                            # Check if the relevant flag differs
                            if flag == 'm':
                                push_width = push_m
                                pull_width = pull_state.m_flag
                            else:
                                push_width = push_x
                                pull_width = pull_state.x_flag

                            if (push_width is not None and pull_width is not None
                                    and push_width != pull_width):
                                push_label = labels.get(push_addr, f"${push_addr:06X}")
                                pull_label = labels.get(op.addr, f"${op.addr:06X}")
                                routine_label = labels.get(routine_start, f"${routine_start:06X}")

                                push_w = '8-bit' if push_width else '16-bit'
                                pull_w = '8-bit' if pull_width else '16-bit'
                                flag_name = 'M' if flag == 'm' else 'X'
                                push_bytes = 1 if push_width else 2
                                pull_bytes = 1 if pull_width else 2

                                issues.append({
                                    'severity': 'error',
                                    'message': (
                                        f"Width-dependent stack imbalance in {routine_label}: "
                                        f"{push_name} at {push_label} ({flag_name}={push_w}, {push_bytes}B) "
                                        f"vs {pull_name} at {pull_label} ({flag_name}={pull_w}, {pull_bytes}B) "
                                        f"— stack shift of {abs(push_bytes - pull_bytes)} byte(s)"
                                    ),
                                    'address': push_addr,
                                    'context': {
                                        'routine': routine_label,
                                        'push_addr': f"${push_addr:06X}",
                                        'pull_addr': f"${op.addr:06X}",
                                        'push_width': push_w,
                                        'pull_width': pull_w,
                                        'flag': flag_name,
                                        'stack_shift': abs(push_bytes - pull_bytes),
                                    }
                                })

    # Check TCS sites - find TCS (0x1B) in visited code with A potentially corrupt
    for pc in range(len(rom_data)):
        if rom_data[pc] != 0x1B:  # TCS
            continue
        snes_addr = pc_to_snes(pc, 'lorom')
        state = tracker.visited.get(snes_addr)
        if not state:
            continue

        # TCS in 16-bit M mode means A is 16-bit, so the full 16-bit A is loaded to SP
        # This is expected for init code. But if we reach TCS via an Oracle hook path
        # where A might hold garbage, SP gets corrupted.
        if state.m_flag is False:  # 16-bit A
            label = labels.get(snes_addr, f"${snes_addr:06X}")
            # Check if this TCS loads from a known-safe source
            # Safe: LDA #$01FF : TCS (the init pattern)
            if pc >= 3 and rom_data[pc-3] == 0xA9:
                val = rom_data[pc-2] | (rom_data[pc-1] << 8)
                if val == 0x01FF:
                    continue  # Safe - init code
            # Dynamic TCS from memory - flag as potential risk
            if pc >= 3 and rom_data[pc-3] == 0xAD:
                mem_addr = rom_data[pc-2] | (rom_data[pc-1] << 8)
                issues.append({
                    'severity': 'warning',
                    'message': (
                        f"Dynamic TCS at {label}: SP loaded from ${mem_addr:04X} "
                        f"with A in 16-bit mode — verify ${mem_addr:04X} is never corrupted"
                    ),
                    'address': snes_addr,
                    'context': {
                        'source_addr': f"${mem_addr:04X}",
                        'state': str(state),
                    }
                })

    return issues


def find_sprite_table_overflow(rom_data: bytes, mapper: str = 'lorom') -> list[dict]:
    """Check that no sprite property data bleeds past entry $F2 in the vanilla tables.

    The 8 property tables in bank $0D each hold entries for sprite IDs $00-$F2.
    Table base addresses (SNES):
        $0DB080, $0DB173, $0DB266, $0DB359,
        $0DB44C, $0DB53F, $0DB632, $0DB725
    SpritePrep_LoadProperties begins at $0DB818.

    For each table, entries $F3-$FF occupy the 13 bytes immediately before the
    next table's base (or before $0DB818 for Table 8).  If any of those bytes
    differ from the vanilla ROM's pattern, something overflowed.

    Rather than requiring a vanilla baseline, we use a simpler sentinel: the
    first 8 bytes of SpritePrep_LoadProperties at $0DB818 must match the known
    vanilla opcodes.  If they have been overwritten, Table 8 overflowed.

    We also check each inter-table gap ($F3-$FF range) for non-zero bytes that
    would indicate a property write beyond the valid range.
    """
    issues = []

    TABLE_BASES = [
        0x0DB080, 0x0DB173, 0x0DB266, 0x0DB359,
        0x0DB44C, 0x0DB53F, 0x0DB632, 0x0DB725,
    ]
    TABLE_NAMES = [
        "OAM/Harmless", "HP", "Damage", "Data",
        "Hitbox", "Fall", "Prize", "Deflect",
    ]
    MAX_VALID_ID = 0xF2
    ENTRIES_PER_TABLE = 0xF3  # IDs $00-$F2
    LOAD_PROPERTIES_ADDR = 0x0DB818

    # Known first 8 bytes of SpritePrep_LoadProperties (vanilla)
    # PHB : PHK : PLB : LDA.w $0E20,X ...
    # 8B 4B AB BD 20 0E C9 00
    LOAD_PROPERTIES_SENTINEL = bytes([0x8B, 0x4B, 0xAB, 0xBD, 0x20, 0x0E, 0xC9, 0x00])

    # Check sentinel: has Table 8 overflowed into LoadProperties?
    sentinel_pc = snes_to_pc(LOAD_PROPERTIES_ADDR, mapper)
    if 0 <= sentinel_pc < len(rom_data) - len(LOAD_PROPERTIES_SENTINEL):
        actual = rom_data[sentinel_pc:sentinel_pc + len(LOAD_PROPERTIES_SENTINEL)]
        if actual != LOAD_PROPERTIES_SENTINEL:
            issues.append({
                'severity': 'error',
                'message': (
                    f"SpritePrep_LoadProperties at ${LOAD_PROPERTIES_ADDR:06X} has been "
                    f"overwritten — sprite property Table 8 (Deflect) overflowed past ID $F2"
                ),
                'address': LOAD_PROPERTIES_ADDR,
                'context': {
                    'expected': ' '.join(f'{b:02X}' for b in LOAD_PROPERTIES_SENTINEL),
                    'actual': ' '.join(f'{b:02X}' for b in actual),
                }
            })

    # Check each table's overflow zone ($F3-$FF)
    for i, (base, name) in enumerate(zip(TABLE_BASES, TABLE_NAMES)):
        overflow_start = base + ENTRIES_PER_TABLE  # First byte past $F2
        overflow_end = base + 0x100  # Full 256 entries would end here

        # Clamp to next table or LoadProperties
        if i + 1 < len(TABLE_BASES):
            boundary = TABLE_BASES[i + 1]
        else:
            boundary = LOAD_PROPERTIES_ADDR
        overflow_end = min(overflow_end, boundary)

        if overflow_start >= overflow_end:
            continue

        for snes_addr in range(overflow_start, overflow_end):
            pc = snes_to_pc(snes_addr, mapper)
            if pc < 0 or pc >= len(rom_data):
                continue
            byte_val = rom_data[pc]
            if byte_val != 0x00:
                sprite_id = snes_addr - base
                issues.append({
                    'severity': 'error',
                    'message': (
                        f"Sprite table overflow: Table {i+1} ({name}) at "
                        f"${snes_addr:06X} has value ${byte_val:02X} "
                        f"(sprite ID ${sprite_id:02X} exceeds $F2 limit)"
                    ),
                    'address': snes_addr,
                    'context': {
                        'table': name,
                        'table_index': i + 1,
                        'sprite_id': f'${sprite_id:02X}',
                        'value': f'${byte_val:02X}',
                    }
                })

    # Also check the vanilla pointer tables for overflow
    POINTER_TABLES = [
        (0x069283, "Sprite Main Pointer"),
        (0x06865B, "Sprite Prep Pointer"),
    ]
    for ptr_base, ptr_name in POINTER_TABLES:
        for sprite_id in range(MAX_VALID_ID + 1, 0x100):
            snes_addr = ptr_base + (sprite_id * 2)
            pc = snes_to_pc(snes_addr, mapper)
            if pc < 0 or pc + 1 >= len(rom_data):
                continue
            lo = rom_data[pc]
            hi = rom_data[pc + 1]
            word = lo | (hi << 8)
            if word != 0x0000:
                issues.append({
                    'severity': 'error',
                    'message': (
                        f"Sprite pointer overflow: {ptr_name} table at "
                        f"${snes_addr:06X} has value ${word:04X} "
                        f"(sprite ID ${sprite_id:02X} exceeds $F2 limit)"
                    ),
                    'address': snes_addr,
                    'context': {
                        'table': ptr_name,
                        'sprite_id': f'${sprite_id:02X}',
                        'value': f'${word:04X}',
                    }
                })

    return issues


def check_phb_plb_pairing(
    rom_data: bytes,
    tracker: StateTracker,
    labels: dict[int, str],
) -> list[dict]:
    """Check PHB/PLB pairing within routines.

    Scans for PHB (0x8B) and PLB (0xAB) opcodes. Within each routine
    (identified by RTL/RTS boundaries in the tracker's return_states),
    verify that PHB count matches PLB count. Report errors for imbalances.
    """
    issues = []

    for routine_start, return_list in tracker.return_states.items():
        for ret_addr, ret_state in return_list:
            if not ret_state.stack_ops:
                continue

            # Count PHB and PLB in this routine's stack ops (skip call/return ops)
            phb_count = 0
            plb_count = 0
            for op in ret_state.stack_ops:
                if op.opcode == 'PHB':
                    phb_count += 1
                elif op.opcode == 'PLB':
                    plb_count += 1

            if phb_count != plb_count:
                routine_label = labels.get(routine_start, f"${routine_start:06X}")
                issues.append({
                    'severity': 'error',
                    'message': (
                        f"PHB/PLB imbalance in {routine_label} "
                        f"(return at ${ret_addr:06X}): "
                        f"PHB={phb_count}, PLB={plb_count}"
                    ),
                    'address': routine_start,
                    'context': {
                        'routine': routine_label,
                        'return_addr': f"${ret_addr:06X}",
                        'phb_count': phb_count,
                        'plb_count': plb_count,
                    }
                })

    return issues


def check_jsl_targets(
    rom_data: bytes,
    labels: dict[int, str],
) -> list[dict]:
    """Validate JSL targets that point into Oracle banks.

    For each JSL instruction (0x22) in the ROM that targets an Oracle bank
    (banks $30-$3F based on the LoROM mapping), verify the target address
    exists in the symbol table (labels dict). Report warnings for JSL
    targets that are not in the symbol table (potential typos / dead code).
    """
    if not labels:
        # No symbols available; skip to avoid warning spam on every JSL.
        return []

    issues = []
    # Oracle code lives in banks $30-$3F in LoROM
    oracle_bank_lo = 0x30
    oracle_bank_hi = 0x3F

    for pc in range(len(rom_data) - 3):
        if rom_data[pc] != 0x22:  # JSL
            continue

        target = rom_data[pc + 1] | (rom_data[pc + 2] << 8) | (rom_data[pc + 3] << 16)
        target_bank = (target >> 16) & 0xFF

        # Only check targets in Oracle banks
        if target_bank < oracle_bank_lo or target_bank > oracle_bank_hi:
            continue

        # Check if the target is known in the symbol table
        if target not in labels:
            src_addr = pc_to_snes(pc, 'lorom')
            src_label = labels.get(src_addr, f"${src_addr:06X}")
            issues.append({
                'severity': 'warning',
                'message': (
                    f"JSL at {src_label} targets ${target:06X} "
                    f"(Oracle bank ${target_bank:02X}) "
                    f"but no symbol found — possible typo or dead code"
                ),
                'address': src_addr,
                'context': {
                    'target': f"${target:06X}",
                    'target_bank': f"${target_bank:02X}",
                    'source': src_label,
                }
            })

    return issues


def check_rtl_rts_consistency(
    tracker: StateTracker,
    labels: dict[int, str],
) -> list[dict]:
    """Check RTL vs RTS consistency against call type.

    Using the tracker's cross_refs, find routines that are called via JSL
    but terminate with RTS (or called via JSR but terminate with RTL).
    Wrong return instruction corrupts the return address.
    """
    issues = []

    # Build a map: routine_addr -> set of call kinds ('jsl', 'jsr')
    call_kinds: dict[int, set[str]] = {}
    for ref in tracker.cross_refs:
        if ref.kind in ('jsl', 'jsr'):
            if ref.to_addr not in call_kinds:
                call_kinds[ref.to_addr] = set()
            call_kinds[ref.to_addr].add(ref.kind)

    # Build a map: routine_addr -> set of return opcodes
    # return_states maps routine_start -> list[(ret_addr, ret_state)]
    # We need to check the actual opcode at ret_addr
    for routine_addr, return_list in tracker.return_states.items():
        kinds = call_kinds.get(routine_addr, set())
        if not kinds:
            continue

        for ret_addr, ret_state in return_list:
            # Determine the return opcode from the last stack op
            ret_opcode = None
            if ret_state.stack_ops:
                last_op = ret_state.stack_ops[-1]
                if last_op.opcode in ('RTL', 'RTS', 'RTI'):
                    ret_opcode = last_op.opcode

            if ret_opcode is None:
                continue

            routine_label = labels.get(routine_addr, f"${routine_addr:06X}")

            # JSL call but RTS return: RTS only pops 2 bytes, JSL pushed 3
            if 'jsl' in kinds and ret_opcode == 'RTS':
                # Find a caller address for context
                callers = [r.from_addr for r in tracker.cross_refs
                           if r.to_addr == routine_addr and r.kind == 'jsl']
                caller_label = labels.get(callers[0], f"${callers[0]:06X}") if callers else "unknown"
                issues.append({
                    'severity': 'error',
                    'message': (
                        f"Routine {routine_label} called via JSL "
                        f"(e.g. from {caller_label}) but returns with RTS — "
                        f"return address corruption (3-byte push, 2-byte pull)"
                    ),
                    'address': routine_addr,
                    'context': {
                        'routine': routine_label,
                        'return_addr': f"${ret_addr:06X}",
                        'call_kind': 'jsl',
                        'return_opcode': ret_opcode,
                        'callers': [f"${c:06X}" for c in callers[:5]],
                    }
                })

            # JSR call but RTL return: RTL pops 3 bytes, JSR only pushed 2
            if 'jsr' in kinds and ret_opcode == 'RTL':
                callers = [r.from_addr for r in tracker.cross_refs
                           if r.to_addr == routine_addr and r.kind == 'jsr']
                caller_label = labels.get(callers[0], f"${callers[0]:06X}") if callers else "unknown"
                issues.append({
                    'severity': 'error',
                    'message': (
                        f"Routine {routine_label} called via JSR "
                        f"(e.g. from {caller_label}) but returns with RTL — "
                        f"return address corruption (2-byte push, 3-byte pull)"
                    ),
                    'address': routine_addr,
                    'context': {
                        'routine': routine_label,
                        'return_addr': f"${ret_addr:06X}",
                        'call_kind': 'jsr',
                        'return_opcode': ret_opcode,
                        'callers': [f"${c:06X}" for c in callers[:5]],
                    }
                })

    return issues


def _get_changed_asm_files(project_root: Path) -> list[str]:
    """Get list of ASM files changed since the last commit.

    First tries staged files (git diff --cached), then falls back to
    unstaged changes vs HEAD.

    Returns a list of file paths relative to project_root.
    """
    try:
        # Try staged files first
        result = subprocess.run(
            ['git', 'diff', '--cached', '--name-only', '--', '*.asm'],
            capture_output=True, text=True, cwd=str(project_root),
            timeout=10,
        )
        files = [f.strip() for f in result.stdout.strip().splitlines() if f.strip()]
        if files:
            return files

        # Fall back to unstaged changes vs HEAD
        result = subprocess.run(
            ['git', 'diff', '--name-only', 'HEAD', '--', '*.asm'],
            capture_output=True, text=True, cwd=str(project_root),
            timeout=10,
        )
        files = [f.strip() for f in result.stdout.strip().splitlines() if f.strip()]
        return files
    except (subprocess.TimeoutExpired, FileNotFoundError, OSError):
        return []


def _filter_diags_by_changed_files(
    diags: list[dict],
    changed_files: list[str],
    labels: dict[int, str],
) -> list[dict]:
    """Filter diagnostics to only those whose address maps to a changed file.

    Uses the symbol table to map diagnostic addresses to source file names.
    A diagnostic passes the filter if:
      - Its address has a label that contains a token matching a changed file's
        stem (e.g. label 'Oracle_FishingRod_Throw' matches 'Items/fishing_rod.asm')
      - Or the diagnostic's context contains a 'module' or 'source' key that
        matches a changed file
      - Or we cannot determine provenance (conservative: keep it)
    """
    if not changed_files:
        return []

    # Build a set of lowercase stems and full relative paths for matching
    changed_stems = set()
    changed_paths_lower = set()
    for f in changed_files:
        changed_paths_lower.add(f.lower())
        stem = Path(f).stem.lower()
        changed_stems.add(stem)
        # Also add without underscores for fuzzy matching
        changed_stems.add(stem.replace('_', ''))

    filtered = []
    for diag in diags:
        addr = diag.get('address', 0)
        label = labels.get(addr, '')
        ctx = diag.get('context', {})

        # Check context for module/source fields
        module = ctx.get('module', '')
        source = ctx.get('source', '')

        keep = False

        # Direct module/source match
        for field_val in (module, source):
            if field_val:
                field_lower = field_val.lower()
                if any(cp in field_lower or field_lower in cp for cp in changed_paths_lower):
                    keep = True
                    break

        # Label-based matching: check if any changed file stem appears in the label
        if not keep and label:
            label_lower = label.lower().replace('_', '')
            for stem in changed_stems:
                if stem in label_lower:
                    keep = True
                    break

        # If we cannot determine provenance, keep the diagnostic (conservative)
        if not keep and not label and not module and not source:
            keep = True

        if keep:
            filtered.append(diag)

    return filtered


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
    parser.add_argument('--compare-hooks', action='store_true',
                       help='Compare hook entry states against baseline ROM')
    parser.add_argument('--check-hooks', action='store_true',
                       help='Check hook entry states')
    parser.add_argument('--find-mx', action='store_true',
                       help='Find M/X flag mismatches')
    parser.add_argument('--find-hangs', action='store_true',
                       help='Find potential hang points')
    parser.add_argument('--find-colors', action='store_true',
                       help='Find color/palette issues')
    parser.add_argument('--check-abi', action='store_true',
                       help='Check hook ABI exit state and push/pull balance')
    parser.add_argument('--find-width-imbalance', action='store_true',
                       help='Find width-dependent stack imbalances (PHY/PLY with different X flag)')
    parser.add_argument('--check-sprite-tables', action='store_true',
                       help='Check sprite property tables for overflow past ID $F2')
    parser.add_argument('--check-phb-plb', action='store_true',
                       help='Check PHB/PLB pairing within routines')
    parser.add_argument('--check-jsl-targets', action='store_true',
                       help='Validate JSL targets in Oracle banks against symbol table')
    parser.add_argument('--check-rtl-rts', action='store_true',
                       help='Check RTL vs RTS consistency against call type (JSL vs JSR)')
    parser.add_argument('--strict', action='store_true',
                       help='Treat warnings as errors (exit code 1 if any warnings)')
    parser.add_argument('--diff', action='store_true',
                       help='Only analyze files changed since last commit')
    parser.add_argument('--project-root', type=Path, default=None,
                       help=f'Oracle project root for --diff mode (default: {ORACLE_DIR})')
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

    if args.check_jsl_targets and not result.base_result.labels:
        print(
            "Warning: symbol file missing; skipping JSL target validation. "
            "Provide --sym or generate a .sym file to enable it.",
            file=sys.stderr,
        )

    # Collect diagnostics based on filters
    all_diags = []
    any_filter = any([args.check_hooks, args.find_mx, args.find_hangs,
                      args.find_colors, args.check_abi, args.find_width_imbalance,
                      args.check_sprite_tables, args.check_phb_plb,
                      args.check_jsl_targets, args.check_rtl_rts])

    if args.check_hooks or not any_filter:
        all_diags.extend(result.hook_violations)
        all_diags.extend(result.hook_target_owner_issues)

    if args.find_mx or not any_filter:
        all_diags.extend(result.mx_mismatches)

    if args.find_hangs or not any_filter:
        all_diags.extend(result.potential_hangs)

    if args.find_colors or not any_filter:
        all_diags.extend(result.color_issues)

    if args.check_abi or not any_filter:
        all_diags.extend(result.hook_abi_issues)

    if args.find_width_imbalance or not any_filter:
        all_diags.extend(result.width_stack_imbalances)

    if args.check_sprite_tables or not any_filter:
        all_diags.extend(result.sprite_table_overflow)

    if args.check_phb_plb or not any_filter:
        all_diags.extend(result.phb_plb_issues)

    if args.check_jsl_targets or not any_filter:
        all_diags.extend(result.jsl_target_issues)

    if args.check_rtl_rts or not any_filter:
        all_diags.extend(result.rtl_rts_issues)

    # Add change detection if baseline provided
    if args.baseline:
        changes = scan_for_recent_changes(args.rom, args.baseline)
        all_diags.extend(changes)

        if args.compare_hooks:
            try:
                patched_rom = args.rom.read_bytes()
                baseline_rom = args.baseline.read_bytes()
                # Reuse hooks list from analysis (addresses only)
                hooks_for_compare = result.hooks or KNOWN_HOOKS
                hook_drift = compare_hook_entry_states(
                    patched_rom,
                    baseline_rom,
                    hooks_for_compare,
                    result.base_result.labels,
                )
                result.hook_state_drift = hook_drift
                all_diags.extend(hook_drift)
            except Exception as exc:
                all_diags.append({
                    'severity': 'warning',
                    'message': f"Hook state drift check failed: {exc}",
                    'address': 0,
                })

    # Apply --diff filter: restrict diagnostics to changed files only
    if args.diff:
        diff_project_root = args.project_root or ORACLE_DIR
        changed_files = _get_changed_asm_files(diff_project_root)
        if changed_files:
            print(f"Diff mode: {len(changed_files)} changed ASM file(s)", file=sys.stderr)
            for cf in changed_files:
                print(f"  {cf}", file=sys.stderr)
            all_diags = _filter_diags_by_changed_files(
                all_diags, changed_files, result.base_result.labels
            )
        else:
            print("Diff mode: no changed ASM files found, showing all diagnostics", file=sys.stderr)

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
                'hook_state_drift': len(result.hook_state_drift),
                'hook_abi_issues': len(result.hook_abi_issues),
                'width_stack_imbalances': len(result.width_stack_imbalances),
                'sprite_table_overflow': len(result.sprite_table_overflow),
                'hook_target_owner_issues': len(result.hook_target_owner_issues),
                'phb_plb_issues': len(result.phb_plb_issues),
                'jsl_target_issues': len(result.jsl_target_issues),
                'rtl_rts_issues': len(result.rtl_rts_issues),
            }
        }
        if args.diff:
            output['diff_mode'] = True
            output['changed_files'] = changed_files if args.diff else []
        print(json.dumps(output, indent=2))
    else:
        print(f"\nOracle of Secrets Static Analysis")
        print("=" * 60)
        print(f"ROM: {args.rom}")
        print(f"Hooks checked: {len(result.hooks) or len(KNOWN_HOOKS)}")
        if args.diff:
            print(f"Diff mode: filtering to changed files only")
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
        if args.compare_hooks and args.baseline:
            print(f"Hook state drift warnings: {len(result.hook_state_drift)}")
        if args.check_abi:
            print(f"Hook ABI issues: {len(result.hook_abi_issues)}")
        if args.find_width_imbalance or result.width_stack_imbalances:
            print(f"Width-dependent stack imbalances: {len(result.width_stack_imbalances)}")
        if args.check_sprite_tables or result.sprite_table_overflow:
            print(f"Sprite table overflows: {len(result.sprite_table_overflow)}")
        if args.check_hooks or result.hook_target_owner_issues:
            print(f"Hook target ownership issues: {len(result.hook_target_owner_issues)}")
        if args.check_phb_plb or result.phb_plb_issues:
            print(f"PHB/PLB pairing issues: {len(result.phb_plb_issues)}")
        if args.check_jsl_targets or result.jsl_target_issues:
            print(f"JSL target issues: {len(result.jsl_target_issues)}")
        if args.check_rtl_rts or result.rtl_rts_issues:
            print(f"RTL/RTS consistency issues: {len(result.rtl_rts_issues)}")

        if errors:
            return 1

        # --strict: treat warnings as errors
        if args.strict and warnings:
            print(f"\n--strict: {len(warnings)} warning(s) treated as errors", file=sys.stderr)
            return 1

    # --strict in JSON mode: still exit 1 on warnings
    if args.json and args.strict:
        warnings = [d for d in all_diags if d.get('severity') == 'warning']
        errors = [d for d in all_diags if d.get('severity') == 'error']
        if errors or warnings:
            return 1

    return 0


if __name__ == '__main__':
    sys.exit(main())
