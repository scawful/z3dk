#!/usr/bin/env python3
"""
Z3DK Static Analyzer - Programmatic Bug Detection for 65816 ROMs

Analyzes assembled ROMs to detect:
- M/X flag mismatches at hook boundaries
- Unbalanced stack operations
- Register state violations
- Cross-reference issues

Designed to catch bugs like B010 (soft lock from M/X mismatch).

Usage:
    python3 static_analyzer.py <rom.sfc> [--sym symbols.sym] [--hooks hooks.json]
    python3 static_analyzer.py --help
"""

from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from enum import Enum, auto
from pathlib import Path
from typing import Optional, Set, List, Dict, Tuple


# =============================================================================
# Constants
# =============================================================================

class OpType(Enum):
    """Opcode type classification."""
    NORMAL = auto()
    BRANCH = auto()       # BCC, BCS, BEQ, etc.
    JUMP = auto()         # JMP, JML
    CALL = auto()         # JSR, JSL
    RETURN = auto()       # RTS, RTL, RTI
    FLAG_SET = auto()     # SEP, REP
    FLAG_RESTORE = auto() # PLP, XCE
    STACK_PUSH = auto()   # PHA, PHX, PHY, PHP, etc.
    STACK_PULL = auto()   # PLA, PLX, PLY, PLP, etc.
    INTERRUPT = auto()    # BRK, COP


# Opcodes that affect M/X flags
FLAG_OPCODES = {
    0xC2: 'REP',  # REP #imm - clear bits (16-bit mode)
    0xE2: 'SEP',  # SEP #imm - set bits (8-bit mode)
    0x28: 'PLP',  # PLP - restore P from stack
    0x40: 'RTI',  # RTI - return from interrupt (restores P)
    0xFB: 'XCE',  # XCE - exchange carry and emulation
}

# Opcodes that are calls (need state tracking across)
CALL_OPCODES = {
    0x20: ('JSR', 2),  # JSR abs
    0x22: ('JSL', 3),  # JSL long
    0xFC: ('JSR', 2),  # JSR (abs,X)
}

# Opcodes that are returns
RETURN_OPCODES = {
    0x60: 'RTS',
    0x6B: 'RTL',
    0x40: 'RTI',
}

# Opcodes that are unconditional jumps (flow ends)
JUMP_OPCODES = {
    0x4C: ('JMP', 2),   # JMP abs
    0x5C: ('JML', 3),   # JML long
    0x6C: ('JMP', 2),   # JMP (abs)
    0x7C: ('JMP', 2),   # JMP (abs,X)
    0xDC: ('JML', 2),   # JML [abs]
}

# Opcodes that are branches
BRANCH_OPCODES = {
    0x10: 'BPL', 0x30: 'BMI', 0x50: 'BVC', 0x70: 'BVS',
    0x90: 'BCC', 0xB0: 'BCS', 0xD0: 'BNE', 0xF0: 'BEQ',
    0x80: 'BRA', 0x82: 'BRL',
}

# Stack operations
PUSH_OPCODES = {0x48: 'PHA', 0xDA: 'PHX', 0x5A: 'PHY', 0x08: 'PHP',
                0x8B: 'PHB', 0x0B: 'PHD', 0x4B: 'PHK', 0xF4: 'PEA',
                0xD4: 'PEI', 0x62: 'PER'}
PULL_OPCODES = {0x68: 'PLA', 0xFA: 'PLX', 0x7A: 'PLY', 0x28: 'PLP',
                0xAB: 'PLB', 0x2B: 'PLD'}

# Stack deltas for balance checking (base values - M/X dependent ones adjusted at runtime)
STACK_DELTAS = {
    'PHA': +1,  # Push A (8-bit) or +2 (16-bit M)
    'PHX': +1,  # Push X (8-bit) or +2 (16-bit X)
    'PHY': +1,  # Push Y
    'PHP': +1,  # Push P (always 1 byte)
    'PHB': +1,  # Push data bank
    'PHD': +2,  # Push direct page (16-bit)
    'PHK': +1,  # Push program bank
    'PEA': +2,  # Push effective address
    'PEI': +2,  # Push effective indirect
    'PER': +2,  # Push effective relative
    'PLA': -1,  # Pull A (8-bit) or -2 (16-bit M)
    'PLX': -1,  # Pull X
    'PLY': -1,  # Pull Y
    'PLP': -1,  # Pull P
    'PLB': -1,  # Pull data bank
    'PLD': -2,  # Pull direct page
    'JSR': +2,  # Push return address (16-bit)
    'JSL': +3,  # Push return address + bank
    'RTS': -2,  # Pull return address
    'RTL': -3,  # Pull return address + bank
    'RTI': -3,  # Pull P + PC (native) or -3 (emulation)
}


# =============================================================================
# Data Structures
# =============================================================================

