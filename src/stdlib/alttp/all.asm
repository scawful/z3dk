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
