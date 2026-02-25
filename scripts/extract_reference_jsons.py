#!/usr/bin/env python3
"""Extract z3dk reference JSONs from snes_data.generated.h."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


QUIRKS = [
    {
        "id": "mx_flag_mismatch",
        "title": "M/X width mismatch across calls",
        "description": "Subroutines can misread 8-bit vs 16-bit registers if callers do not preserve REP/SEP state.",
        "example_code": "SEP #$20\nJSR RoutineExpecting16BitA",
        "workaround": "Document entry width and set REP/SEP explicitly at routine boundaries.",
        "type": "mx_flag",
        "text": "Subroutines can misread 8-bit vs 16-bit registers if callers do not preserve REP/SEP state.",
        "source": "z3dk_curated",
    },
    {
        "id": "stack_imbalance",
        "title": "Unbalanced stack operations",
        "description": "PHA/PHX/PHY without matching pulls causes RTS/RTL to return to invalid addresses.",
        "example_code": "PHA\nJSR Work\nRTS",
        "workaround": "Match every push with a corresponding pull on all control-flow paths.",
        "type": "stack",
        "text": "PHA/PHX/PHY without matching pulls causes RTS/RTL to return to invalid addresses.",
        "source": "z3dk_curated",
    },
    {
        "id": "long_vs_short_call",
        "title": "JSR/JSL bank mismatch",
        "description": "JSR is bank-local. Cross-bank calls require JSL/RTL.",
        "example_code": "JSR $9000 ; target lives in another bank",
        "workaround": "Use JSL for far calls and ensure callee returns with RTL.",
        "type": "calling_convention",
        "text": "JSR is bank-local. Cross-bank calls require JSL/RTL.",
        "source": "z3dk_curated",
    },
    {
        "id": "vram_outside_blank",
        "title": "VRAM writes outside VBlank",
        "description": "Writing VRAM/CGRAM/OAM during active display can corrupt visual output.",
        "example_code": "STA $2118\nSTA $2119",
        "workaround": "Perform PPU memory writes during VBlank/HBlank or forced blank.",
        "type": "ppu_timing",
        "text": "Writing VRAM/CGRAM/OAM during active display can corrupt visual output.",
        "source": "z3dk_curated",
    },
    {
        "id": "dma_without_size",
        "title": "DMA trigger without byte count",
        "description": "Starting DMA with $43x5/$43x6 unset copies unexpected lengths.",
        "example_code": "LDA #$01 : STA $420B",
        "workaround": "Set transfer mode, source, destination, and size before writing $420B.",
        "type": "dma",
        "text": "Starting DMA with $43x5/$43x6 unset copies unexpected lengths.",
        "source": "z3dk_curated",
    },
    {
        "id": "dummy_vram_read",
        "title": "Missing initial VRAM dummy read",
        "description": "The first read from $2139/$213A after setting $2116/$2117 is a prefetch value.",
        "example_code": "LDA $2139 ; treated as first real byte",
        "workaround": "Discard the first read after each VRAM address change.",
        "type": "vram_read",
        "text": "The first read from $2139/$213A after setting $2116/$2117 is a prefetch value.",
        "source": "z3dk_curated",
    },
    {
        "id": "direct_page_assumption",
        "title": "Direct page relocation assumptions",
        "description": "Code using direct-page addressing may break if D register is moved.",
        "example_code": "LDA $00",
        "workaround": "Set D register intentionally or avoid hard DP assumptions in shared routines.",
        "type": "direct_page",
        "text": "Code using direct-page addressing may break if D register is moved.",
        "source": "z3dk_curated",
    },
    {
        "id": "db_register_leak",
        "title": "Data bank register leakage",
        "description": "Long data reads depend on DB. Subroutines that change DB must restore it.",
        "example_code": "PHB\nLDA #$7E : PHA : PLB\n... ; missing PLB",
        "workaround": "Wrap DB changes with PHB/PLB and keep scope minimal.",
        "type": "data_bank",
        "text": "Long data reads depend on DB. Subroutines that change DB must restore it.",
        "source": "z3dk_curated",
    },
    {
        "id": "forced_blank_restore",
        "title": "Forgetting to restore INIDISP",
        "description": "Leaving forced blank set keeps the screen black.",
        "example_code": "LDA #$80 : STA $2100",
        "workaround": "Restore brightness and clear forced blank after PPU updates.",
        "type": "ppu_state",
        "text": "Leaving forced blank set keeps the screen black.",
        "source": "z3dk_curated",
    },
    {
        "id": "oam_hi_table_conflict",
        "title": "OAM high-table overlap",
        "description": "Sprite size/priority bits in high OAM can be overwritten by incorrect indexing.",
        "example_code": "STA $2104 ; sequential writes without high-table planning",
        "workaround": "Account for both low OAM and high table layout when streaming OAM data.",
        "type": "oam",
        "text": "Sprite size/priority bits in high OAM can be overwritten by incorrect indexing.",
        "source": "z3dk_curated",
    },
    {
        "id": "branch_range_limit",
        "title": "8-bit branch range overflow",
        "description": "Conditional branches only span -128..+127 bytes and fail silently in macro expansions.",
        "example_code": "BNE far_label",
        "workaround": "Use BRL or split logic with intermediate labels.",
        "type": "branching",
        "text": "Conditional branches only span -128..+127 bytes and fail silently in macro expansions.",
        "source": "z3dk_curated",
    },
    {
        "id": "nmi_race",
        "title": "NMI race while updating shared state",
        "description": "Main thread and NMI thread can race on WRAM queues or PPU mirrors.",
        "example_code": "STA $7E1F00 ; updated in both main and NMI",
        "workaround": "Use handoff flags or double-buffered mirrors synchronized in NMI.",
        "type": "concurrency",
        "text": "Main thread and NMI thread can race on WRAM queues or PPU mirrors.",
        "source": "z3dk_curated",
    },
    {
        "id": "hdma_table_termination",
        "title": "HDMA table termination missing",
        "description": "HDMA tables must terminate with 00 count byte or engine reads past table end.",
        "example_code": "db $7F, $12, $34 ; no 00 terminator",
        "workaround": "Always terminate HDMA tables and validate table bank/address.",
        "type": "hdma",
        "text": "HDMA tables must terminate with 00 count byte or engine reads past table end.",
        "source": "z3dk_curated",
    },
    {
        "id": "self_modifying_bank_wrap",
        "title": "Bank wrap in self-modifying code",
        "description": "Incrementing low-word pointers can wrap within bank and miss intended target.",
        "example_code": "INC pointer_low\nBNE +\nINC pointer_high",
        "workaround": "Use full 24-bit pointer logic when patching across bank boundaries.",
        "type": "pointer_math",
        "text": "Incrementing low-word pointers can wrap within bank and miss intended target.",
        "source": "z3dk_curated",
    },
    {
        "id": "irq_nmi_reentry",
        "title": "Interrupt re-entry assumptions",
        "description": "Handlers that do not preserve full CPU state can corrupt mainline execution.",
        "example_code": "IRQ:\n  PHX\n  ...\n  RTI",
        "workaround": "Save/restore all clobbered registers and keep handlers bounded.",
        "type": "interrupts",
        "text": "Handlers that do not preserve full CPU state can corrupt mainline execution.",
        "source": "z3dk_curated",
    },
    {
        "id": "joypad_latch_timing",
        "title": "Joypad latch timing misuse",
        "description": "Polling joypad registers outside expected latch cadence can yield stale inputs.",
        "example_code": "LDA $4218\nLDA $4219",
        "workaround": "Read joypad state in the expected frame step after latch/update.",
        "type": "input",
        "text": "Polling joypad registers outside expected latch cadence can yield stale inputs.",
        "source": "z3dk_curated",
    },
    {
        "id": "cgram_half_write",
        "title": "CGRAM half-write ordering",
        "description": "CGRAM color values require low byte then high byte; reversed order corrupts colors.",
        "example_code": "STA $2122 ; high byte first",
        "workaround": "Write low 8 bits then high 7 bits for each color entry.",
        "type": "cgram",
        "text": "CGRAM color values require low byte then high byte; reversed order corrupts colors.",
        "source": "z3dk_curated",
    },
    {
        "id": "mode7_matrix_stale",
        "title": "Mode 7 matrix stale registers",
        "description": "Partial updates to $211B-$2120 can produce transient distortion.",
        "example_code": "STA $211B\nSTA $211C ; other matrix regs unchanged",
        "workaround": "Update all required Mode 7 registers as one atomic configuration step.",
        "type": "mode7",
        "text": "Partial updates to $211B-$2120 can produce transient distortion.",
        "source": "z3dk_curated",
    },
]


ENTRY_RE = re.compile(r"^\s*(SnesRegisterInfo|OpcodeDocInfo)\{(.+)\},\s*$")


def _split_cpp_fields(payload: str) -> list[str]:
    fields: list[str] = []
    current: list[str] = []
    in_string = False
    escape = False

    for ch in payload:
        if in_string:
            current.append(ch)
            if escape:
                escape = False
            elif ch == "\\":
                escape = True
            elif ch == '"':
                in_string = False
            continue

        if ch == '"':
            in_string = True
            current.append(ch)
            continue

        if ch == ",":
            fields.append("".join(current).strip())
            current = []
            continue

        current.append(ch)

    if current:
        fields.append("".join(current).strip())
    return fields


def _decode_cpp_string(value: str) -> str:
    if value == "nullptr":
        return ""
    if not (value.startswith('"') and value.endswith('"')):
        return value
    content = value[1:-1]
    return bytes(content, "utf-8").decode("unicode_escape")


def _sanitize_text(value: str) -> str:
    text = value.replace("\r", "")
    text = re.sub(r"[ \t]+", " ", text)
    text = re.sub(r"\n{3,}", "\n\n", text)
    return text.strip()


def _register_category(address: int) -> str:
    if 0x2100 <= address <= 0x213F:
        return "ppu"
    if 0x2140 <= address <= 0x2143:
        return "apu_io"
    if 0x2180 <= address <= 0x2183:
        return "wram_io"
    if 0x4200 <= address <= 0x421F:
        return "cpu_io"
    if 0x4300 <= address <= 0x437F:
        return "dma"
    return "misc"


def _extract_array_block(header_text: str, array_name: str) -> list[str]:
    marker = f"{array_name} = {{"
    start = header_text.find(marker)
    if start < 0:
        raise ValueError(f"Array marker not found: {array_name}")
    body_start = header_text.find("\n", start)
    if body_start < 0:
        raise ValueError(f"Array body start not found: {array_name}")
    end = header_text.find("\n};", body_start)
    if end < 0:
        raise ValueError(f"Array body end not found: {array_name}")
    block = header_text[body_start + 1 : end]
    return block.splitlines()


def _parse_registers(lines: list[str]) -> list[dict]:
    rows: list[dict] = []
    for line in lines:
        match = ENTRY_RE.match(line)
        if not match or match.group(1) != "SnesRegisterInfo":
            continue
        fields = _split_cpp_fields(match.group(2))
        if len(fields) != 3:
            continue
        try:
            address = int(fields[0], 16)
        except ValueError:
            continue
        name = _decode_cpp_string(fields[1]).strip() or "UNKNOWN"
        description = _sanitize_text(_decode_cpp_string(fields[2]))
        readable = not name.endswith("_WR")
        writable = not name.endswith("_RD")
        rows.append(
            {
                "address": f"0x{address:04X}",
                "name": name,
                "description": description,
                "readable": readable,
                "writable": writable,
                "category": _register_category(address),
            }
        )
    return rows


def _extract_opcode_hexes(codes_raw: str) -> list[str]:
    found = re.findall(r"\b([0-9A-F]{2})H\b", codes_raw.upper())
    deduped = []
    seen = set()
    for code in found:
        if code in seen:
            continue
        seen.add(code)
        deduped.append(f"0x{code}")
    return deduped


def _parse_opcodes(lines: list[str]) -> list[dict]:
    rows: list[dict] = []
    for line in lines:
        match = ENTRY_RE.match(line)
        if not match or match.group(1) != "OpcodeDocInfo":
            continue
        fields = _split_cpp_fields(match.group(2))
        if len(fields) != 5:
            continue
        mnemonic = _decode_cpp_string(fields[0]).strip()
        if not mnemonic:
            continue
        full_name = _decode_cpp_string(fields[1]).strip()
        description = _sanitize_text(_decode_cpp_string(fields[2]))
        flags_raw = _decode_cpp_string(fields[3]).strip()
        flags_affected = re.sub(r"^\s*Flags Affected:\s*", "", flags_raw, flags=re.IGNORECASE).strip()
        codes_raw = _sanitize_text(_decode_cpp_string(fields[4]))
        addressing_modes = [
            {
                "mode": "unknown",
                "opcode_hex": op_hex,
                "bytes": 0,
                "cycles": "",
                "syntax": "",
            }
            for op_hex in _extract_opcode_hexes(codes_raw)
        ]
        rows.append(
            {
                "mnemonic": mnemonic,
                "full_name": full_name,
                "description": description,
                "flags": flags_affected,
                "flags_affected": flags_affected,
                "addressing_modes": addressing_modes,
                "codes_raw": codes_raw,
            }
        )
    return rows


def _write_json(path: Path, payload: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate docs/reference JSON files from snes_data.generated.h.")
    parser.add_argument(
        "--header",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "src" / "z3dk_core" / "snes_data.generated.h",
        help="Path to snes_data.generated.h",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "docs" / "reference",
        help="Output directory for JSON reference files.",
    )
    args = parser.parse_args()

    header_path = args.header.expanduser().resolve()
    out_dir = args.out_dir.expanduser().resolve()
    if not header_path.exists():
        raise SystemExit(f"Header not found: {header_path}")

    header_text = header_path.read_text(encoding="utf-8")
    register_lines = _extract_array_block(header_text, "kSnesRegisters")
    opcode_lines = _extract_array_block(header_text, "kOpcodeDocs")

    registers = _parse_registers(register_lines)
    opcodes = _parse_opcodes(opcode_lines)
    quirks = QUIRKS

    _write_json(out_dir / "snes_registers.json", registers)
    _write_json(out_dir / "65816_opcodes.json", opcodes)
    _write_json(out_dir / "snes_quirks.json", quirks)

    print(f"header: {header_path}")
    print(f"output_dir: {out_dir}")
    print(f"snes_registers: {len(registers)}")
    print(f"65816_opcodes: {len(opcodes)}")
    print(f"snes_quirks: {len(quirks)}")


if __name__ == "__main__":
    main()