@dataclass
class RegisterState:
    """CPU register state at a point in execution."""
    m_flag: Optional[bool] = None   # True = 8-bit, False = 16-bit, None = unknown
    x_flag: Optional[bool] = None   # True = 8-bit, False = 16-bit, None = unknown
    emulation: bool = False         # Emulation mode (always 8-bit if True)
    stack_depth: int = 0            # Relative stack depth for balance checking
    stack_ops: List[StackOp] = field(default_factory=list)  # Stack operation history

    def m_width(self) -> int:
        """Return A/M register width in bytes."""
        if self.emulation or self.m_flag:
            return 1
        elif self.m_flag is False:
            return 2
        return 1  # Default to 8-bit when unknown

    def x_width(self) -> int:
        """Return X/Y register width in bytes."""
        if self.emulation or self.x_flag:
            return 1
        elif self.x_flag is False:
            return 2
        return 1  # Default to 8-bit when unknown

    def get_stack_delta(self, opcode: str) -> int:
        """Calculate stack delta accounting for register width."""
        base_delta = STACK_DELTAS.get(opcode, 0)

        # PHA/PLA size depends on M flag
        if opcode in ('PHA', 'PLA'):
            multiplier = 2 if self.m_flag is False else 1
            return base_delta * multiplier

        # PHX/PLX/PHY/PLY size depends on X flag
        if opcode in ('PHX', 'PLX', 'PHY', 'PLY'):
            multiplier = 2 if self.x_flag is False else 1
            return base_delta * multiplier

        return base_delta

    def record_stack_op(self, addr: int, opcode: str) -> None:
        """Record a stack operation and update depth."""
        delta = self.get_stack_delta(opcode)
        self.stack_depth += delta
        self.stack_ops.append(StackOp(addr, opcode, delta, self.stack_depth))

    def copy(self) -> 'RegisterState':
        return RegisterState(
            m_flag=self.m_flag,
            x_flag=self.x_flag,
            emulation=self.emulation,
            stack_depth=self.stack_depth,
            stack_ops=list(self.stack_ops)  # Copy the list
        )

    def __repr__(self):
        m = '8' if self.m_flag else ('16' if self.m_flag is False else '?')
        x = '8' if self.x_flag else ('16' if self.x_flag is False else '?')
        return f"M={m} X={x} stk={self.stack_depth}"


@dataclass
class HookInfo:
    """Information about a hook point."""
    name: str
    address: int
    kind: str                          # 'jsl', 'jml', 'patch', etc.
    expected_entry_state: Optional[RegisterState] = None
    expected_exit_state: Optional[RegisterState] = None
    source_file: Optional[str] = None
    source_line: Optional[int] = None


@dataclass
class CrossRef:
    """Cross-reference entry."""
    from_addr: int
    to_addr: int
    kind: str   # 'jsl', 'jsr', 'jmp', 'jml', 'branch'
    from_bank: int = 0
    to_bank: int = 0
    is_tail_call: bool = False  # JMP used as tail call


@dataclass
class StackOp:
    """Record of a stack operation for balance tracking."""
    addr: int
    opcode: str
    delta: int
    cumulative: int  # Stack depth after this operation


class CallGraph:
    """Manages call graph with forward/reverse mappings and analysis."""

    def __init__(self):
        self.forward: Dict[int, List[CrossRef]] = defaultdict(list)  # caller -> [callees]
        self.reverse: Dict[int, List[CrossRef]] = defaultdict(list)  # callee -> [callers]
        self.entry_points: Set[int] = set()  # hooks, vectors
        self.leaf_routines: Set[int] = set()  # no outgoing calls
        self.routine_bounds: Dict[int, Tuple[int, int]] = {}  # addr -> (start, end)

    def add_reference(self, ref: CrossRef) -> None:
        """Add a cross-reference to both forward and reverse maps."""
        # Set bank info from addresses
        ref.from_bank = ref.from_addr >> 16
        ref.to_bank = ref.to_addr >> 16

        self.forward[ref.from_addr].append(ref)
        self.reverse[ref.to_addr].append(ref)

    def add_entry_point(self, addr: int) -> None:
        """Mark an address as an entry point (hook, vector, etc.)."""
        self.entry_points.add(addr)

    def get_callers(self, addr: int) -> List[CrossRef]:
        """Get all references that call/jump to this address."""
        return self.reverse.get(addr, [])

    def get_callees(self, addr: int) -> List[CrossRef]:
        """Get all references from this address."""
        return self.forward.get(addr, [])

    def find_recursive_calls(self) -> List[List[int]]:
        """Find cycles in call graph (potential infinite loops).

        Uses Tarjan's algorithm for strongly connected components.
        Returns list of cycles (each cycle is a list of addresses).
        """
        index_counter = [0]
        stack = []
        lowlinks = {}
        index = {}
        on_stack = {}
        sccs = []

        def strongconnect(v):
            index[v] = index_counter[0]
            lowlinks[v] = index_counter[0]
            index_counter[0] += 1
            stack.append(v)
            on_stack[v] = True

            for ref in self.forward.get(v, []):
                w = ref.to_addr
                if w not in index:
                    strongconnect(w)
                    lowlinks[v] = min(lowlinks[v], lowlinks[w])
                elif on_stack.get(w, False):
                    lowlinks[v] = min(lowlinks[v], index[w])

            if lowlinks[v] == index[v]:
                scc = []
                while True:
                    w = stack.pop()
                    on_stack[w] = False
                    scc.append(w)
                    if w == v:
                        break
                if len(scc) > 1:  # Only report actual cycles
                    sccs.append(scc)

        for v in self.forward.keys():
            if v not in index:
                strongconnect(v)

        return sccs

    def find_cross_bank_calls(self) -> List[CrossRef]:
        """Find JSR/JMP that cross bank boundaries (usually bugs)."""
        cross_bank = []
        for refs in self.forward.values():
            for ref in refs:
                if ref.kind in ('jsr', 'jmp') and ref.from_bank != ref.to_bank:
                    cross_bank.append(ref)
        return cross_bank

    def find_orphan_code(self) -> Set[int]:
        """Find addresses not reachable from any entry point."""
        if not self.entry_points:
            return set()

        reachable = self._bfs_reachable(self.entry_points)
        all_addrs = set(self.forward.keys()) | set(self.reverse.keys())
        return all_addrs - reachable

    def _bfs_reachable(self, start_addrs: Set[int]) -> Set[int]:
        """BFS to find all reachable addresses from starting set."""
        visited = set()
        queue = list(start_addrs)

        while queue:
            addr = queue.pop(0)
            if addr in visited:
                continue
            visited.add(addr)

            for ref in self.forward.get(addr, []):
                if ref.to_addr not in visited:
                    queue.append(ref.to_addr)

        return visited

    def compute_leaf_routines(self) -> None:
        """Identify routines with no outgoing calls (leaf nodes)."""
        # All addresses that are targets but have no outgoing calls
        all_targets = set(self.reverse.keys())
        callers = set(self.forward.keys())
        self.leaf_routines = all_targets - callers

    def export_dict(self) -> Dict:
        """Export call graph as a dictionary for JSON serialization."""
        return {
            'forward': {
                hex(addr): [
                    {
                        'from': hex(ref.from_addr),
                        'to': hex(ref.to_addr),
                        'kind': ref.kind,
                        'cross_bank': ref.from_bank != ref.to_bank,
                    }
                    for ref in refs
                ]
                for addr, refs in self.forward.items()
            },
            'entry_points': [hex(addr) for addr in sorted(self.entry_points)],
            'leaf_routines': [hex(addr) for addr in sorted(self.leaf_routines)],
            'stats': {
                'total_refs': sum(len(refs) for refs in self.forward.values()),
                'unique_callers': len(self.forward),
                'unique_targets': len(self.reverse),
                'entry_points': len(self.entry_points),
                'leaf_routines': len(self.leaf_routines),
            }
        }

    def export_dot(self, output_path: str, max_nodes: int = 500) -> None:
        """Export to Graphviz DOT format for visualization."""
        # Limit nodes for reasonable graph size
        all_addrs = set(self.forward.keys()) | set(self.reverse.keys())
        if len(all_addrs) > max_nodes:
            # Prioritize entry points and their callees
            priority = set()
            for entry in self.entry_points:
                priority.add(entry)
                for ref in self.forward.get(entry, [])[:10]:
                    priority.add(ref.to_addr)
            all_addrs = priority

        with open(output_path, 'w') as f:
            f.write('digraph CallGraph {\n')
            f.write('  rankdir=LR;\n')
            f.write('  node [shape=box, fontsize=10];\n')

            # Color entry points
            for addr in self.entry_points:
                if addr in all_addrs:
                    f.write(f'  "{addr:06X}" [style=filled, fillcolor=lightgreen];\n')

            # Color leaf routines
            for addr in self.leaf_routines:
                if addr in all_addrs:
                    f.write(f'  "{addr:06X}" [style=filled, fillcolor=lightyellow];\n')

            # Edges
            for from_addr, refs in self.forward.items():
                if from_addr not in all_addrs:
                    continue
                for ref in refs:
                    if ref.to_addr not in all_addrs:
                        continue
                    color = 'red' if ref.from_bank != ref.to_bank else 'black'
                    style = 'dashed' if ref.kind == 'branch' else 'solid'
                    f.write(f'  "{from_addr:06X}" -> "{ref.to_addr:06X}" '
                            f'[color={color}, style={style}, label="{ref.kind}"];\n')

            f.write('}\n')

    def export_json(self, output_path: str) -> None:
        """Export to JSON for tooling integration."""
        data = self.export_dict()
        with open(output_path, 'w') as f:
            json.dump(data, f, indent=2)

    def export_hooks_subgraph(self, hooks: List['HookInfo'], depth: int = 3) -> Dict:
        """Export subgraph showing N levels from each hook."""
        subgraph = {'nodes': set(), 'edges': []}

        def add_descendants(addr: int, current_depth: int):
            if current_depth > depth or addr in subgraph['nodes']:
                return
            subgraph['nodes'].add(addr)
            for ref in self.forward.get(addr, []):
                subgraph['edges'].append({
                    'from': hex(ref.from_addr),
                    'to': hex(ref.to_addr),
                    'kind': ref.kind,
                })
                add_descendants(ref.to_addr, current_depth + 1)

        for hook in hooks:
            add_descendants(hook.address, 0)

        return {
            'nodes': [hex(addr) for addr in sorted(subgraph['nodes'])],
            'edges': subgraph['edges'],
        }


