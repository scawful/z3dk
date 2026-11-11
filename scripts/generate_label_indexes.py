#!/usr/bin/env python3
"""Generate Z3DK label indexes and z3disasm label maps.

Vanilla disassemblies (kan's usdasm, MathOnNapkins, Jared Brian's AsarUSALTTPDisassembly)
become labels_<style>.csv files that `z3disasm --labels` accepts. Without --disasm-only the
script also builds the Oracle of Secrets overrides, index and merged label map.
"""
from __future__ import annotations

import argparse
import csv
import re
import sys
from pathlib import Path
from typing import Callable

SYM_LINE = re.compile(r"^([0-9A-Fa-f]{2}:[0-9A-Fa-f]{4})\s+(:)?(\S+)$")
ASM_LABEL = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_.]*)\s*:")
STRUCT_START = re.compile(r"^\s*struct\s+([A-Za-z_][A-Za-z0-9_]*)\b", re.IGNORECASE)
STRUCT_END = re.compile(r"^\s*endstruct\b", re.IGNORECASE)
STRUCT_FIELD = re.compile(r"^\s*\.([A-Za-z_][A-Za-z0-9_]*)\s*:")
USDASM_ADDR = re.compile(r"#_([0-9A-Fa-f]{6})")
# MoN / Jared Brian style: "; *$10000-$10053 LONG" (ROM file offset) on the line(s) above a routine.
PC_OFFSET_COMMENT = re.compile(r"^\s*;\s*\*?\$([0-9A-Fa-f]{3,6})(?:\s*-\s*\$[0-9A-Fa-f]{3,6})?(?:\s|$)")
# Colon must touch the name so chained opcodes like "CLC : XCE" are not labels.
OFFSET_STYLE_LABEL = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*):(?=\s|$)")
LOROM_MAX_PC = 0x400000
# Jared Brian's General/WRAM.asm: "struct WRAM $7E0000", "; $22[0x02] - (Player)", ".PlayerXCoord: skip $02".
WRAM_STRUCT_START = re.compile(r"^\s*struct\s+([A-Za-z_][A-Za-z0-9_]*)\s+\$([0-9A-Fa-f]{6})\b")
WRAM_HEADER = re.compile(r"^\s*;\s*\$([0-9A-Fa-f]{2,6})\[([^\]]*)\]\s*-?\s*(?:\(([^)]*)\))?")
WRAM_FIELD = re.compile(r"^\s*\.([A-Za-z_][A-Za-z0-9_]*):(?:\s*skip\s+(\S+))?")
MESEN_INVALID_CHARS = re.compile(r"[^@_a-zA-Z0-9]")
NOISY_SYM_PREFIXES = (
    "neg_",
    "pos_",
    "anon_",
    "data_",
    "label_",
    "tmp_",
    "loc_",
)

SKIP_DIRS = {
    ".git",
    ".context",
    ".claude",
    ".genkit",
    ".gemini",
    ".vscode",
    "Roms",
    "SaveStates",
    "ExportedRooms",
    "ExportedDungeons",
    "build",
    "tests",
    "scripts",
}

PREFIX_REGION = [
    ("Oracle_Menu_", "menu"),
    ("Oracle_ItemMenu", "menu"),
    ("Oracle_HUD", "menu"),
    ("Oracle_Map", "map"),
    ("Oracle_Message", "dialog"),
    ("Oracle_Save", "save"),
    ("Oracle_Overworld", "overworld"),
    ("Oracle_LoadOverworld", "overworld"),
    ("Oracle_OverworldTransition", "overworld"),
    ("Oracle_Dungeon", "dungeon"),
    ("Oracle_RoomTag", "dungeon"),
    ("Oracle_DungeonMap", "dungeon"),
]

PREFIX_INCLUDE = [p for p, _ in PREFIX_REGION]

