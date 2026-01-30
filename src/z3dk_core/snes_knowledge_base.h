#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <span>
#include "snes_data.generated.h"

namespace z3dk {

class SnesKnowledgeBase {
public:
    static std::optional<OpcodeDocInfo> GetOpcodeInfo(std::string_view mnemonic);
    static std::optional<SnesRegisterInfo> GetRegisterInfo(uint32_t address);
    static std::optional<SnesRegisterInfo> GetRegisterInfo(std::string_view name);
    
    // Returns a span of all quirks. Spans are lightweight views.
    static std::span<const HardwareQuirk> GetQuirks();

    // Returns a short annotation like "; INIDISP ($2100)"
    static std::string GetHardwareAnnotation(uint32_t address);
};

} // namespace z3dk