@dataclass
class AnalysisDiagnostic:
    """A diagnostic from static analysis."""
    severity: str                      # 'error', 'warning', 'info'
    message: str
    address: int
    source_file: Optional[str] = None
    source_line: Optional[int] = None
    context: dict = field(default_factory=dict)


@dataclass
class AnalysisResult:
    """Complete analysis result."""
    diagnostics: list[AnalysisDiagnostic] = field(default_factory=list)
    hooks: list[HookInfo] = field(default_factory=list)
    cross_refs: list[CrossRef] = field(default_factory=list)
    labels: dict[int, str] = field(default_factory=dict)
    entry_states: dict[int, RegisterState] = field(default_factory=dict)
    call_graph: Optional[CallGraph] = None
    stack_issues: list[AnalysisDiagnostic] = field(default_factory=list)
    return_states: dict[int, list[tuple[int, RegisterState]]] = field(default_factory=dict)

    def errors(self) -> list[AnalysisDiagnostic]:
        return [d for d in self.diagnostics if d.severity == 'error']

    def warnings(self) -> list[AnalysisDiagnostic]:
        return [d for d in self.diagnostics if d.severity == 'warning']

    def success(self) -> bool:
        return len(self.errors()) == 0


# =============================================================================
# ROM Utilities
# =============================================================================

def snes_to_pc(addr: int, mapper: str = 'lorom') -> int:
    """Convert SNES address to PC (file) offset."""
    if mapper == 'lorom':
        bank = (addr >> 16) & 0xFF
        offset = addr & 0xFFFF
        if bank < 0x40:
            return (bank * 0x8000) + (offset - 0x8000)
        elif 0x80 <= bank < 0xC0:
            return ((bank - 0x80) * 0x8000) + (offset - 0x8000)
        elif bank >= 0xC0:
            return ((bank - 0xC0) * 0x8000) + (offset - 0x8000)
    elif mapper == 'hirom':
        bank = (addr >> 16) & 0xFF
        offset = addr & 0xFFFF
        if bank < 0x40:
            return (bank * 0x10000) + offset
        elif 0x80 <= bank < 0xC0:
            return ((bank - 0x80) * 0x10000) + offset
    return -1


