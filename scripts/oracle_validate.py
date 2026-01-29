#!/usr/bin/env python3
"""
Oracle of Secrets ROM Validator

CLI-first sanity checks for:
- Room header pointers + header bytes
- Room object data pointers + terminators
- Tile16 table basic integrity
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


def snes_to_pc(addr: int, mapper: str = "lorom") -> int:
    if mapper == "lorom":
        bank = (addr >> 16) & 0xFF
        offset = addr & 0xFFFF
        if bank < 0x40:
            return (bank * 0x8000) + (offset - 0x8000)
        if 0x80 <= bank < 0xC0:
            return ((bank - 0x80) * 0x8000) + (offset - 0x8000)
        if bank >= 0xC0:
            return ((bank - 0xC0) * 0x8000) + (offset - 0x8000)
    return -1


def pc_to_snes(pc: int, mapper: str = "lorom") -> int:
    if mapper == "lorom":
        bank = pc // 0x8000
        offset = (pc % 0x8000) + 0x8000
        return (bank << 16) | offset
    return -1


def _read_byte(rom: bytes, pc: int) -> int | None:
    if 0 <= pc < len(rom):
        return rom[pc]
    return None


def _read_word(rom: bytes, pc: int) -> int | None:
    lo = _read_byte(rom, pc)
    hi = _read_byte(rom, pc + 1)
    if lo is None or hi is None:
        return None
    return lo | (hi << 8)


def _read_long(rom: bytes, pc: int) -> int | None:
    lo = _read_byte(rom, pc)
    mid = _read_byte(rom, pc + 1)
    hi = _read_byte(rom, pc + 2)
    if lo is None or mid is None or hi is None:
        return None
    return lo | (mid << 8) | (hi << 16)


def _parse_int(value: str) -> int:
    return int(value, 0)


@dataclass
class Diagnostic:
    severity: str
    message: str
    address: int | None = None
    context: dict[str, Any] = field(default_factory=dict)


def validate_room_headers(
    rom: bytes,
    *,
    mapper: str,
    room_count: int,
    header_ptr_pc: int,
    header_bank_pc: int,
) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    header_ptr = _read_long(rom, header_ptr_pc)
    if header_ptr is None:
        diagnostics.append(Diagnostic(
            severity="error",
            message=f"Room header pointer not readable at PC 0x{header_ptr_pc:06X}",
            address=header_ptr_pc,
        ))
        return diagnostics

    header_table_pc = snes_to_pc(header_ptr, mapper)
    if header_table_pc < 0:
        diagnostics.append(Diagnostic(
            severity="error",
            message=f"Room header table pointer invalid SNES ${header_ptr:06X}",
            address=header_ptr_pc,
        ))
        return diagnostics

    bank = _read_byte(rom, header_bank_pc)
    if bank is None:
        diagnostics.append(Diagnostic(
            severity="error",
            message=f"Room header bank byte not readable at PC 0x{header_bank_pc:06X}",
            address=header_bank_pc,
        ))
        return diagnostics

    for room_id in range(room_count):
        entry_pc = header_table_pc + room_id * 2
        lo = _read_byte(rom, entry_pc)
        hi = _read_byte(rom, entry_pc + 1)
        if lo is None or hi is None:
            diagnostics.append(Diagnostic(
                severity="error",
                message=f"Room header pointer missing for room {room_id:03X}",
                address=entry_pc,
                context={"room": room_id},
            ))
            continue

        header_snes = (bank << 16) | (hi << 8) | lo
        header_pc = snes_to_pc(header_snes, mapper)
        if header_pc < 0 or header_pc + 8 >= len(rom):
            diagnostics.append(Diagnostic(
                severity="error",
                message=f"Room {room_id:03X} header out of bounds (${header_snes:06X})",
                address=entry_pc,
                context={"room": room_id, "header_snes": f"${header_snes:06X}"},
            ))
            continue

        header = rom[header_pc:header_pc + 9]
        if header[1] & 0xC0:
            diagnostics.append(Diagnostic(
                severity="warning",
                message=f"Room {room_id:03X} palette byte has high bits set",
                address=header_pc,
                context={"room": room_id, "value": f"0x{header[1]:02X}"},
            ))
        if header[8] & 0xFC:
            diagnostics.append(Diagnostic(
                severity="warning",
                message=f"Room {room_id:03X} header byte 8 has reserved bits set",
                address=header_pc + 8,
                context={"room": room_id, "value": f"0x{header[8]:02X}"},
            ))

    return diagnostics


def validate_room_objects(
    rom: bytes,
    *,
    mapper: str,
    room_count: int,
    object_ptr_pc: int,
    max_object_bytes: int,
) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    obj_ptr = _read_long(rom, object_ptr_pc)
    if obj_ptr is None:
        diagnostics.append(Diagnostic(
            severity="error",
            message=f"Room object pointer not readable at PC 0x{object_ptr_pc:06X}",
            address=object_ptr_pc,
        ))
        return diagnostics

    table_pc = snes_to_pc(obj_ptr, mapper)
    if table_pc < 0:
        diagnostics.append(Diagnostic(
            severity="error",
            message=f"Room object table pointer invalid SNES ${obj_ptr:06X}",
            address=object_ptr_pc,
        ))
        return diagnostics

    for room_id in range(room_count):
        entry_pc = table_pc + room_id * 3
        ptr = _read_long(rom, entry_pc)
        if ptr is None:
            diagnostics.append(Diagnostic(
                severity="error",
                message=f"Room object pointer missing for room {room_id:03X}",
                address=entry_pc,
                context={"room": room_id},
            ))
            continue

        obj_pc = snes_to_pc(ptr, mapper)
        if obj_pc < 0 or obj_pc + 1 >= len(rom):
            diagnostics.append(Diagnostic(
                severity="error",
                message=f"Room {room_id:03X} object data out of bounds (${ptr:06X})",
                address=entry_pc,
                context={"room": room_id, "object_snes": f"${ptr:06X}"},
            ))
            continue

        pos = obj_pc + 2  # skip floor/layout bytes
        bytes_read = 2
        layer = 0
        door = False
        transitions = 0

        while pos + 1 < len(rom) and bytes_read < max_object_bytes:
            b1 = rom[pos]
            b2 = rom[pos + 1]

            if b1 == 0xFF and b2 == 0xFF:
                pos += 2
                bytes_read += 2
                layer += 1
                transitions += 1
                door = False
                if layer == 3:
                    break
                continue

            if b1 == 0xF0 and b2 == 0xFF:
                pos += 2
                bytes_read += 2
                door = True
                continue

            if door:
                pos += 2
                bytes_read += 2
                continue

            if pos + 2 >= len(rom):
                diagnostics.append(Diagnostic(
                    severity="error",
                    message=f"Room {room_id:03X} object data truncated at PC 0x{pos:06X}",
                    address=pos,
                    context={"room": room_id},
                ))
                break

            pos += 3
            bytes_read += 3

        if layer < 3:
            diagnostics.append(Diagnostic(
                severity="warning",
                message=f"Room {room_id:03X} object data did not reach layer terminator (layers={layer})",
                address=obj_pc,
                context={"room": room_id, "layers_seen": layer, "bytes_read": bytes_read},
            ))

        if bytes_read >= max_object_bytes:
            diagnostics.append(Diagnostic(
                severity="warning",
                message=f"Room {room_id:03X} object data exceeded max bytes ({max_object_bytes})",
                address=obj_pc,
                context={"room": room_id, "bytes_read": bytes_read},
            ))

    return diagnostics


def validate_tile16(
    rom: bytes,
    *,
    tile16_start_pc: int,
    tile16_size: int,
    entry_size: int,
) -> list[Diagnostic]:
    diagnostics: list[Diagnostic] = []
    end_pc = tile16_start_pc + tile16_size
    if tile16_start_pc < 0 or end_pc > len(rom):
        diagnostics.append(Diagnostic(
            severity="error",
            message=f"Tile16 region out of bounds (PC 0x{tile16_start_pc:06X}-0x{end_pc:06X})",
            address=tile16_start_pc,
        ))
        return diagnostics

    blank_entries: list[int] = []
    count = tile16_size // entry_size
    for idx in range(count):
        start = tile16_start_pc + idx * entry_size
        entry = rom[start:start + entry_size]
        if all(b == 0x00 for b in entry) or all(b == 0xFF for b in entry):
            blank_entries.append(idx)

    if blank_entries:
        sample = [f"0x{idx:03X}" for idx in blank_entries[:10]]
        diagnostics.append(Diagnostic(
            severity="warning",
            message=f"Tile16 table contains {len(blank_entries)} blank entries",
            address=tile16_start_pc,
            context={"sample_indices": sample},
        ))

    return diagnostics


def main() -> int:
    parser = argparse.ArgumentParser(description="Oracle of Secrets ROM Validator")
    parser.add_argument("rom", type=Path, help="ROM file to validate")
    parser.add_argument("--mapper", choices=("lorom", "hirom"), default="lorom")
    parser.add_argument("--json", action="store_true", help="Output JSON diagnostics")
    parser.add_argument("--checks", default="headers,objects,tile16",
                        help="Comma-separated checks: headers,objects,tile16")

    parser.add_argument("--room-count", type=_parse_int, default=296)
    parser.add_argument("--room-header-pointer", type=_parse_int, default=0x00B5DD)
    parser.add_argument("--room-header-bank", type=_parse_int, default=0x00B5E7)
    parser.add_argument("--room-object-pointer", type=_parse_int, default=0x00874C)
    parser.add_argument("--max-object-bytes", type=_parse_int, default=0x2000)

    parser.add_argument("--tile16-start", type=_parse_int, default=0x1E8000)
    parser.add_argument("--tile16-size", type=_parse_int, default=0x8000)
    parser.add_argument("--tile16-entry-size", type=_parse_int, default=8)

    args = parser.parse_args()

    if not args.rom.exists():
        print(f"Error: ROM not found: {args.rom}", file=sys.stderr)
        return 1

    rom = args.rom.read_bytes()
    requested = {c.strip().lower() for c in args.checks.split(",") if c.strip()}

    diagnostics: list[Diagnostic] = []

    if "headers" in requested:
        diagnostics.extend(validate_room_headers(
            rom,
            mapper=args.mapper,
            room_count=args.room_count,
            header_ptr_pc=args.room_header_pointer,
            header_bank_pc=args.room_header_bank,
        ))

    if "objects" in requested:
        diagnostics.extend(validate_room_objects(
            rom,
            mapper=args.mapper,
            room_count=args.room_count,
            object_ptr_pc=args.room_object_pointer,
            max_object_bytes=args.max_object_bytes,
        ))

    if "tile16" in requested:
        diagnostics.extend(validate_tile16(
            rom,
            tile16_start_pc=args.tile16_start,
            tile16_size=args.tile16_size,
            entry_size=args.tile16_entry_size,
        ))

    if args.json:
        print(json.dumps([d.__dict__ for d in diagnostics], indent=2))
    else:
        if diagnostics:
            errors = [d for d in diagnostics if d.severity == "error"]
            warnings = [d for d in diagnostics if d.severity == "warning"]
            print(f"Oracle Validator: {len(errors)} errors, {len(warnings)} warnings")
            for d in diagnostics:
                addr = f" (PC 0x{d.address:06X})" if d.address is not None else ""
                print(f"[{d.severity.upper()}]{addr} {d.message}")
        else:
            print("Oracle Validator: no issues found.")

    return 1 if any(d.severity == "error" for d in diagnostics) else 0


if __name__ == "__main__":
    sys.exit(main())
