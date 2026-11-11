# z3disasm - Zelda 3 Smart Disassembler

`z3disasm` turns a LoROM image into one `bank_XX.asm` file per 32 KB bank, naming
addresses from whatever symbol or label maps you give it.

## Build

```bash
cmake -S . -B build
cmake --build build --target z3disasm --config Release
```

The binary lands in `build/bin/z3disasm` (Visual Studio: `build/bin/Release/z3disasm.exe`).

## Usage

```bash
z3disasm --rom zelda3.sfc --out disasm/ [--labels labels.csv] [--symbols game.sym] \
         [--hooks [hooks.json]] [--bank-start 00] [--bank-end 1F] [--m-width 8|16] [--x-width 8|16]
```

- `--labels` / `--symbols` accept `.csv`, `.sym` (WLA) or `.mlb` (Mesen). Both can be given.
- `--bank-start` / `--bank-end` are hex bank numbers.
- `--m-width` / `--x-width` set the starting accumulator/index width (default 8-bit).
- A copier header (512 bytes) is stripped automatically. Only LoROM is supported.

## Disassembling with kan, MoN or Jared Brian labels

`scripts/generate_label_indexes.py --disasm-only` reads a vanilla disassembly checkout
and writes a `labels_<style>.csv` that `--labels` accepts:

| Style | Source | Address source |
| --- | --- | --- |
| `usdasm` | [spannerisms/usdasm](https://github.com/spannerisms/usdasm) (kan) | `#_BBAAAA:` line prefixes |
| `mon` | [walkingeyerobot/alttp-disassembly](https://github.com/walkingeyerobot/alttp-disassembly) (MathOnNapkins) | `; *$PPPPP-$PPPPP` file-offset comment above each routine |
| `jared` | [JaredBrian/AsarUSALTTPDisassembly](https://github.com/JaredBrian/AsarUSALTTPDisassembly) | `; $PPPPPP-$PPPPPP` file-offset comment above each routine |

```bash
python3 scripts/generate_label_indexes.py --disasm-only \
  --jared-root ../AsarUSALTTPDisassembly --out-style-dir labels/ \
  --out-jared-index labels/label_index_jared.csv
z3disasm --rom zelda3.sfc --labels labels/labels_jared.csv --out disasm/ --bank-start 00 --bank-end 00
```

Pass `--usdasm-root` and `--mon-root` the same way to build the other styles; roots that do
not exist are skipped. For MoN and Jared, only top-level labels that sit directly under a
file-offset comment get an address (local `.labels` and un-commented branch labels do not).
The label index CSV is cached against the checkout's file times, so delete it if you change
how labels are parsed.

Label CSV format (header row required, extra columns ignored):

```csv
address,label
$00:8000,Vector_Reset
```

## Output format

```asm
; bank $00
org $008000

Vector_Reset:
  SEI
  STZ $4200 ; NMITIMEN
  LDA #$80
  STA $2100 ; INIDISP
  JSR Sound_LoadIntroSongBank
```

- Labels print on their own line; operands that hit a labeled address use the label.
- Direct, absolute and long accesses to SNES I/O registers get a `; NAME` comment
  (PPU, CPU and DMA channel registers).
- `hooks.json` entries print as `; HOOK name [kind] -> target ...` above the hooked address.

## Limitations

- Every byte is decoded as code; data tables come out as instructions.
- Accumulator/index widths follow `REP`/`SEP`/`XCE` linearly through the bank, not per routine,
  so a wrong width can misalign decoding until the next `REP`/`SEP`.
- Addresses without a label stay as raw hex; z3disasm does not invent labels.

## Source layout

- **`main.cc`**: bank iteration and instruction output.
- **`options`**: command-line parsing.
- **`symbols`**: `.mlb`, `.sym` and `.csv` loading.
- **`hooks`**: `hooks.json` hook annotations.
- **`formatter`**: operand formatting and label substitution.
- **`utils`**: hex/int parsing, file reading, PC to LoROM conversion.