def pc_to_snes(pc: int, mapper: str = 'lorom') -> int:
    """Convert PC offset to SNES address."""
    if mapper == 'lorom':
        bank = pc // 0x8000
        offset = (pc % 0x8000) + 0x8000
        return (bank << 16) | offset
    elif mapper == 'hirom':
        bank = pc // 0x10000
        offset = pc % 0x10000
        return (bank << 16) | offset
    return -1


def get_operand_size(opcode: int, m_width: int, x_width: int) -> int:
    """Get operand size for an opcode given M/X state."""
    # This is a simplified version - full implementation would use opcode_table

    # Immediate M-dependent: LDA, ADC, SBC, AND, ORA, EOR, CMP, BIT
    imm_m = {0x09, 0x29, 0x49, 0x69, 0x89, 0xA9, 0xC9, 0xE9}
    # Immediate X-dependent: LDX, LDY, CPX, CPY
    imm_x = {0xA0, 0xA2, 0xC0, 0xE0}

    # Check mode based on high nibble patterns
    mode = opcode & 0x1F

    if opcode in imm_m:
        return m_width
    if opcode in imm_x:
        return x_width

    # Direct page modes
    if mode in {0x05, 0x06, 0x07, 0x15, 0x16, 0x17}:
        return 1
    # Absolute modes
    if mode in {0x0D, 0x0E, 0x0F, 0x1D, 0x1E, 0x1F}:
        if (opcode & 0x0F) == 0x0F:  # Long modes
            return 3
        return 2
    # Stack relative
    if mode in {0x03, 0x13}:
        return 1
    # Relative branches
    if opcode in {0x10, 0x30, 0x50, 0x70, 0x90, 0xB0, 0xD0, 0xF0, 0x80}:
        return 1
    if opcode in {0x82}:  # BRL
        return 2
    # Block move
    if opcode in {0x44, 0x54}:
        return 2
    # JSR/JMP absolute
    if opcode in {0x20, 0x4C, 0x6C, 0x7C, 0xFC}:
        return 2
    # JSL/JML long
    if opcode in {0x22, 0x5C, 0xDC}:
        return 3
    # Immediate 8-bit always
    if opcode in {0x00, 0x02, 0xC2, 0xE2}:  # BRK, COP, REP, SEP
        return 1
    # PEA, PER
    if opcode in {0xF4, 0x62}:
        return 2
    # PEI
    if opcode == 0xD4:
        return 1

    # Default: implied or accumulator
    return 0


# =============================================================================
# Disassembler / State Tracker
# =============================================================================

