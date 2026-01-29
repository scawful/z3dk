#!/usr/bin/env python3
"""Generate a Mesen2 .watch file from MLB symbols."""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


def parse_mlb(path: Path) -> list[tuple[str, int, str, str]]:
    entries: list[tuple[str, int, str, str]] = []
    for raw in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        line = raw.strip()
        if not line or line.startswith(";") or line.startswith("#"):
            continue
        parts = line.split(":", 3)
        if len(parts) < 3:
            continue
        kind = parts[0].strip()
        addr_str = parts[1].strip()
        name = parts[2].strip()
        comment = parts[3].strip() if len(parts) > 3 else ""
        try:
            addr = int(addr_str, 16)
        except ValueError:
            continue
        entries.append((kind, addr, name, comment))
    return entries


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate .watch from MLB symbols")
    parser.add_argument("--mlb", required=True, type=Path, help="Input MLB file")
    parser.add_argument("--out", required=True, type=Path, help="Output .watch file")
    parser.add_argument("--types", default="SnesWorkRam,SnesSaveRam",
                        help="Comma-separated MLB types to include")
    parser.add_argument("--filter", help="Regex to filter label names")
    parser.add_argument("--include-comments", action="store_true",
                        help="Append MLB comments to labels")
    parser.add_argument("--dedupe", action="store_true", help="Dedupe by address")
    parser.add_argument("--max", type=int, default=0, help="Max entries (0 = no limit)")
    parser.add_argument("--annotations", type=Path,
                        help="Optional annotations.json; if provided, only @watch entries are used")

    args = parser.parse_args()

    if not args.mlb.exists():
        raise SystemExit(f"MLB not found: {args.mlb}")

    types = {t.strip() for t in args.types.split(",") if t.strip()}
    label_re = re.compile(args.filter) if args.filter else None

    entries = parse_mlb(args.mlb)
    addr_by_label = {name: addr for kind, addr, name, _ in entries}

    out_lines = []
    seen_addrs = set()

    if args.annotations:
        annotations = json.loads(args.annotations.read_text())
        for entry in annotations.get("annotations", []):
            if entry.get("type") != "watch":
                continue
            addr = entry.get("address")
            label = entry.get("label", "")
            fmt = entry.get("format", "")
            if not addr and label:
                addr = addr_by_label.get(label)
            if addr is None:
                continue
            if isinstance(addr, str):
                try:
                    addr = int(addr, 0)
                except ValueError:
                    continue
            if args.dedupe and addr in seen_addrs:
                continue
            seen_addrs.add(addr)
            label_out = label or entry.get("name", "")
            if args.include_comments and entry.get("note"):
                label_out = f"{label_out} - {entry.get('note')}"
            suffix = f" {fmt}" if fmt in ("hex", "dec", "bin") else ""
            out_lines.append(f"${addr:06X} {label_out}{suffix}")
            if args.max and len(out_lines) >= args.max:
                break
    else:
        for kind, addr, name, comment in entries:
            if kind not in types:
                continue
            if label_re and not label_re.search(name):
                continue
            if args.dedupe:
                if addr in seen_addrs:
                    continue
                seen_addrs.add(addr)
            label = name
            if args.include_comments and comment:
                label = f"{label} - {comment}"
            out_lines.append(f"${addr:06X} {label}")
            if args.max and len(out_lines) >= args.max:
                break

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("\n".join(out_lines) + ("\n" if out_lines else ""))
    print(f"Wrote {len(out_lines)} entries to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
