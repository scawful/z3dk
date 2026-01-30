#include "snes_knowledge_base.h"
#include <algorithm>
#include <cctype>

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



std::string SnesKnowledgeBase::GetHardwareAnnotation(uint32_t address) {
    uint32_t low_addr = address & 0xFFFF;
    uint8_t bank = (address >> 16) & 0xFF;
    
    // SNES I/O is mirrored in banks $00-$3F and $80-$BF
    bool is_io_bank = (bank <= 0x3F) || (bank >= 0x80 && bank <= 0xBF);
    if (!is_io_bank) return "";
    
    // Check if the low address matches a known register
    // Note: Our DB keys are 16-bit addresses like 0x2100
    auto info = GetRegisterInfo(low_addr);
    if (info.has_value()) {
        std::string result = "; ";
        result += info->name;
        return result;
    }
    return "";
}



} // namespace z3dk
