#include <cstdlib>
#include <iostream>
#include <string>

#include "snes_knowledge_base.h"

using z3dk::SnesKnowledgeBase;

namespace {

int failures = 0;

void ExpectAnnotation(uint32_t address, const std::string& expected) {
    std::string actual = SnesKnowledgeBase::GetHardwareAnnotation(address);
    if (actual != expected) {
        std::cerr << "GetHardwareAnnotation($" << std::hex << address << ") = \"" << actual
                  << "\", expected \"" << expected << "\"\n";
        ++failures;
    }
}

}  // namespace

int main() {
    // I/O is mirrored in banks $00-$3F and $80-$BF.
    ExpectAnnotation(0x012100, "; INIDISP");
    ExpectAnnotation(0x802100, "; INIDISP");
    ExpectAnnotation(0x812100, "; INIDISP");
    ExpectAnnotation(0x402100, "");

    ExpectAnnotation(0x002105, "; BGMODE");
    ExpectAnnotation(0x002140, "; APUIO0");
    ExpectAnnotation(0x002180, "; WMDATA");
    ExpectAnnotation(0x004200, "; NMITIMEN");
    ExpectAnnotation(0x00420B, "; MDMAEN");
    ExpectAnnotation(0x00420E, "");
    ExpectAnnotation(0x004218, "; JOY1L");

    ExpectAnnotation(0x004300, "; DMAP0");
    ExpectAnnotation(0x004312, "; A1T1L");
    ExpectAnnotation(0x004327, "; DASB2");
    ExpectAnnotation(0x00437A, "; NTRL7");
    ExpectAnnotation(0x00437B, "");

    if (failures != 0) {
        return EXIT_FAILURE;
    }
    std::cout << "annotation test: ok\n";
    return EXIT_SUCCESS;
}
