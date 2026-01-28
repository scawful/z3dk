#!/usr/bin/env python3
"""Generate Z3DK label overrides and a label -> source index for Oracle of Secrets."""
from __future__ import annotations

import argparse
import csv
import re
import sys
from pathlib import Path

SYM_LINE = re.compile(r"^([0-9A-Fa-f]{2}:[0-9A-Fa-f]{4})\s+(:)?(\S+)$")
ASM_LABEL = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_.]*)\s*:")
STRUCT_START = re.compile(r"^\s*struct\s+([A-Za-z_][A-Za-z0-9_]*)\b", re.IGNORECASE)
STRUCT_END = re.compile(r"^\s*endstruct\b", re.IGNORECASE)
STRUCT_FIELD = re.compile(r"^\s*\.([A-Za-z_][A-Za-z0-9_]*)\s*:")
USDASM_ADDR = re.compile(r"#_([0-9A-Fa-f]{6})")
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


def build_usdasm_index(usdasm_labels: dict[str, dict[str, str]]) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for name, meta in usdasm_labels.items():
        rows.append({
            "label": name,
            "address": meta.get("address", ""),
            "file": meta.get("file", ""),
            "line": meta.get("line", ""),
            "source_label": name,
            "source_repo": "usdasm",
        })
    rows.sort(key=lambda r: (r["file"], int(r["line"] or "0")))
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
    default_all_index = root / ".context" / "knowledge" / "label_index_all.csv"
    default_merged_labels = root / ".context" / "knowledge" / "labels_merged.csv"

    parser = argparse.ArgumentParser(description="Generate Z3DK label overrides and label index.")
    parser.add_argument("--sym", type=Path, default=default_sym, help="Path to .sym file.")
    parser.add_argument("--asm-root", type=Path, default=default_asm, help="Oracle asm repo root.")
    parser.add_argument("--out-overrides", type=Path, default=default_overrides, help="Output overrides CSV.")
    parser.add_argument("--out-index", type=Path, default=default_index, help="Output label index CSV.")
    parser.add_argument("--usdasm-root", type=Path, default=default_usdasm, help="USDASM disassembly root.")
    parser.add_argument("--out-usdasm-index", type=Path, default=default_usdasm_index, help="Output USDASM label index CSV.")
    parser.add_argument("--out-all-index", type=Path, default=default_all_index, help="Output combined label index CSV.")
    parser.add_argument("--out-merged-labels", type=Path, default=default_merged_labels, help="Output merged label map CSV.")
    args = parser.parse_args()

    if not args.sym.exists():
        print(f"[z3dk] Missing sym file: {args.sym}", file=sys.stderr)
        return 1
    if not args.asm_root.exists():
        print(f"[z3dk] Missing asm root: {args.asm_root}", file=sys.stderr)
        return 1

    labels = parse_sym(args.sym)
    seed_overrides = read_seed_overrides(args.out_overrides)
    overrides = build_override_labels(labels, seed_overrides)
    write_csv(args.out_overrides, overrides, ["address", "label", "type", "region", "notes"])

    asm_labels = collect_asm_labels(args.asm_root)
    index_rows = build_label_index(labels, asm_labels)
    write_csv(args.out_index, index_rows, ["label", "address", "file", "line", "source_label", "source_repo"])

    usdasm_rows: list[dict[str, str]] = []
    usdasm_labels: dict[str, dict[str, str]] = {}
    if args.usdasm_root.exists():
        usdasm_latest = newest_mtime(args.usdasm_root, ("*.asm", "*.inc"))
        usdasm_cached = args.out_usdasm_index.exists() and args.out_usdasm_index.stat().st_mtime >= usdasm_latest
        if usdasm_cached:
            usdasm_rows = read_csv_rows(args.out_usdasm_index)
            for row in usdasm_rows:
                label = row.get("label", "")
                if not label:
                    continue
                usdasm_labels[label] = {
                    "address": row.get("address", ""),
                    "file": row.get("file", ""),
                    "line": row.get("line", ""),
                }
        else:
            usdasm_labels = collect_usdasm_labels(args.usdasm_root)
            usdasm_rows = build_usdasm_index(usdasm_labels)
            write_csv(args.out_usdasm_index, usdasm_rows, ["label", "address", "file", "line", "source_label", "source_repo"])
    else:
        print(f"[z3dk] USDASM root not found: {args.usdasm_root}", file=sys.stderr)

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