EXTRA_INCLUDE = [
    "Oracle_LinkItem_",
    "Oracle_MagicRing_",
    "Oracle_Ancilla_",
]


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def parse_sym(sym_path: Path) -> dict[str, str]:
    labels: dict[str, str] = {}
    in_labels = False
    with sym_path.open() as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if not line:
                continue
            if line.startswith("[labels]"):
                in_labels = True
                continue
            if not in_labels or line.startswith("["):
                continue
            match = SYM_LINE.match(line)
            if not match:
                continue
            addr = match.group(1).upper()
            name = match.group(3)
            labels[name] = addr
    return labels


def read_seed_overrides(path: Path) -> dict[str, dict[str, str]]:
    if not path.exists():
        return {}
    seeds: dict[str, dict[str, str]] = {}
    with path.open() as handle:
        reader = csv.DictReader(handle)
        for row in reader:
            label = (row.get("label") or "").strip()
            if not label:
                continue
            seeds[label] = {
                "type": (row.get("type") or "").strip(),
                "region": (row.get("region") or "").strip(),
                "notes": (row.get("notes") or "").strip(),
            }
    return seeds


def classify_type(name: str, addr: str, seed_type: str | None) -> str:
    if seed_type:
        return seed_type
    if addr.startswith("7E:") or addr.startswith("7F:"):
        return "data"
    lower = name.lower()
    if "palette" in lower:
        return "pal"
    if "gfx" in lower or "chr" in lower:
        return "gfx"
    if "table" in lower or "tbl" in lower or "speedtable" in lower:
        return "tbl"
    if "ptr" in lower or "pointer" in lower:
        return "ptr"
    if "message" in lower or "dialog" in lower or "text" in lower:
        return "msg"
    return "func"


def classify_region(name: str, addr: str, seed_region: str | None) -> str:
    if seed_region:
        return seed_region
    for prefix, region in PREFIX_REGION:
        if name.startswith(prefix):
            return region
    if name.startswith("Oracle_Sprite_"):
        return "sprite"
    if name.startswith("Oracle_LinkItem_") or name.startswith("Oracle_MagicRing_") or name.startswith("Oracle_Ancilla_"):
        return "item"
    if any(token in name for token in ("Mask", "Deku", "Zora", "Bunny", "Wolf", "Minish", "Moosh")):
        return "mask"
    if any(token in name for token in ("Theme", "Song")):
        return "music"
    if addr.startswith("7E:") or addr.startswith("7F:"):
        return "wram"
    return "core"


def collect_asm_labels(root: Path) -> dict[str, tuple[str, int]]:
    labels: dict[str, tuple[str, int]] = {}
    for path in root.rglob("*.asm"):
        if any(part in SKIP_DIRS for part in path.parts):
            continue
        try:
            rel = path.relative_to(root)
        except ValueError:
            rel = path
        with path.open(errors="ignore") as handle:
            current_struct = ""
            for idx, line in enumerate(handle, 1):
                stripped = line.split(";", 1)[0]
                if STRUCT_END.match(stripped):
                    current_struct = ""
                    continue
                struct_match = STRUCT_START.match(stripped)
                if struct_match:
                    current_struct = struct_match.group(1)
                    if current_struct and current_struct not in labels:
                        labels[current_struct] = (str(rel), idx)
                    continue
                if current_struct:
                    field_match = STRUCT_FIELD.match(stripped)
                    if field_match:
                        field_label = f"{current_struct}.{field_match.group(1)}"
                        if field_label not in labels:
                            labels[field_label] = (str(rel), idx)
                        continue
                match = ASM_LABEL.match(stripped)
                if not match:
                    continue
                label = match.group(1)
                if label not in labels:
                    labels[label] = (str(rel), idx)
    return labels