class StateTracker:
    """Tracks register state while walking through code."""

    def __init__(self, rom_data: bytes, mapper: str = 'lorom'):
        self.rom = rom_data
        self.mapper = mapper
        self.visited: dict[int, RegisterState] = {}
        self.pending: list[tuple[int, RegisterState, int]] = []
        self.cross_refs: list[CrossRef] = []
        self.diagnostics: list[AnalysisDiagnostic] = []
        self.labels: dict[int, str] = {}
        self.call_graph = CallGraph()
        self.routine_starts: Dict[int, int] = {}  # Return addr -> routine start
        self.stack_issues: List[AnalysisDiagnostic] = []
        self.return_states: Dict[int, List[Tuple[int, RegisterState]]] = defaultdict(list)

    def read_byte(self, snes_addr: int) -> Optional[int]:
        """Read a byte from ROM at SNES address."""
        pc = snes_to_pc(snes_addr, self.mapper)
        if 0 <= pc < len(self.rom):
            return self.rom[pc]
        return None

    def read_word(self, snes_addr: int) -> Optional[int]:
        """Read a 16-bit word (little-endian) from ROM."""
        lo = self.read_byte(snes_addr)
        hi = self.read_byte(snes_addr + 1)
        if lo is not None and hi is not None:
            return lo | (hi << 8)
        return None

    def read_long(self, snes_addr: int) -> Optional[int]:
        """Read a 24-bit long address from ROM."""
        lo = self.read_byte(snes_addr)
        mid = self.read_byte(snes_addr + 1)
        hi = self.read_byte(snes_addr + 2)
        if all(x is not None for x in [lo, mid, hi]):
            return lo | (mid << 8) | (hi << 16)
        return None

    def analyze_from(self, start_addr: int, initial_state: RegisterState) -> None:
        """Analyze code starting from an address with given state."""
        self.call_graph.add_entry_point(start_addr)
        self.pending.append((start_addr, initial_state, start_addr))

        while self.pending:
            addr, state, routine_start = self.pending.pop(0)
            self._trace_block(addr, state, routine_start)

    def _record_call(self, from_addr: int, to_addr: int, kind: str) -> None:
        """Record a call/jump reference in both cross_refs and call_graph."""
        ref = CrossRef(from_addr, to_addr, kind)
        self.cross_refs.append(ref)
        self.call_graph.add_reference(ref)

    def _trace_block(self, addr: int, state: RegisterState, routine_start: int) -> None:
        """Trace a basic block of code."""
        current_state = state.copy()

        while True:
            # Check if already visited with compatible state
            if addr in self.visited:
                existing = self.visited[addr]
                if self._states_compatible(existing, current_state):
                    return  # Already analyzed this path
                else:
                    # State mismatch - this could be a bug!
                    self._check_state_mismatch(addr, existing, current_state)
                    return

            # Mark as visited
            self.visited[addr] = current_state.copy()

            # Read opcode
            opcode = self.read_byte(addr)
            if opcode is None:
                return  # Out of bounds

            # Get operand size
            op_size = get_operand_size(opcode, current_state.m_width(), current_state.x_width())

            # Process the instruction
            next_addr = addr + 1 + op_size

            # Handle flag-modifying instructions
            if opcode == 0xC2:  # REP
                mask = self.read_byte(addr + 1)
                if mask is not None:
                    if mask & 0x20:  # M bit
                        current_state.m_flag = False  # 16-bit
                    if mask & 0x10:  # X bit
                        current_state.x_flag = False  # 16-bit

            elif opcode == 0xE2:  # SEP
                mask = self.read_byte(addr + 1)
                if mask is not None:
                    if mask & 0x20:  # M bit
                        current_state.m_flag = True  # 8-bit
                    if mask & 0x10:  # X bit
                        current_state.x_flag = True  # 8-bit

            elif opcode == 0x28:  # PLP
                # P register restored from stack - state becomes unknown
                current_state.m_flag = None
                current_state.x_flag = None
                current_state.stack_depth -= 1

            elif opcode == 0xFB:  # XCE
                # Swap emulation mode - complex, simplify to native mode assumption
                current_state.m_flag = True
                current_state.x_flag = True

            # Handle stack operations with proper tracking
            elif opcode in PUSH_OPCODES:
                mnemonic = PUSH_OPCODES[opcode]
                current_state.record_stack_op(addr, mnemonic)

            elif opcode in PULL_OPCODES:
                mnemonic = PULL_OPCODES[opcode]
                current_state.record_stack_op(addr, mnemonic)

            # Handle calls - record cross-reference and continue
            if opcode in CALL_OPCODES:
                mnemonic, size = CALL_OPCODES[opcode]
                if size == 2:  # JSR abs
                    target = self.read_word(addr + 1)
                    if target is not None:
                        # Same bank
                        target_addr = (addr & 0xFF0000) | target
                        self._record_call(addr, target_addr, 'jsr')
                        # Record stack effect of JSR
                        current_state.record_stack_op(addr, 'JSR')
                        # Queue the call target for analysis
                        self.pending.append((target_addr, current_state.copy(), target_addr))
                elif size == 3:  # JSL long
                    target = self.read_long(addr + 1)
                    if target is not None:
                        self._record_call(addr, target, 'jsl')
                        # Record stack effect of JSL
                        current_state.record_stack_op(addr, 'JSL')
                        # Queue the call target for analysis
                        self.pending.append((target, current_state.copy(), target))
                # Continue after call (return will bring us back)
                addr = next_addr
                continue

            # Handle returns - end of block and check stack balance
            if opcode in RETURN_OPCODES:
                mnemonic = RETURN_OPCODES[opcode]
                current_state.record_stack_op(addr, mnemonic)

                if opcode == 0x40:  # RTI
                    current_state.m_flag = None
                    current_state.x_flag = None

                # Record return state for the routine start
                self.return_states[routine_start].append((addr, current_state.copy()))

                # Check stack balance at return
                # Note: We don't count JSR/JSL/RTS/RTL in the check since
                # those are matched by the call/return, but PHP/PLP pairs should match
                self._check_stack_balance_at_return(addr, current_state)
                return

            # Handle unconditional jumps - end of block, queue target
            if opcode in JUMP_OPCODES:
                mnemonic, size = JUMP_OPCODES[opcode]
                if size == 2:
                    target = self.read_word(addr + 1)
                    if target is not None:
                        target_addr = (addr & 0xFF0000) | target
                        self._record_call(addr, target_addr, mnemonic.lower())
                        self.pending.append((target_addr, current_state.copy(), routine_start))
                elif size == 3:
                    target = self.read_long(addr + 1)
                    if target is not None:
                        self._record_call(addr, target, mnemonic.lower())
                        self.pending.append((target, current_state.copy(), routine_start))
                return

            # Handle branches - queue both paths
            if opcode in BRANCH_OPCODES:
                if opcode == 0x82:  # BRL - 16-bit relative
                    offset = self.read_word(addr + 1)
                    if offset is not None:
                        if offset >= 0x8000:
                            offset -= 0x10000
                        target = next_addr + offset
                else:  # 8-bit relative
                    offset = self.read_byte(addr + 1)
                    if offset is not None:
                        if offset >= 0x80:
                            offset -= 0x100
                        target = next_addr + offset

                if opcode == 0x80:  # BRA - unconditional
                    self._record_call(addr, target, 'branch')
                    self.pending.append((target, current_state.copy(), routine_start))
                    return
                else:
                    # Conditional branch - continue both paths
                    self._record_call(addr, target, 'branch')
                    self.pending.append((target, current_state.copy(), routine_start))
                    # Fall through to next instruction
                    addr = next_addr
                    continue

            # Normal instruction - continue to next
            addr = next_addr

    def _states_compatible(self, s1: RegisterState, s2: RegisterState) -> bool:
        """Check if two states are compatible (same or one unknown)."""
        m_ok = (s1.m_flag == s2.m_flag or s1.m_flag is None or s2.m_flag is None)
        x_ok = (s1.x_flag == s2.x_flag or s1.x_flag is None or s2.x_flag is None)
        return m_ok and x_ok

    def _check_state_mismatch(self, addr: int, existing: RegisterState, new: RegisterState) -> None:
        """Check for and report state mismatches."""
        issues = []

        if existing.m_flag is not None and new.m_flag is not None:
            if existing.m_flag != new.m_flag:
                existing_m = '8-bit' if existing.m_flag else '16-bit'
                new_m = '8-bit' if new.m_flag else '16-bit'
                issues.append(f"M flag mismatch: expected {existing_m}, got {new_m}")

        if existing.x_flag is not None and new.x_flag is not None:
            if existing.x_flag != new.x_flag:
                existing_x = '8-bit' if existing.x_flag else '16-bit'
                new_x = '8-bit' if new.x_flag else '16-bit'
                issues.append(f"X flag mismatch: expected {existing_x}, got {new_x}")

        if issues:
            label = self.labels.get(addr, f"${addr:06X}")
            self.diagnostics.append(AnalysisDiagnostic(
                severity='warning',
                message=f"State mismatch at {label}: " + "; ".join(issues),
                address=addr,
                context={
                    'expected': str(existing),
                    'actual': str(new)
                }
            ))

    def _check_stack_balance_at_return(self, addr: int, state: RegisterState) -> None:
        """Check for stack imbalance at a return instruction.

        Counts only local stack operations (PHP/PLP, PHA/PLA, etc.), not
        the implicit JSR/JSL/RTS/RTL pairs which are handled by call structure.
        """
        # Filter to only local push/pull operations (not calls/returns)
        local_ops = [op for op in state.stack_ops
                     if op.opcode not in ('JSR', 'JSL', 'RTS', 'RTL', 'RTI')]

        if not local_ops:
            return

        # Calculate local stack balance
        local_balance = sum(op.delta for op in local_ops)

        if local_balance != 0:
            label = self.labels.get(addr, f"${addr:06X}")
            self.stack_issues.append(AnalysisDiagnostic(
                severity='warning',
                message=f"Stack imbalance at return {label}: depth={local_balance}",
                address=addr,
                context={
                    'balance': local_balance,
                    'operations': [
                        {'addr': f"${op.addr:06X}", 'op': op.opcode, 'delta': op.delta}
                        for op in local_ops[-10:]  # Last 10 ops for context
                    ]
                }
            ))

        # Check common ABI push/pull pairs for mismatches
        pair_counts = {op.opcode: 0 for op in local_ops}
        for op in local_ops:
            pair_counts[op.opcode] = pair_counts.get(op.opcode, 0) + 1

        pairs = [
            ("PHB", "PLB"),
            ("PHD", "PLD"),
            ("PHP", "PLP"),
            ("PHA", "PLA"),
            ("PHX", "PLX"),
            ("PHY", "PLY"),
        ]

        for push, pull in pairs:
            pushes = pair_counts.get(push, 0)
            pulls = pair_counts.get(pull, 0)
            if pushes != pulls:
                label = self.labels.get(addr, f"${addr:06X}")
                self.stack_issues.append(AnalysisDiagnostic(
                    severity='warning',
                    message=f"Unbalanced {push}/{pull} at return {label}: pushes={pushes}, pulls={pulls}",
                    address=addr,
                    context={
                        'pushes': pushes,
                        'pulls': pulls,
                        'pair': f"{push}/{pull}",
                    }
                ))

    def validate_stack_balance(self, routine_start: int, routine_end: int) -> List[AnalysisDiagnostic]:
        """Validate stack is balanced between routine entry and RTS/RTL.

        Args:
            routine_start: SNES address of routine start
            routine_end: SNES address of routine end (inclusive)

        Returns:
            List of stack imbalance diagnostics
        """
        warnings = []

        for addr in range(routine_start, routine_end + 1):
            state = self.visited.get(addr)
            if not state:
                continue

            # Read opcode
            opcode = self.read_byte(addr)
            if opcode is None:
                continue

            # Check at RTS/RTL
            if opcode in (0x60, 0x6B):  # RTS, RTL
                # Filter to only local push/pull operations
                local_ops = [op for op in state.stack_ops
                             if op.opcode not in ('JSR', 'JSL', 'RTS', 'RTL', 'RTI')]

                if local_ops:
                    local_balance = sum(op.delta for op in local_ops)
                    if local_balance != 0:
                        label = self.labels.get(addr, f"${addr:06X}")
                        warnings.append(AnalysisDiagnostic(
                            severity='warning',
                            message=f"Stack imbalance at {label}: depth={local_balance}",
                            address=addr,
                            context={
                                'routine_start': f"${routine_start:06X}",
                                'balance': local_balance,
                            }
                        ))

        return warnings

    def finalize_call_graph(self) -> None:
        """Finalize call graph analysis after tracing is complete."""
        self.call_graph.compute_leaf_routines()


