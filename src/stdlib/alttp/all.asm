; Z3DK Zelda Standard Library - Main Entry (ALTTP)

incsrc "ram.asm"
incsrc "rom.asm"

macro PushMX()
    PHP
endmacro

macro PopMX()
    PLP
endmacro

macro SetM8()
    SEP #$20
endmacro

macro SetM16()
    REP #$20
endmacro

macro SetX8()
    SEP #$10
endmacro

macro SetX16()
    REP #$10
endmacro

macro SetMX8()
    SEP #$30
endmacro

macro SetMX16()
    REP #$30
endmacro

; --- Long call entry/exit ---
; Use at the start of routines called via JSL/JML.
; Sets the data bank to the current code bank.

macro LongEntry()
    PHB : PHK : PLB
endmacro

macro LongExit()
    PLB : RTL
endmacro

; Safe variant that preserves and restores processor flags.
; Use when called from unknown M/X state.

macro LongEntrySafe()
    PHP : PHB : PHK : PLB
endmacro

macro LongExitSafe()
    PLB : PLP : RTL
endmacro

; --- Sprite routine entry/exit ---
; Standard pattern for sprite Long routines (vanilla ABI).

macro SpriteEntry()
    PHB : PHK : PLB
endmacro

macro SpriteExit()
    PLB : RTL
endmacro
