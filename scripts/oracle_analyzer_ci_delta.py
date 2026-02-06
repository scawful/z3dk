#!/usr/bin/env python3
"""
oracle_analyzer_ci_delta.py

CI-friendly wrapper around:
  - scripts/oracle_analyzer.py --json
  - scripts/oracle_analyzer_delta.py

Goal: fail builds only when NEW analyzer diagnostics are introduced versus a
baseline ROM (after optional filtering).

This script writes intermediate analyzer JSON to a temp dir (default: /tmp) and
does not create repo artifacts.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def _run(cmd: list[str], *, stdout_path: Path) -> None:
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if p.returncode != 0:
        raise SystemExit(
            "Command failed:\n"
            f"  {' '.join(cmd)}\n\n"
            f"stderr:\n{p.stderr}\n"
        )
    stdout_path.write_text(p.stdout)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Run oracle_analyzer on baseline/current ROMs and fail only on NEW diagnostics.",
    )
    parser.add_argument("--baseline-rom", type=Path, required=True, help="Baseline ROM path")
    parser.add_argument("--current-rom", type=Path, required=True, help="Current ROM path")

    # Pass-through analyzer args commonly needed in CI/dev.
    parser.add_argument("--sym", type=Path, default=None, help="Symbol file for oracle_analyzer.py")
    parser.add_argument("--hooks", type=Path, default=None, help="Hooks manifest JSON for oracle_analyzer.py")
    parser.add_argument("--project-root", type=Path, default=None, help="Project root for oracle_analyzer.py")

    # Delta filters.
    parser.add_argument("--severity", default="error", help="Severities to consider new (default: error)")
    parser.add_argument("--include", default="", help="Only include messages matching this regex")
    parser.add_argument("--exclude", default="", help="Exclude messages matching this regex")
    parser.add_argument(
        "--exclude-file",
        type=Path,
        default=None,
        help="Path to a file with regex patterns to exclude (one per line, '#' comments).",
    )
    parser.add_argument("--context-keys", default="", help="Context keys to include in delta identity")
    parser.add_argument("--max", type=int, default=50, help="Max lines to print in each delta section")
    parser.add_argument(
        "--tmpdir",
        type=Path,
        default=Path("/tmp"),
        help="Directory to write intermediate JSON (default: /tmp)",
    )
    parser.add_argument(
        "--keep-temp",
        action="store_true",
        help="Keep the temp directory instead of deleting it (debugging).",
    )

    args = parser.parse_args(argv)

    analyzer = Path(__file__).with_name("oracle_analyzer.py")
    delta = Path(__file__).with_name("oracle_analyzer_delta.py")

    if not analyzer.exists():
        raise SystemExit(f"Missing analyzer script: {analyzer}")
    if not delta.exists():
        raise SystemExit(f"Missing delta script: {delta}")

    tmpdir = Path(
        tempfile.mkdtemp(prefix="z3dk_oracle_analyzer_delta_", dir=str(args.tmpdir))
    )
    try:
        base_json = tmpdir / "an_base.json"
        cur_json = tmpdir / "an_cur.json"
        summary_json = tmpdir / "delta_summary.json"

        analyzer_args: list[str] = [sys.executable, str(analyzer)]
        if args.sym is not None:
            analyzer_args += ["--sym", str(args.sym)]
        if args.hooks is not None:
            analyzer_args += ["--hooks", str(args.hooks)]
        if args.project_root is not None:
            analyzer_args += ["--project-root", str(args.project_root)]
        analyzer_args += ["--json"]

        _run(analyzer_args + [str(args.baseline_rom)], stdout_path=base_json)
        _run(analyzer_args + [str(args.current_rom)], stdout_path=cur_json)

        delta_cmd: list[str] = [
            sys.executable,
            str(delta),
            "--baseline",
            str(base_json),
            "--current",
            str(cur_json),
            "--only",
            "new",
            "--severity",
            args.severity,
            "--context-keys",
            args.context_keys,
            "--include",
            args.include,
            "--exclude",
            args.exclude,
            "--max",
            str(args.max),
            "--fail-on-new",
            "--json-summary",
            str(summary_json),
        ]
        if args.exclude_file is not None:
            delta_cmd += ["--exclude-file", str(args.exclude_file)]

        # Exit code comes from --fail-on-new.
        p = subprocess.run(delta_cmd)
        if args.keep_temp:
            print(f"\nTemp dir kept: {tmpdir}", file=sys.stderr)
        return p.returncode
    finally:
        if not args.keep_temp:
            for child in tmpdir.glob("*"):
                try:
                    child.unlink()
                except OSError:
                    pass
            try:
                tmpdir.rmdir()
            except OSError:
                pass


if __name__ == "__main__":
    raise SystemExit(main())