# =============================================================================
# Hook Analysis
# =============================================================================

def parse_hooks_json(path: Path) -> list[HookInfo]:
    """Parse a hooks.json file."""
    hooks = []
    with open(path) as f:
        data = json.load(f)

    def _coerce_width(value):
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

    for hook in data.get('hooks', []):
        addr_str = hook.get('address', '0')
        if addr_str.startswith('0x') or addr_str.startswith('$'):
            addr = int(addr_str.replace('$', ''), 16)
        else:
            addr = int(addr_str)

        exp_m = _coerce_width(hook.get('expected_m'))
        exp_x = _coerce_width(hook.get('expected_x'))
        exp_exit_m = _coerce_width(hook.get('expected_exit_m'))
        exp_exit_x = _coerce_width(hook.get('expected_exit_x'))
        expected_entry_state = None
        if exp_m is not None or exp_x is not None:
            expected_entry_state = RegisterState(
                m_flag=exp_m == 8 if exp_m is not None else None,
                x_flag=exp_x == 8 if exp_x is not None else None
            )
        expected_exit_state = None
        if exp_exit_m is not None or exp_exit_x is not None:
            expected_exit_state = RegisterState(
                m_flag=exp_exit_m == 8 if exp_exit_m is not None else None,
                x_flag=exp_exit_x == 8 if exp_exit_x is not None else None
            )

        hooks.append(HookInfo(
            name=hook.get('name', f'hook_{addr:06X}'),
            address=addr,
            kind=hook.get('kind', 'jsl'),
            expected_entry_state=expected_entry_state,
            expected_exit_state=expected_exit_state,
            source_file=hook.get('source', '').split(':')[0] if ':' in hook.get('source', '') else None,
            source_line=int(hook.get('source', '').split(':')[1]) if ':' in hook.get('source', '') else None,
        ))

    return hooks


def parse_sym_file(path: Path) -> dict[int, str]:
    """Parse a WLA-style symbol file."""
    labels = {}

    with open(path) as f:
        in_labels = False
        for line in f:
            line = line.strip()
            if line == '[labels]':
                in_labels = True
                continue
            if line.startswith('['):
                in_labels = False
                continue
            if in_labels and ':' in line:
                parts = line.split(':')
                if len(parts) >= 2:
                    bank = int(parts[0], 16)
                    rest = parts[1].split()
                    if len(rest) >= 2:
                        offset = int(rest[0], 16)
                        name = rest[1]
                        addr = (bank << 16) | offset
                        labels[addr] = name

    return labels


