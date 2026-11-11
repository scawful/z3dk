#!/usr/bin/env python3
import csv
import pathlib
import subprocess
import sys
import tempfile

SCRIPT = pathlib.Path(__file__).resolve().parents[2] / 'scripts' / 'generate_label_indexes.py'


def write_file(path: pathlib.Path, contents: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(contents)


def write_disasm_fixtures(root: pathlib.Path) -> tuple[pathlib.Path, pathlib.Path]:
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
    write_file(
        jared_root / 'General' / 'WRAM.asm',
        'struct WRAM $7E0000\n'
        '{\n'
        '    ; $00[0x20] - (Main)\n'
        '    .Work: skip $20\n'
        '    ; $20[0x02] - (Player)\n'
        '    .PlayerYCoord: skip $02\n'
        '        ; Player\'s Y-Coordinate (mirrored at $0FC4).\n'
        '    ; $22[0x02] - (Player)\n'
        '    .PlayerXCoord:\n'
        '    ; $22[0x01] - (Attract)\n'
        '    .AttractModule: skip $01s\n'
        '    ; $24[0x02] - (Dungeon, Overworld)\n'
        '    .PlayerZCoord: skip $02\n'
        '    .Work: skip $01\n'
        '}\n'
        'endstruct\n'
    )
    return mon_root, jared_root


def disasm_args(root: pathlib.Path, mon_root: pathlib.Path, jared_root: pathlib.Path) -> list[str]:
    return [
        '--usdasm-root', str(root / 'missing-usdasm'),
        '--out-usdasm-index', str(root / 'label_index_usdasm.csv'),
        '--mon-root', str(mon_root),
        '--out-mon-index', str(root / 'label_index_mon.csv'),
        '--jared-root', str(jared_root),
        '--out-jared-index', str(root / 'label_index_jared.csv'),
        '--out-style-dir', str(root),
    ]


def style_map(root: pathlib.Path, name: str) -> dict:
    with (root / f'labels_{name}.csv').open() as handle:
        return {row['label']: row['address'] for row in csv.DictReader(handle)}


def assert_style_maps(root: pathlib.Path) -> None:
    mon = style_map(root, 'mon')
    assert mon == {'Intro_SetupScreen': '$02:8000', 'NMI_ReadJoypads': '$00:83D1'}, mon
    jared = style_map(root, 'jared')
    assert jared == {
        'Pool_Module_MainRouting': '$00:8061',
        'WRAM.Work': '$7E:0000',
        'WRAM.PlayerYCoord': '$7E:0020',
        'WRAM.PlayerXCoord': '$7E:0022',
        'WRAM.AttractModule': '$7E:0022',
        'WRAM.PlayerZCoord': '$7E:0024',
    }, jared

    mlb = (root / 'labels_jared.mlb').read_text().splitlines()
    assert mlb == [
        'SnesPrgRom:000061:Pool_Module_MainRouting',
        'SnesWorkRam:0000-001F:WRAM_Work:Main',
        'SnesWorkRam:0020-0021:WRAM_PlayerYCoord:Player',
        'SnesWorkRam:0022-0023:WRAM_PlayerXCoord:Player',
        'SnesWorkRam:0024-0025:WRAM_PlayerZCoord:Dungeon, Overworld',
    ], mlb
    assert (root / 'labels_mon.mlb').read_text().splitlines() == [
        'SnesPrgRom:0003D1:NMI_ReadJoypads',
        'SnesPrgRom:010000:Intro_SetupScreen',
    ]

    warnings = (root / 'labels_jared_wram_warnings.txt').read_text()
    assert "WRAM.AttractModule has unparsable 'skip $01s'" in warnings, warnings
    assert 'WRAM.PlayerZCoord header says $7E0024 but skips sum to $7E0023' in warnings, warnings
    assert 'WRAM.Work duplicates line' in warnings, warnings


def test_oracle_index_and_style_maps() -> None:
    with tempfile.TemporaryDirectory() as tmpdir:
        root = pathlib.Path(tmpdir)
        asm_root = root / 'asm'
        sym_path = root / 'test.sym'
        write_file(
            sym_path,
            '[labels]\n'
            '7E:E000 TimeState\n'
            '7E:E000 TimeState.Hours\n'
            '7E:E001 TimeState.Minutes\n'
        )
        write_file(
            asm_root / 'structs.asm',
            'struct TimeState $7EE000\n'
            '{\n'
            '  .Hours: skip 1\n'
            '  .Minutes: skip 1\n'
            '}\n'
            'endstruct\n'
        )
        mon_root, jared_root = write_disasm_fixtures(root)
        out_index = root / 'label_index.csv'

        subprocess.check_call([
            sys.executable,
            str(SCRIPT),
            '--sym', str(sym_path),
            '--asm-root', str(asm_root),
            '--out-overrides', str(root / 'label_overrides.csv'),
            '--out-index', str(out_index),
            '--out-all-index', str(root / 'label_index_all.csv'),
            '--out-merged-labels', str(root / 'labels_merged.csv'),
            *disasm_args(root, mon_root, jared_root),
        ])

        assert_style_maps(root)
        with out_index.open() as handle:
            labels = {(row.get('label') or '').strip() for row in csv.DictReader(handle)}
        missing = {'TimeState', 'TimeState.Hours', 'TimeState.Minutes'} - labels
        assert not missing, f'missing struct labels: {sorted(missing)}'


def test_disasm_only_skips_oracle_inputs() -> None:
    with tempfile.TemporaryDirectory() as tmpdir:
        root = pathlib.Path(tmpdir)
        mon_root, jared_root = write_disasm_fixtures(root)

        subprocess.check_call([
            sys.executable,
            str(SCRIPT),
            '--disasm-only',
            '--sym', str(root / 'missing.sym'),
            '--out-merged-labels', str(root / 'labels_merged.csv'),
            *disasm_args(root, mon_root, jared_root),
        ])

        assert_style_maps(root)
        assert not (root / 'labels_merged.csv').exists()


if __name__ == '__main__':
    test_oracle_index_and_style_maps()
    test_disasm_only_skips_oracle_inputs()
    print('label index tests: ok')
