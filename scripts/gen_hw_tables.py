import json
import os
import re

def escape_cpp_string(s):
    if not s: return "nullptr"
    # Normalize unicode quotes to ascii
    s = s.replace('“', '"').replace('”', '"')
    # Escape backslashes first to avoid escaping the escapes
    s = s.replace('\\', '\\\\')
    # Escape quotes
    s = s.replace('"', '\\"')
    # Escape newlines
    s = s.replace('\n', '\\n')
    # Filter non-printable/weird characters to avoid compiler issues
    s = "".join(c for c in s if 32 <= ord(c) <= 126 or c in ('\\', 'n'))
    return f'"{s}"'

def sanitize_name(name):
    # Split by common separators (newline, forward slash, or " I " which is OCR for |)
    # Also handle "I " (I space) which appears in some OCR'd names like "BG12NBAI BG34NBA"
    parts = re.split(r'\n|/|\sI\s|I\s', name)
    for p in parts:
        # Keep alphanumeric
        clean = re.sub(r'[^A-Z0-9]', '', p.upper())
        if clean and not clean[0].isdigit():
            return clean
    return "UNKNOWN"

def main():
    try:
        # Load Registers
        with open('../../docs/reference/snes_registers.json', 'r') as f:
            registers = json.load(f)
        
        # ... (opcodes/quirks)
        # Load Opcodes
        with open('../../docs/reference/65816_opcodes.json', 'r') as f:
            opcodes = json.load(f)

        # Load Quirks
        with open('../../docs/reference/snes_quirks.json', 'r') as f:
            quirks = json.load(f)

        out_path = '../src/z3dk_core/snes_data.generated.h'
        
        seen_names = {} # name -> count

        with open(out_path, 'w') as f:
            f.write("#pragma once\n")
            # ...
            f.write("#include <cstdint>\n")
            f.write("#include <array>\n")
            f.write("#include <cstddef>\n\n")
            f.write("namespace z3dk {\n\n")

            # Registers
            f.write("struct SnesRegisterInfo {\n")
            f.write("    uint32_t address;\n")
            f.write("    const char* name;\n")
            f.write("    const char* description;\n")
            f.write("};\n\n")

            f.write(f"constexpr std::array<SnesRegisterInfo, {len(registers)}> kSnesRegisters = {{\n")
            for reg in registers:
                try:
                    addr_str = reg['address'].replace('$', '0x')
                    addr = int(addr_str, 16)
                    raw_name = reg['name']
                    is_read = raw_name.startswith('*')
                    name = sanitize_name(raw_name)
                    
                    if name in seen_names:
                        if is_read:
                            name = name + "_RD"
                        else:
                            name = name + "_WR"
                        # If still exists, append address
                        if name in seen_names:
                            name = name + "_" + addr_str.replace('0x', '')
                    
                    seen_names[name] = seen_names.get(name, 0) + 1
                    
                    clean_name = escape_cpp_string(name)
                    desc = escape_cpp_string(reg['description'])
                    f.write(f"    SnesRegisterInfo{{ {addr:#06x}, {clean_name}, {desc} }},\n")
                except ValueError:
                    print(f"Skipping register with invalid address: {reg['address']}")
            f.write("};\n\n")

            # Opcodes
            f.write("struct OpcodeDocInfo {\n")
            f.write("    const char* mnemonic;\n")
            f.write("    const char* full_name;\n")
            f.write("    const char* description;\n")
            f.write("    const char* flags;\n")
            f.write("    const char* timing_table;\n")
            f.write("};\n\n")

            f.write(f"constexpr std::array<OpcodeDocInfo, {len(opcodes)}> kOpcodeDocs = {{\n")
            for op in opcodes:
                mnem = escape_cpp_string(op['mnemonic'])
                fname = escape_cpp_string(op['full_name'])
                desc = escape_cpp_string(op['description'])
                flags = escape_cpp_string(op['flags'])
                timing = escape_cpp_string(op.get('codes_raw', ''))
                f.write(f"    OpcodeDocInfo{{ {mnem}, {fname}, {desc}, {flags}, {timing} }},\n")
            f.write("};\n\n")

            # Quirks
            f.write("struct HardwareQuirk {\n")
            f.write("    const char* type;\n")
            f.write("    const char* text;\n")
            f.write("    const char* source;\n")
            f.write("};\n\n")

            f.write(f"constexpr std::array<HardwareQuirk, {len(quirks)}> kHardwareQuirks = {{\n")
            for q in quirks:
                qtype = escape_cpp_string(q['type'])
                text = escape_cpp_string(q['text'])
                src = escape_cpp_string(q['source'])
                f.write(f"    HardwareQuirk{{ {qtype}, {text}, {src} }},\n")
            f.write("};\n\n")

            f.write("} // namespace z3dk\n")
            
        print(f"Successfully generated {out_path}")

    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    main()