def collect_usdasm_labels(root: Path) -> dict[str, dict[str, str]]:
    labels: dict[str, dict[str, str]] = {}
    paths = list(root.rglob("*.asm")) + list(root.rglob("*.inc"))
    for path in paths:
        if any(part in SKIP_DIRS for part in path.parts):
            continue
        try:
            rel = path.relative_to(root)
        except ValueError:
            rel = path
        pending: list[str] = []
        with path.open(errors="ignore") as handle:
            for idx, line in enumerate(handle, 1):
                label_match = ASM_LABEL.match(line)
                if label_match:
                    label = label_match.group(1)
                    if label not in labels:
                        labels[label] = {"file": str(rel), "line": str(idx), "address": ""}
                    pending.append(label)
                    continue
                addr_match = USDASM_ADDR.search(line)
                if addr_match and pending:
                    raw = addr_match.group(1).upper()
                    addr = f"${raw[:2]}:{raw[2:]}"
                    for label in pending:
                        if labels[label]["address"] == "":
                            labels[label]["address"] = addr
                    pending = []
    return labels


def pc_to_lorom(pc: int) -> str:
    return f"${pc // 0x8000:02X}:{(pc % 0x8000) + 0x8000:04X}"


def collect_offset_comment_labels(root: Path) -> dict[str, dict[str, str]]:
    labels: dict[str, dict[str, str]] = {}
    for path in sorted(root.rglob("*.asm")):
        if any(part in SKIP_DIRS for part in path.parts):
            continue
        try:
            rel = path.relative_to(root)
        except ValueError:
            rel = path
        pending: int | None = None
        with path.open(errors="ignore") as handle:
            for idx, line in enumerate(handle, 1):
                offset_match = PC_OFFSET_COMMENT.match(line)
                if offset_match:
                    pc = int(offset_match.group(1), 16)
                    pending = pc if pc < LOROM_MAX_PC else None
                    continue
                code = line.split(";", 1)[0]
                if not code.strip():
                    continue
                label_match = OFFSET_STYLE_LABEL.match(code)
                if label_match:
                    label = label_match.group(1)
                    if label not in labels:
                        address = pc_to_lorom(pending) if pending is not None else ""
                        labels[label] = {"file": str(rel), "line": str(idx), "address": address}
                pending = None
    return labels


def parse_asm_number(token: str) -> int | None:
    token = token.strip()
    try:
        if token.startswith("$"):
            return int(token[1:], 16)
        if token.lower().startswith("0x"):
            return int(token[2:], 16)
        return int(token, 10)
    except ValueError:
        return None


def format_snes_address(address: int) -> str:
    return f"${address >> 16:02X}:{address & 0xFFFF:04X}"


def collect_struct_wram_labels(path: Path, root: Path) -> tuple[dict[str, dict[str, str]], list[str]]:
    """Read struct fields; the "; $XX[0xNN] - (Tags)" header wins over summed skips when both exist."""
    labels: dict[str, dict[str, str]] = {}
    warnings: list[str] = []
    rel = str(path.relative_to(root)) if path.is_relative_to(root) else str(path)
    struct_name = ""
    base = offset = 0
    header: tuple[int, int | None, str] | None = None
    with path.open(errors="ignore") as handle:
        for idx, line in enumerate(handle, 1):
            if not struct_name:
                struct_match = WRAM_STRUCT_START.match(line)
                if struct_match:
                    struct_name = struct_match.group(1)
                    base = int(struct_match.group(2), 16)
                    offset = 0
                    header = None
                continue
            if STRUCT_END.match(line):
                struct_name = ""
                continue
            header_match = WRAM_HEADER.match(line)
            if header_match:
                digits = header_match.group(1)
                value = int(digits, 16)
                address = value if len(digits) == 6 else base + value
                header = (address, parse_asm_number(header_match.group(2)), (header_match.group(3) or "").strip())
                continue
            field_match = WRAM_FIELD.match(line.split(";", 1)[0])
            if not field_match:
                continue
            name = f"{struct_name}.{field_match.group(1)}"
            summed = base + offset
            address, size, tags = header if header else (summed, None, "")
            if header and header[0] != summed:
                warnings.append(
                    f"{rel}:{idx}: {name} header says ${header[0]:06X} but skips sum to ${summed:06X}"
                )
                # Resync so one missing skip is reported once, not for every later field.
                offset = header[0] - base
            header = None
            if field_match.group(2):
                step = parse_asm_number(field_match.group(2))
                if step is None:
                    warnings.append(f"{rel}:{idx}: {name} has unparsable 'skip {field_match.group(2)}'")
                    step = size or 0
                offset += step
            if name in labels:
                warnings.append(f"{rel}:{idx}: {name} duplicates line {labels[name]['line']}")
                continue
            labels[name] = {
                "file": rel,
                "line": str(idx),
                "address": format_snes_address(address),
                "size": str(size or ""),
                "comment": tags,
            }
    return labels, warnings


