#include "snes_diagnostics.h"
#include "snes_knowledge_base.h"
#include <regex>
#include <sstream>

namespace z3dk {

std::vector<Diagnostic> DiagnoseRegisterQuirks(const std::string& text, const std::string& filename) {
    std::vector<Diagnostic> diags;
    // Look for store instructions to hex addresses: STA/STX/STY/STZ $xxxx
    // Match strict $XXXX format.
    std::regex store_regex(R"((STA|STX|STY|STZ)\s+\$([0-9A-Fa-f]{4}))", std::regex::icase);
    
    std::istringstream stream(text);
    std::string line;
    int line_num = 0;
    
    while (std::getline(stream, line)) {
        // Simple comment stripping
        std::string code_line = line;
        size_t comment_pos = code_line.find(';');
        if (comment_pos != std::string::npos) {
            code_line = code_line.substr(0, comment_pos);
        }

        std::smatch match;
        if (std::regex_search(code_line, match, store_regex)) {
            try {
                uint32_t addr = std::stoul(match[2].str(), nullptr, 16);
                auto reg_info = SnesKnowledgeBase::GetRegisterInfo(addr);
                
                if (reg_info.has_value() && reg_info->description) {
                    std::string desc = reg_info->description;
                    size_t note_pos = desc.find("NOTE:");
                    if (note_pos == std::string::npos) note_pos = desc.find("CAUTION:");
                    if (note_pos == std::string::npos) note_pos = desc.find("WARNING:");
                    
                    if (note_pos != std::string::npos) {
                        size_t end_pos = desc.find('\n', note_pos);
                        std::string note = desc.substr(note_pos, end_pos - note_pos);
                        if (note.length() > 100) note = note.substr(0, 97) + "...";
                        
                        Diagnostic d;
                        d.severity = DiagnosticSeverity::kWarning;
                        d.message = "Hardware Quirk (" + std::string(reg_info->name) + "): " + note;
                        d.line = line_num;
                        d.column = static_cast<int>(match.position(0));
                        d.filename = filename;
                        diags.push_back(d);
                    }
                }
            } catch (...) {}
        }
        line_num++;
    }
    return diags;
}

} // namespace z3dk