def find_jsl_hooks(rom_data: bytes, labels: dict[int, str], mapper: str = 'lorom') -> list[HookInfo]:
    """Find JSL hooks by scanning for JSL instructions to labeled addresses."""
    hooks = []

    # Build reverse label map
    name_to_addr = {v: k for k, v in labels.items()}

    # Scan for JSL (0x22) opcodes
    for pc in range(len(rom_data) - 3):
        if rom_data[pc] == 0x22:  # JSL
            target = rom_data[pc + 1] | (rom_data[pc + 2] << 8) | (rom_data[pc + 3] << 16)

            # Check if target has a label
            if target in labels:
                src_addr = pc_to_snes(pc, mapper)
                hooks.append(HookInfo(
                    name=labels[target],
                    address=target,
                    kind='jsl',
                ))

    # Deduplicate by target address
    seen = set()
    unique_hooks = []
    for h in hooks:
        if h.address not in seen:
            seen.add(h.address)
            unique_hooks.append(h)

    return unique_hooks


# =============================================================================
# Main Analysis
# =============================================================================

def analyze_rom(rom_path: Path,
                sym_path: Optional[Path] = None,
                hooks_path: Optional[Path] = None,
                entry_points: Optional[list[int]] = None,
                mapper: str = 'lorom') -> AnalysisResult:
    """Perform static analysis on a ROM."""

    result = AnalysisResult()

    # Load ROM
    rom_data = rom_path.read_bytes()

    # Load labels from symbol file
    if sym_path and sym_path.exists():
        result.labels = parse_sym_file(sym_path)

    # Load hooks
    if hooks_path and hooks_path.exists():
        result.hooks = parse_hooks_json(hooks_path)
    else:
        # Auto-detect hooks from JSL targets
        result.hooks = find_jsl_hooks(rom_data, result.labels, mapper)

    # Create state tracker
    tracker = StateTracker(rom_data, mapper)
    tracker.labels = result.labels

    # Analyze from entry points
    if entry_points:
        for addr in entry_points:
            # Default entry state: 8-bit M/X (common ALTTP convention)
            initial_state = RegisterState(m_flag=True, x_flag=True)
            tracker.analyze_from(addr, initial_state)

    # Analyze from hooks with expected states
    for hook in result.hooks:
        # ALTTP convention: most routines expect 8-bit M/X
        initial_state = RegisterState(m_flag=True, x_flag=True)
        if hook.expected_entry_state:
            initial_state = hook.expected_entry_state
        tracker.analyze_from(hook.address, initial_state)

    # Finalize call graph
    tracker.finalize_call_graph()

    # Collect results
    result.diagnostics = tracker.diagnostics
    result.cross_refs = tracker.cross_refs
    result.entry_states = tracker.visited
    result.call_graph = tracker.call_graph
    result.stack_issues = tracker.stack_issues
    result.return_states = tracker.return_states

    # Additional validation
    _validate_hooks(result, tracker)

    return result


def _validate_hooks(result: AnalysisResult, tracker: StateTracker) -> None:
    """Validate hook entry states against expectations."""

    for hook in result.hooks:
        if hook.address not in tracker.visited:
            result.diagnostics.append(AnalysisDiagnostic(
                severity='warning',
                message=f"Hook '{hook.name}' at ${hook.address:06X} was not reached during analysis",
                address=hook.address,
            ))
            continue

        actual_state = tracker.visited[hook.address]

        if hook.expected_entry_state:
            expected = hook.expected_entry_state
            issues = []
            if expected.m_flag is not None and actual_state.m_flag is not None:
                if expected.m_flag != actual_state.m_flag:
                    exp = '8-bit' if expected.m_flag else '16-bit'
                    act = '8-bit' if actual_state.m_flag else '16-bit'
                    issues.append(f"M flag: expected {exp}, got {act}")

            if expected.x_flag is not None and actual_state.x_flag is not None:
                if expected.x_flag != actual_state.x_flag:
                    exp = '8-bit' if expected.x_flag else '16-bit'
                    act = '8-bit' if actual_state.x_flag else '16-bit'
                    issues.append(f"X flag: expected {exp}, got {act}")

            if issues:
                result.diagnostics.append(AnalysisDiagnostic(
                    severity='error',
                    message=f"Hook '{hook.name}' entry state mismatch: " + "; ".join(issues),
                    address=hook.address,
                    context={'expected': str(expected), 'actual': str(actual_state)}
                ))

        if hook.expected_exit_state:
            return_states = tracker.return_states.get(hook.address, [])
            if not return_states:
                result.diagnostics.append(AnalysisDiagnostic(
                    severity='warning',
                    message=f"Hook '{hook.name}' has no recorded return states for exit check",
                    address=hook.address,
                ))
            else:
                expected_exit = hook.expected_exit_state
                for ret_addr, ret_state in return_states:
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
                        result.diagnostics.append(AnalysisDiagnostic(
                            severity='error',
                            message=f"Hook '{hook.name}' exit state mismatch at ${ret_addr:06X}: " + "; ".join(exit_issues),
                            address=ret_addr,
                            context={'expected': str(expected_exit), 'actual': str(ret_state)}
                        ))

        # Check for unknown states at entry
        if actual_state.m_flag is None:
            result.diagnostics.append(AnalysisDiagnostic(
                severity='warning',
                message=f"Hook '{hook.name}': M flag unknown at entry - may cause width mismatch",
                address=hook.address,
                context={'entry_state': str(actual_state)}
            ))

        if actual_state.x_flag is None:
            result.diagnostics.append(AnalysisDiagnostic(
                severity='warning',
                message=f"Hook '{hook.name}': X flag unknown at entry - may cause width mismatch",
                address=hook.address,
                context={'entry_state': str(actual_state)}
            ))


# =============================================================================
# CLI
# =============================================================================