def mesen_label_name(label: str) -> str:
    name = MESEN_INVALID_CHARS.sub("_", label)
    return name if re.match(r"[@_a-zA-Z]", name) else f"_{name}"


def build_mlb_lines(labels: dict[str, dict[str, str]]) -> list[str]:
    """Mesen2 .mlb: SnesPrgRom takes a LoROM file offset, SnesWorkRam a $0-$1FFFF offset; one label per address."""
    entries: list[tuple[str, int, int, str, str]] = []
    seen_addresses: set[tuple[str, int]] = set()
    seen_names: set[str] = set()
    for label, meta in labels.items():
        address_text = meta.get("address") or ""
        if not address_text:
            continue
        bank_text, low_text = address_text.lstrip("$").split(":")
        bank, low = int(bank_text, 16), int(low_text, 16)
        if bank in (0x7E, 0x7F):
            memory, start = "SnesWorkRam", (bank - 0x7E) * 0x10000 + low
        elif low >= 0x8000:
            memory, start = "SnesPrgRom", (bank & 0x7F) * 0x8000 + (low - 0x8000)
        else:
            continue
        name = mesen_label_name(label)
        if (memory, start) in seen_addresses or name in seen_names:
            continue
        seen_addresses.add((memory, start))
        seen_names.add(name)
        size = parse_asm_number(meta.get("size") or "") or 1
        entries.append((memory, start, size, name, meta.get("comment") or ""))

    entries.sort(key=lambda e: (e[0], e[1]))
    lines: list[str] = []
    for i, (memory, start, size, name, comment) in enumerate(entries):
        end = start + size - 1
        next_start = entries[i + 1][1] if i + 1 < len(entries) and entries[i + 1][0] == memory else None
        width = 6 if memory == "SnesPrgRom" else 4
        span = f"{start:0{width}X}"
        if size > 1 and (next_start is None or next_start > end):
            span += f"-{end:0{width}X}"
        lines.append(f"{memory}:{span}:{name}" + (f":{comment}" if comment else ""))
    return lines


def newest_mtime(root: Path, patterns: tuple[str, ...]) -> float:
    latest = 0.0
    for pattern in patterns:
        for path in root.rglob(pattern):
            if any(part in SKIP_DIRS for part in path.parts):
                continue
            try:
                mtime = path.stat().st_mtime
            except FileNotFoundError:
                continue
            if mtime > latest:
                latest = mtime
    return latest


def read_csv_rows(path: Path) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    if not path.exists():
        return rows
    with path.open() as handle:
        reader = csv.DictReader(handle)
        for row in reader:
            rows.append(row)
    return rows


def build_override_labels(labels: dict[str, str], seed_overrides: dict[str, dict[str, str]]) -> list[dict[str, str]]:
    selected: set[str] = set(seed_overrides)

    for name in labels:
        if not name.startswith("Oracle_"):
            continue
        if any(name.startswith(prefix) for prefix in PREFIX_INCLUDE):
            selected.add(name)
            continue
        if any(name.startswith(prefix) for prefix in EXTRA_INCLUDE):
            selected.add(name)

    missing = sorted(name for name in selected if name not in labels)
    if missing:
        print("[z3dk] Missing labels from sym:", file=sys.stderr)
        for name in missing:
            print(f"  - {name}", file=sys.stderr)

    rows: list[dict[str, str]] = []
    for name in sorted(selected):
        addr = labels.get(name)
        if not addr:
            continue
        seed = seed_overrides.get(name, {})
        row = {
            "address": f"${addr}",
            "label": name,
            "type": classify_type(name, addr, seed.get("type")),
            "region": classify_region(name, addr, seed.get("region")),
            "notes": seed.get("notes", "") or "",
        }
        rows.append(row)
    rows.sort(key=lambda r: r["address"])
    return rows


