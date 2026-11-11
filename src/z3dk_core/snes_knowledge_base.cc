#include "snes_knowledge_base.h"
#include <algorithm>
#include <cctype>
#include <iterator>
#include <string>

namespace z3dk {

std::optional<OpcodeDocInfo> SnesKnowledgeBase::GetOpcodeInfo(std::string_view mnemonic) {
    // Linear scan is acceptable for < 100 items.
    // Case-insensitive comparison
    for (const auto& op : kOpcodeDocs) {
        std::string op_mnem = op.mnemonic;
        if (op_mnem.length() != mnemonic.length()) continue;
        
        bool match = true;
        for (size_t i = 0; i < mnemonic.length(); ++i) {
            if (std::toupper(op_mnem[i]) != std::toupper(mnemonic[i])) {
                match = false;
                break;
            }
        }
        
        if (match) {
            return op;
        }
    }
    return std::nullopt;
}

std::optional<SnesRegisterInfo> SnesKnowledgeBase::GetRegisterInfo(uint32_t address) {
    for (const auto& reg : kSnesRegisters) {
        if (reg.address == address) {
            return reg;
        }
    }
    return std::nullopt;
}

std::optional<SnesRegisterInfo> SnesKnowledgeBase::GetRegisterInfo(std::string_view name) {
    for (const auto& reg : kSnesRegisters) {
        std::string reg_name = reg.name;
        // Simple case insensitive check, could be optimized
        if (reg_name.length() != name.length()) continue;
        
        bool match = true;
        for (size_t i = 0; i < name.length(); ++i) {
            if (std::toupper(reg_name[i]) != std::toupper(name[i])) {
                match = false;
                break;
            }
        }
        
        if (match) {
            return reg;
        }
    }
    return std::nullopt;
}

std::span<const HardwareQuirk> SnesKnowledgeBase::GetQuirks() {

    return kHardwareQuirks;

}



namespace {

// Standard (fullsnes/Anomie) names; kSnesRegisters is OCR-derived and incomplete.
constexpr const char* kPpuRegisterNames[] = {
    "INIDISP", "OBSEL",   "OAMADDL", "OAMADDH", "OAMDATA", "BGMODE",  "MOSAIC",  "BG1SC",
    "BG2SC",   "BG3SC",   "BG4SC",   "BG12NBA", "BG34NBA", "BG1HOFS", "BG1VOFS", "BG2HOFS",
    "BG2VOFS", "BG3HOFS", "BG3VOFS", "BG4HOFS", "BG4VOFS", "VMAIN",   "VMADDL",  "VMADDH",
    "VMDATAL", "VMDATAH", "M7SEL",   "M7A",     "M7B",     "M7C",     "M7D",     "M7X",
    "M7Y",     "CGADD",   "CGDATA",  "W12SEL",  "W34SEL",  "WOBJSEL", "WH0",     "WH1",
    "WH2",     "WH3",     "WBGLOG",  "WOBJLOG", "TM",      "TS",      "TMW",     "TSW",
    "CGWSEL",  "CGADSUB", "COLDATA", "SETINI",  "MPYL",    "MPYM",    "MPYH",    "SLHV",
    "RDOAM",   "RDVRAML", "RDVRAMH", "RDCGRAM", "OPHCT",   "OPVCT",   "STAT77",  "STAT78",
    "APUIO0",  "APUIO1",  "APUIO2",  "APUIO3",
};

constexpr const char* kCpuRegisterNames[] = {
    "NMITIMEN", "WRIO",   "WRMPYA", "WRMPYB", "WRDIVL", "WRDIVH", "WRDIVB", "HTIMEL",
    "HTIMEH",   "VTIMEL", "VTIMEH", "MDMAEN", "HDMAEN", "MEMSEL", nullptr,  nullptr,
    "RDNMI",    "TIMEUP", "HVBJOY", "RDIO",   "RDDIVL", "RDDIVH", "RDMPYL", "RDMPYH",
    "JOY1L",    "JOY1H",  "JOY2L",  "JOY2H",  "JOY3L",  "JOY3H",  "JOY4L",  "JOY4H",
};

// '#' is replaced by the DMA channel number.
constexpr const char* kDmaRegisterNames[] = {
    "DMAP#", "BBAD#", "A1T#L", "A1T#H", "A1B#", "DAS#L", "DAS#H", "DASB#", "A2A#L", "A2A#H", "NTRL#",
};

static_assert(std::size(kPpuRegisterNames) == 0x44);
static_assert(std::size(kCpuRegisterNames) == 0x20);
static_assert(std::size(kDmaRegisterNames) == 0x0B);

std::string RegisterName(uint16_t addr) {
    if (addr >= 0x2100 && addr <= 0x2143) {
        return kPpuRegisterNames[addr - 0x2100];
    }
    switch (addr) {
        case 0x2180: return "WMDATA";
        case 0x2181: return "WMADDL";
        case 0x2182: return "WMADDM";
        case 0x2183: return "WMADDH";
        case 0x4016: return "JOYSER0";
        case 0x4017: return "JOYSER1";
        default: break;
    }
    if (addr >= 0x4200 && addr <= 0x421F) {
        const char* name = kCpuRegisterNames[addr - 0x4200];
        return name ? name : "";
    }
    if (addr >= 0x4300 && addr <= 0x437F && (addr & 0x0F) <= 0x0A) {
        std::string name = kDmaRegisterNames[addr & 0x0F];
        name[name.find('#')] = static_cast<char>('0' + ((addr >> 4) & 0x07));
        return name;
    }
    return "";
}

}  // namespace

std::string SnesKnowledgeBase::GetHardwareAnnotation(uint32_t address) {
    uint16_t low_addr = address & 0xFFFF;
    uint8_t bank = (address >> 16) & 0xFF;

    // SNES I/O is mirrored in banks $00-$3F and $80-$BF
    bool is_io_bank = (bank <= 0x3F) || (bank >= 0x80 && bank <= 0xBF);
    if (!is_io_bank) return "";

    std::string name = RegisterName(low_addr);
    return name.empty() ? "" : "; " + name;
}



} // namespace z3dk
