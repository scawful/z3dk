#!/usr/bin/env python3
import csv
import os
import pathlib
import subprocess
import tempfile


def write_file(path: pathlib.Path, contents: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(contents)


def run() -> None:
    script = pathlib.Path(__file__).resolve().parents[2] / 'scripts' / 'generate_label_indexes.py'
    with tempfile.TemporaryDirectory() as tmpdir:
        root = pathlib.Path(tmpdir)
        asm_root = root / 'asm'
        usdasm_root = root / 'usdasm'
        usdasm_root.mkdir(parents=True, exist_ok=True)

        sym_path = root / 'test.sym'
        write_file(
            sym_path,
            '[labels]\n'
            '7E:E000 TimeState\n'
            '7E:E000 TimeState.Hours\n'
            '7E:E001 TimeState.Minutes\n'
        )

        asm_path = asm_root / 'structs.asm'
        write_file(
            asm_path,
            'struct TimeState $7EE000\n'
            '{\n'
            '  .Hours: skip 1\n'
            '  .Minutes: skip 1\n'
            '}\n'
            'endstruct\n'
        )

        mon_root = root / 'mon'
        write_file(
            mon_root / 'Bank02.asm',
            '    ; *$10000-$10053 LONG\n'
            '    Intro_SetupScreen:\n'
            '    {\n'
            '        CLC : XCE\n'
            '    }\n'
            '    BRANCH_ALPHA:\n'
            '    ; *$3D1 - $41D LOCAL\n'
            '    NMI_ReadJoypads:\n'
        )
        jared_root = root / 'jared'
        write_file(
            jared_root / 'Bank00.asm',
            '; $000061-$00007D JUMP TABLE\n'
            'Pool_Module_MainRouting:\n'
            '    .low  ; $000061\n'
        )

        out_overrides = root / 'label_overrides.csv'
        out_index = root / 'label_index.csv'
        out_usdasm = root / 'label_index_usdasm.csv'
        out_mon = root / 'label_index_mon.csv'
        out_jared = root / 'label_index_jared.csv'
        out_all = root / 'label_index_all.csv'
        out_merged = root / 'labels_merged.csv'

        cmd = [
            'python3',
            str(script),
            '--sym', str(sym_path),
            '--asm-root', str(asm_root),
            '--usdasm-root', str(usdasm_root),
            '--out-overrides', str(out_overrides),
            '--out-index', str(out_index),
            '--out-usdasm-index', str(out_usdasm),
            '--mon-root', str(mon_root),
            '--out-mon-index', str(out_mon),
            '--jared-root', str(jared_root),
            '--out-jared-index', str(out_jared),
            '--out-style-dir', str(root),
            '--out-all-index', str(out_all),
            '--out-merged-labels', str(out_merged)
        ]
        subprocess.check_call(cmd)

        def style_map(name: str) -> dict:
            with (root / f'labels_{name}.csv').open() as handle:
                return {row['label']: row['address'] for row in csv.DictReader(handle)}

        mon = style_map('mon')
        if mon != {'Intro_SetupScreen': '$02:8000', 'NMI_ReadJoypads': '$00:83D1'}:
            raise AssertionError(f'unexpected MoN style labels: {mon}')
        jared = style_map('jared')
        if jared != {'Pool_Module_MainRouting': '$00:8061'}:
            raise AssertionError(f'unexpected Jared style labels: {jared}')

        labels = set()
        with out_index.open() as handle:
            reader = csv.DictReader(handle)
            for row in reader:
                label = (row.get('label') or '').strip()
                if label:
                    labels.add(label)

        missing = {'TimeState', 'TimeState.Hours', 'TimeState.Minutes'} - labels
        if missing:
            raise AssertionError(f'missing struct labels: {sorted(missing)}')

    print('label struct test: ok')


if __name__ == '__main__':
    run()