def is_noisy_sym_label(label: str) -> bool:
    return label.startswith(NOISY_SYM_PREFIXES)


def write_csv(path: Path, rows: list[dict[str, str]], headers: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=headers)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def build_label_index(labels: dict[str, str], asm_labels: dict[str, tuple[str, int]]) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for name, (rel_path, line) in asm_labels.items():
        sym_label = name if name in labels else f"Oracle_{name}"
        addr = labels.get(sym_label)
        if not addr:
            continue
        rows.append({
            "label": sym_label,
            "address": f"${addr}",
            "file": rel_path,
            "line": str(line),
            "source_label": name,
            "source_repo": "oracle-of-secrets",
        })
    rows.sort(key=lambda r: (r["file"], int(r["line"])))
    return rows


def build_disasm_index(disasm_labels: dict[str, dict[str, str]], source: str) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for name, meta in disasm_labels.items():
        rows.append({
            "label": name,
            "address": meta.get("address", ""),
            "file": meta.get("file", ""),
            "line": meta.get("line", ""),
            "source_label": name,
            "source_repo": source,
        })
    rows.sort(key=lambda r: (r["file"], int(r["line"] or "0")))
    return rows


def load_disasm_labels(
    root: Path,
    index_path: Path,
    source: str,
    collector: Callable[[Path], dict[str, dict[str, str]]],
    patterns: tuple[str, ...],
) -> tuple[dict[str, dict[str, str]], list[dict[str, str]]]:
    if not root.exists():
        print(f"[z3dk] {source} root not found: {root}", file=sys.stderr)
        return {}, []
    latest = newest_mtime(root, patterns)
    if index_path.exists() and index_path.stat().st_mtime >= latest:
        rows = read_csv_rows(index_path)
        labels: dict[str, dict[str, str]] = {}
        for row in rows:
            label = row.get("label", "")
            if not label:
                continue
            labels[label] = {
                "address": row.get("address", ""),
                "file": row.get("file", ""),
                "line": row.get("line", ""),
            }
        return labels, rows
    labels = collector(root)
    rows = build_disasm_index(labels, source)
    write_csv(index_path, rows, ["label", "address", "file", "line", "source_label", "source_repo"])
    return labels, rows


def build_style_labels(disasm_labels: dict[str, dict[str, str]], source: str) -> list[dict[str, str]]:
    rows = [
        {"address": meta["address"], "label": label, "source": source}
        for label, meta in disasm_labels.items()
        if meta.get("address")
    ]
    rows.sort(key=lambda r: (r["address"], r["label"]))
    return rows


def build_merged_labels(sym_labels: dict[str, str], overrides: list[dict[str, str]], usdasm_labels: dict[str, dict[str, str]]) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    seen: set[tuple[str, str]] = set()

    for row in overrides:
        address = row["address"]
        label = row["label"]
        key = (address, label)
        if key in seen:
            continue
        seen.add(key)
        rows.append({
            "address": address,
            "label": label,
            "source": "override",
            "priority": "0",
        })

    for label, addr in sym_labels.items():
        if is_noisy_sym_label(label):
            continue
        address = f"${addr}"
        key = (address, label)
        if key in seen:
            continue
        seen.add(key)
        rows.append({
            "address": address,
            "label": label,
            "source": "sym",
            "priority": "1",
        })

    for label, meta in usdasm_labels.items():
        address = meta.get("address") or ""
        if not address:
            continue
        key = (address, label)
        if key in seen:
            continue
        seen.add(key)
        rows.append({
            "address": address,
            "label": label,
            "source": "usdasm",
            "priority": "3",
        })

    rows.sort(key=lambda r: (r["address"], int(r["priority"])))
    return rows


