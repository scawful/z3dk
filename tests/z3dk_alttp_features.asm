;`+
;`00000 AF 20 00 7E 22 C3 C0 02 60
;`140C3 22 08 80 90
;`49A50 5C 17 80 90
;`80000 53 54 41 52 06 00 F9 FF A9 01 8F A0 00 7E 6B 53 54 41 52 09 00 F6 FF A9 05 8F 00 00 7E 5C 54 9A 09 00
;`FFFFF 00
; Test Z3DK Zelda Features
; Run with: build/bin/z3asm tests/z3dk_alttp_features.asm dummy.sfc

incsrc "../src/stdlib/alttp/all.asm"

org $808000 ; LoROM address
Main:
    LDA !LinkX
    JSL !Overworld_SetCameraBounds
    RTS

; Test the new HOOK directive
HOOK $02C0C3, JSL
    LDA #$01
    STA !RoomIndex
ENDHOOK

HOOK $099A50, JML
    ; Custom damage logic
    LDA #$05
    STA $7E0000
    JML $099A54 ; jump back to vanilla+offset
ENDHOOK
