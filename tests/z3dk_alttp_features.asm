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