def main() -> int:
    root = repo_root()
    default_sym = root.parent / "oracle-of-secrets" / "Roms" / "oos168x.sym"
    default_asm = root.parent / "oracle-of-secrets"
    default_overrides = root / ".context" / "knowledge" / "label_overrides.csv"
    default_index = root / ".context" / "knowledge" / "label_index.csv"
    default_usdasm = Path.home() / "src" / "hobby" / "usdasm"
    default_usdasm_index = root / ".context" / "knowledge" / "label_index_usdasm.csv"
    default_mon = Path.home() / "src" / "hobby" / "alttp-disassembly"
    default_mon_index = root / ".context" / "knowledge" / "label_index_mon.csv"
    default_jared = Path.home() / "src" / "hobby" / "AsarUSALTTPDisassembly"
    default_jared_index = root / ".context" / "knowledge" / "label_index_jared.csv"
    default_style_dir = root / ".context" / "knowledge"
    default_all_index = root / ".context" / "knowledge" / "label_index_all.csv"
    default_merged_labels = root / ".context" / "knowledge" / "labels_merged.csv"

    parser = argparse.ArgumentParser(description="Generate Z3DK label indexes and z3disasm label maps.")
    parser.add_argument("--sym", type=Path, default=default_sym, help="Path to .sym file.")
    parser.add_argument("--asm-root", type=Path, default=default_asm, help="Oracle asm repo root.")
    parser.add_argument("--out-overrides", type=Path, default=default_overrides, help="Output overrides CSV.")
    parser.add_argument("--out-index", type=Path, default=default_index, help="Output label index CSV.")
    parser.add_argument("--usdasm-root", type=Path, default=default_usdasm, help="USDASM disassembly root.")
    parser.add_argument("--out-usdasm-index", type=Path, default=default_usdasm_index, help="Output USDASM label index CSV.")
    parser.add_argument("--mon-root", type=Path, default=default_mon, help="MathOnNapkins disassembly root.")
    parser.add_argument("--out-mon-index", type=Path, default=default_mon_index, help="Output MoN label index CSV.")
    parser.add_argument("--jared-root", type=Path, default=default_jared, help="Jared Brian AsarUSALTTPDisassembly root.")
    parser.add_argument("--out-jared-index", type=Path, default=default_jared_index, help="Output Jared Brian label index CSV.")
    parser.add_argument(
        "--jared-wram",
        type=Path,
        default=None,
        help="Jared Brian WRAM struct file (default: <jared-root>/General/WRAM.asm).",
    )
    parser.add_argument(
        "--out-style-dir",
        type=Path,
        default=default_style_dir,
        help="Directory for per-disassembly z3disasm label maps (labels_<style>.csv).",
    )
    parser.add_argument("--out-all-index", type=Path, default=default_all_index, help="Output combined label index CSV.")
    parser.add_argument("--out-merged-labels", type=Path, default=default_merged_labels, help="Output merged label map CSV.")
    parser.add_argument(
        "--disasm-only",
        action="store_true",
        help="Only index the vanilla disassemblies and write labels_<style>.csv; skip the Oracle of Secrets sym/asm inputs.",
    )
    args = parser.parse_args()

    if not args.disasm_only:
        if not args.sym.exists():
            print(f"[z3dk] Missing sym file: {args.sym} (use --disasm-only for vanilla label maps)", file=sys.stderr)
            return 1
        if not args.asm_root.exists():
            print(f"[z3dk] Missing asm root: {args.asm_root} (use --disasm-only for vanilla label maps)", file=sys.stderr)
            return 1

    usdasm_labels, usdasm_rows = load_disasm_labels(
        args.usdasm_root, args.out_usdasm_index, "usdasm", collect_usdasm_labels, ("*.asm", "*.inc")
    )
    mon_labels, mon_rows = load_disasm_labels(
        args.mon_root, args.out_mon_index, "mon", collect_offset_comment_labels, ("*.asm",)
    )
    jared_labels, jared_rows = load_disasm_labels(
        args.jared_root, args.out_jared_index, "jared", collect_offset_comment_labels, ("*.asm",)
    )

    jared_wram: dict[str, dict[str, str]] = {}
    wram_path = args.jared_wram or args.jared_root / "General" / "WRAM.asm"
    if jared_labels and wram_path.exists():
        jared_wram, wram_warnings = collect_struct_wram_labels(wram_path, args.jared_root)
        print(f"[z3dk] jared WRAM: {wram_path} ({len(jared_wram)} fields)")
        warnings_path = args.out_style_dir / "labels_jared_wram_warnings.txt"
        if wram_warnings:
            warnings_path.parent.mkdir(parents=True, exist_ok=True)
            warnings_path.write_text("\n".join(wram_warnings) + "\n")
            print(f"[z3dk] jared WRAM warnings: {warnings_path} ({len(wram_warnings)})")
        elif warnings_path.exists():
            warnings_path.unlink()

    style_counts: list[tuple[Path, int]] = []
    styles = (("usdasm", usdasm_labels), ("mon", mon_labels), ("jared", {**jared_labels, **jared_wram}))
    for style, style_labels in styles:
        if not style_labels:
            continue
        style_path = args.out_style_dir / f"labels_{style}.csv"
        style_rows = build_style_labels(style_labels, style)
        write_csv(style_path, style_rows, ["address", "label", "source"])
        mlb_lines = build_mlb_lines(style_labels)
        style_path.with_suffix(".mlb").write_text("\n".join(mlb_lines) + "\n")
        style_counts.append((style_path, len(style_rows)))

    if not style_counts and args.disasm_only:
        print("[z3dk] No disassembly roots found; pass --usdasm-root, --mon-root or --jared-root.", file=sys.stderr)
        return 1
    for style_path, count in style_counts:
        print(f"[z3dk] style labels: {style_path} (+ .mlb) ({count} addressed labels)")
    if mon_rows:
        print(f"[z3dk] mon index: {args.out_mon_index} ({len(mon_rows)} labels)")
    if jared_rows:
        print(f"[z3dk] jared index: {args.out_jared_index} ({len(jared_rows)} labels)")
    if args.disasm_only:
        return 0

    labels = parse_sym(args.sym)
    seed_overrides = read_seed_overrides(args.out_overrides)
    overrides = build_override_labels(labels, seed_overrides)
    write_csv(args.out_overrides, overrides, ["address", "label", "type", "region", "notes"])

    asm_labels = collect_asm_labels(args.asm_root)
    index_rows = build_label_index(labels, asm_labels)
    write_csv(args.out_index, index_rows, ["label", "address", "file", "line", "source_label", "source_repo"])

    all_rows = index_rows + usdasm_rows
    if all_rows:
        write_csv(args.out_all_index, all_rows, ["label", "address", "file", "line", "source_label", "source_repo"])

    merged_rows = build_merged_labels(labels, overrides, usdasm_labels)
    write_csv(args.out_merged_labels, merged_rows, ["address", "label", "source", "priority"])

    print(f"[z3dk] overrides: {args.out_overrides} ({len(overrides)} labels)")
    print(f"[z3dk] index: {args.out_index} ({len(index_rows)} labels)")
    if usdasm_rows:
        print(f"[z3dk] usdasm index: {args.out_usdasm_index} ({len(usdasm_rows)} labels)")
        print(f"[z3dk] all index: {args.out_all_index} ({len(all_rows)} labels)")
    print(f"[z3dk] merged labels: {args.out_merged_labels} ({len(merged_rows)} labels)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