def main():
    parser = argparse.ArgumentParser(
        description="Z3DK Static Analyzer - Programmatic Bug Detection",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Analyze ROM with symbol file
  %(prog)s game.sfc --sym game.sym

  # Analyze with hooks manifest
  %(prog)s game.sfc --hooks hooks.json

  # Analyze starting from specific addresses
  %(prog)s game.sfc --entry 0x008000 --entry 0x02C0C3

  # Output as JSON
  %(prog)s game.sfc --sym game.sym --json > report.json
"""
    )

    parser.add_argument('rom', type=Path, help='ROM file to analyze')
    parser.add_argument('--sym', type=Path, help='Symbol file (.sym)')
    parser.add_argument('--hooks', type=Path, help='Hooks manifest (hooks.json)')
    parser.add_argument('--entry', action='append', type=lambda x: int(x, 0),
                       help='Entry point address (can specify multiple)')
    parser.add_argument('--mapper', choices=['lorom', 'hirom'], default='lorom',
                       help='ROM mapper type (default: lorom)')
    parser.add_argument('--json', action='store_true', help='Output as JSON')
    parser.add_argument('--call-graph', type=Path, metavar='FILE',
                       help='Export call graph to DOT file for visualization')
    parser.add_argument('--call-graph-json', type=Path, metavar='FILE',
                       help='Export call graph to JSON file')
    parser.add_argument('--check-stack', action='store_true',
                       help='Enable detailed stack balance checking')
    parser.add_argument('-v', '--verbose', action='store_true', help='Verbose output')

    args = parser.parse_args()

    if not args.rom.exists():
        print(f"Error: ROM file not found: {args.rom}", file=sys.stderr)
        return 1

    # Run analysis
    result = analyze_rom(
        args.rom,
        sym_path=args.sym,
        hooks_path=args.hooks,
        entry_points=args.entry,
        mapper=args.mapper
    )

    # Export call graph if requested
    if args.call_graph and result.call_graph:
        result.call_graph.export_dot(str(args.call_graph))
        print(f"Call graph exported to: {args.call_graph}", file=sys.stderr)

    if args.call_graph_json and result.call_graph:
        result.call_graph.export_json(str(args.call_graph_json))
        print(f"Call graph JSON exported to: {args.call_graph_json}", file=sys.stderr)

    # Output results
    if args.json:
        all_diagnostics = result.diagnostics + result.stack_issues
        output = {
            'success': result.success(),
            'diagnostics': [
                {
                    'severity': d.severity,
                    'message': d.message,
                    'address': f"${d.address:06X}",
                    'source_file': d.source_file,
                    'source_line': d.source_line,
                    'context': d.context,
                }
                for d in all_diagnostics
            ],
            'hooks_analyzed': len(result.hooks),
            'cross_refs_found': len(result.cross_refs),
            'addresses_visited': len(result.entry_states),
            'stack_issues': len(result.stack_issues),
            'return_states': len(result.return_states),
        }
        # Add call graph stats if available
        if result.call_graph:
            output['call_graph'] = result.call_graph.export_dict()['stats']
            # Add problematic patterns
            cross_bank = result.call_graph.find_cross_bank_calls()
            cycles = result.call_graph.find_recursive_calls()
            if cross_bank:
                output['cross_bank_calls'] = [
                    {'from': f"${r.from_addr:06X}", 'to': f"${r.to_addr:06X}", 'kind': r.kind}
                    for r in cross_bank[:20]  # Limit output
                ]
            if cycles:
                output['recursive_cycles'] = [
                    [f"${addr:06X}" for addr in cycle]
                    for cycle in cycles[:10]  # Limit output
                ]
        print(json.dumps(output, indent=2))
    else:
        print(f"Z3DK Static Analysis Report")
        print(f"=" * 60)
        print(f"ROM: {args.rom}")
        print(f"Hooks analyzed: {len(result.hooks)}")
        print(f"Cross-references: {len(result.cross_refs)}")
        print(f"Addresses visited: {len(result.entry_states)}")

        # Call graph stats
        if result.call_graph:
            stats = result.call_graph.export_dict()['stats']
            print(f"Call graph: {stats['total_refs']} refs, "
                  f"{stats['unique_callers']} callers, "
                  f"{stats['unique_targets']} targets")

            # Report problematic patterns
            cross_bank = result.call_graph.find_cross_bank_calls()
            cycles = result.call_graph.find_recursive_calls()
            if cross_bank:
                print(f"Cross-bank calls (potential bugs): {len(cross_bank)}")
            if cycles:
                print(f"Recursive cycles: {len(cycles)}")

        print()

        all_diagnostics = result.diagnostics + result.stack_issues
        if all_diagnostics:
            print(f"Diagnostics ({len(all_diagnostics)}):")
            print("-" * 60)
            for d in all_diagnostics:
                prefix = "ERROR" if d.severity == 'error' else "WARN" if d.severity == 'warning' else "INFO"
                addr = f"${d.address:06X}"
                label = result.labels.get(d.address, '')
                if label:
                    addr = f"{label} ({addr})"
                print(f"  [{prefix}] {addr}: {d.message}")
                if args.verbose and d.context:
                    for k, v in d.context.items():
                        print(f"          {k}: {v}")
            print()
        else:
            print("No issues found.")

        # Report cross-bank calls in verbose mode
        if args.verbose and result.call_graph:
            cross_bank = result.call_graph.find_cross_bank_calls()
            if cross_bank:
                print(f"Cross-Bank Calls ({len(cross_bank)}):")
                print("-" * 60)
                for ref in cross_bank[:20]:
                    print(f"  ${ref.from_addr:06X} -> ${ref.to_addr:06X} ({ref.kind})")
                if len(cross_bank) > 20:
                    print(f"  ... and {len(cross_bank) - 20} more")
                print()

        if not result.success():
            return 1

    return 0


if __name__ == '__main__':
    sys.exit(main())
