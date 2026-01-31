; Asar compat: org + db (LoROM default)
; Expected: single byte $42 at ROM offset 0 (SNES $808000)
lorom
org $8000
db $42
