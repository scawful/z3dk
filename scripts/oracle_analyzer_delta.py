#!/usr/bin/env python3
"""
oracle_analyzer_delta.py

Compute a baseline vs current delta for z3dk Oracle analyzer diagnostics.

This is intentionally a small wrapper around the analyzer's `--json` output so
we can answer the only question that matters during development:

  "What NEW issues did this change introduce (and what did it fix)?"

Usage example:
  python3 scripts/oracle_analyzer.py /path/to/rom.sfc --json > /tmp/an_cur.json
  python3 scripts/oracle_analyzer.py /path/to/baseline.sfc --json > /tmp/an_base.json
  python3 scripts/oracle_analyzer_delta.py --baseline /tmp/an_base.json --current /tmp/an_cur.json
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Optional


_SEVERITY_ORDER = {
    "error": 0,
    "warning": 1,
    "info": 2,
}


def _parse_int(value: object) -> int:
    if isinstance(value, int):
        return value
    if isinstance(value, float):
        return int(value)
    if isinstance(value, str):
        s = value.strip()
        if not s:
            return 0
        try:
            # Allow: "0x123", "$123", "123"
            if s.startswith("$"):
                return int(s[1:], 16)
            return int(s, 0)
        except ValueError:
            return 0
    return 0


def _normalize_ctx_value(value: object) -> str:
    if value is None:
        return ""
    if isinstance(value, (str, int, float, bool)):
        return str(value)
    try:
        return json.dumps(value, sort_keys=True, separators=(",", ":"))
    except TypeError:
        return str(value)


@dataclass(frozen=True)
class DiagKey:
    severity: str
    address: int
    message: str
    ctx_items: tuple[tuple[str, str], ...]

    def sort_key(self) -> tuple[int, int, str, tuple[tuple[str, str], ...]]:
        return (_SEVERITY_ORDER.get(self.severity, 99), self.address, self.message, self.ctx_items)


def _diag_to_key(diag: dict[str, Any], ctx_keys: list[str]) -> DiagKey:
    severity = str(diag.get("severity", "")).strip().lower()
    address = _parse_int(diag.get("address", 0))
    message = str(diag.get("message", "")).strip()

    ctx = diag.get("context") or {}
    ctx_items: list[tuple[str, str]] = []
    if isinstance(ctx, dict):
        for k in ctx_keys:
            if k in ctx:
                ctx_items.append((k, _normalize_ctx_value(ctx.get(k))))

    return DiagKey(
        severity=severity,
        address=address,
        message=message,
        ctx_items=tuple(ctx_items),
    )


def _diag_matches_filters(
    diag: dict[str, Any],
    allowed_severities: Optional[set[str]],
    include_re: Optional[re.Pattern[str]],
    exclude_re: Optional[re.Pattern[str]],
) -> bool:
    severity = str(diag.get("severity", "")).strip().lower()
    if allowed_severities is not None and severity not in allowed_severities:
        return False

    msg = str(diag.get("message", "")).strip()
    if include_re is not None and not include_re.search(msg):
        return False
    if exclude_re is not None and exclude_re.search(msg):
        return False

    return True


def _load_diags(path: Path) -> list[dict[str, Any]]:
    try:
        data = json.loads(path.read_text())
    except Exception as exc:
        raise SystemExit(f"Failed to read JSON: {path} ({exc})")

    diags = data.get("diagnostics")
    if not isinstance(diags, list):
        raise SystemExit(f"Invalid analyzer JSON (missing diagnostics[]): {path}")

    # Expect list of dicts, but be defensive.
    out: list[dict[str, Any]] = []
    for d in diags:
        if isinstance(d, dict):
            out.append(d)
    return out


def _counter_from_diags(
    diags: Iterable[dict[str, Any]],
    ctx_keys: list[str],
    allowed_severities: Optional[set[str]],
    include_re: Optional[re.Pattern[str]],
    exclude_re: Optional[re.Pattern[str]],
) -> tuple[Counter[DiagKey], dict[DiagKey, dict[str, Any]]]:
    counter: Counter[DiagKey] = Counter()
    exemplar: dict[DiagKey, dict[str, Any]] = {}

    for d in diags:
        if not _diag_matches_filters(d, allowed_severities, include_re, exclude_re):
            continue
        k = _diag_to_key(d, ctx_keys)
        counter[k] += 1
        exemplar.setdefault(k, d)

    return counter, exemplar


def _count_by_severity(counter: Counter[DiagKey]) -> dict[str, int]:
    out: dict[str, int] = {}
    for k, n in counter.items():
        out[k.severity] = out.get(k.severity, 0) + n
    return out


def _format_key(k: DiagKey, count: int) -> str:
    addr = f"${k.address:06X}"
    suffix = ""
    if k.ctx_items:
        suffix = " [" + ", ".join(f"{ck}={cv}" for ck, cv in k.ctx_items) + "]"
    mult = f" x{count}" if count != 1 else ""
    return f"[{k.severity.upper():7}] {addr}: {k.message}{suffix}{mult}"


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Compute baseline vs current delta for Oracle analyzer diagnostics (z3dk)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--baseline", type=Path, required=True, help="Baseline analyzer JSON (oracle_analyzer.py --json)")
    parser.add_argument("--current", type=Path, required=True, help="Current analyzer JSON (oracle_analyzer.py --json)")
    parser.add_argument(
        "--only",
        choices=["new", "resolved", "both"],
        default="both",
        help="Which deltas to show (default: both)",
    )
    parser.add_argument(
        "--severity",
        default="error,warning",
        help="Comma-separated severities to include (default: error,warning; use 'all' for everything)",
    )
    parser.add_argument(
        "--context-keys",
        default="",
        help="Comma-separated context keys to include in identity/printing (default: none)",
    )
    parser.add_argument("--include", default="", help="Only include diagnostics whose message matches this regex")
    parser.add_argument("--exclude", default="", help="Exclude diagnostics whose message matches this regex")
    parser.add_argument("--max", type=int, default=50, help="Max lines to print per section (default: 50)")

    args = parser.parse_args(argv)

    ctx_keys = [s.strip() for s in args.context_keys.split(",") if s.strip()]

    allowed_severities: Optional[set[str]]
    if args.severity.strip().lower() == "all":
        allowed_severities = None
    else:
        allowed_severities = {s.strip().lower() for s in args.severity.split(",") if s.strip()}

    include_re = re.compile(args.include) if args.include else None
    exclude_re = re.compile(args.exclude) if args.exclude else None

    base_diags = _load_diags(args.baseline)
    cur_diags = _load_diags(args.current)

    base_counter, _base_exemplar = _counter_from_diags(base_diags, ctx_keys, allowed_severities, include_re, exclude_re)
    cur_counter, _cur_exemplar = _counter_from_diags(cur_diags, ctx_keys, allowed_severities, include_re, exclude_re)

    new_counter = cur_counter - base_counter
    resolved_counter = base_counter - cur_counter

    # Summary header
    print("Oracle Analyzer Delta")
    print("=" * 78)
    print(f"Baseline: {args.baseline}")
    print(f"Current:  {args.current}")
    print()
    print(f"Baseline diagnostics (filtered): {sum(base_counter.values())}")
    print(f"Current  diagnostics (filtered): {sum(cur_counter.values())}")
    print()
    print(f"New issues:      {sum(new_counter.values())}  {_count_by_severity(new_counter)}")
    print(f"Resolved issues: {sum(resolved_counter.values())}  {_count_by_severity(resolved_counter)}")
    print()

    def _print_section(title: str, counter: Counter[DiagKey]) -> None:
        print(title)
        print("-" * 78)
        if not counter:
            print("(none)")
            print()
            return
        keys = sorted(counter.keys(), key=lambda k: k.sort_key())
        for k in keys[: max(args.max, 0)]:
            print(_format_key(k, counter[k]))
        if len(keys) > args.max:
            print(f"... ({len(keys) - args.max} more)")
        print()

    if args.only in ("new", "both"):
        _print_section("New Diagnostics", new_counter)
    if args.only in ("resolved", "both"):
        _print_section("Resolved Diagnostics", resolved_counter)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

