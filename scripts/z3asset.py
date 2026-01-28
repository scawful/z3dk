#!/usr/bin/env python3
import sys
import os
import argparse
from pathlib import Path

# Zelda 3 Character Table (Partial/Sample)
CHAR_TABLE = {
    ' ': 0xFF,
    'A': 0x00, 'B': 0x01, 'C': 0x02, 'D': 0x03, 'E': 0x04, 'F': 0x05, 'G': 0x06,
    'H': 0x07, 'I': 0x08, 'J': 0x09, 'K': 0x0A, 'L': 0x0B, 'M': 0x0C, 'N': 0x0D,
    'O': 0x0E, 'P': 0x0F, 'Q': 0x10, 'R': 0x11, 'S': 0x12, 'T': 0x13, 'U': 0x14,
    'V': 0x15, 'W': 0x16, 'X': 0x17, 'Y': 0x18, 'Z': 0x19,
    'a': 0x1A, 'b': 0x1B, 'c': 0x1C, 'd': 0x1D, 'e': 0x1E, 'f': 0x1F, 'g': 0x20,
    'h': 0x21, 'i': 0x22, 'j': 0x23, 'k': 0x24, 'l': 0x25, 'm': 0x26, 'n': 0x27,
    'o': 0x28, 'p': 0x29, 'q': 0x2A, 'r': 0x2B, 's': 0x2C, 't': 0x2D, 'u': 0x2E,
    'v': 0x2F, 'w': 0x30, 'x': 0x31, 'y': 0x32, 'z': 0x33,
    '0': 0x34, '1': 0x35, '2': 0x36, '3': 0x37, '4': 0x38, '5': 0x39, '6': 0x3A,
    '7': 0x3B, '8': 0x3C, '9': 0x3D,
    '!': 0x3E, '?': 0x3F, ',': 0x40, '.': 0x41, '-': 0x42, ':': 0x43,
}

def encode_msg(text):
    output = bytearray()
    for char in text:
        if char in CHAR_TABLE:
            output.append(CHAR_TABLE[char])
        else:
            output.append(0x41) # Default to '.'
    return output

def convert_to_4bpp(input_path):
    # This is a placeholder for a real 4bpp conversion.
    # In a real tool, we'd use Pillow to read the PNG.
    # For now, we'll return a mock pattern or read raw if it's already converted.
    if input_path.endswith('.bin'):
        with open(input_path, 'rb') as f:
            return f.read()
    
    # Mock 4bpp data (all pixels = index 1)
    # A tile is 8x8 pixels. 4bpp = 32 bytes per tile.
    return bytes([0xAA] * 32)

def main():
    parser = argparse.ArgumentParser(description="Z3DK Asset Tool")
    subparsers = parser.add_subparsers(dest="command")

    gfx_parser = subparsers.add_parser("gfx")
    gfx_parser.add_argument("input")
    gfx_parser.add_argument("format")
    gfx_parser.add_argument("output")

    msg_parser = subparsers.add_parser("msg")
    msg_parser.add_argument("text")
    msg_parser.add_argument("output")

    args = parser.parse_args()

    if args.command == "gfx":
        data = convert_to_4bpp(args.input)
        os.makedirs(os.path.dirname(args.output), exist_ok=True)
        with open(args.output, "wb") as f:
            f.write(data)
    elif args.command == "msg":
        data = encode_msg(args.text)
        os.makedirs(os.path.dirname(args.output), exist_ok=True)
        with open(args.output, "wb") as f:
            f.write(data)

if __name__ == "__main__":
    main()
