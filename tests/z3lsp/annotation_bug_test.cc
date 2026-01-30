#include <gtest/gtest.h>
#include "z3dk_core/snes_knowledge_base.h"

using namespace z3dk;

TEST(AnnotationBugTest, AnnotatesMirroredBanks) {
    // Bank $01 is a mirror of $00 for I/O range
    uint32_t addr_bank01 = 0x012100; 
    EXPECT_EQ(SnesKnowledgeBase::GetHardwareAnnotation(addr_bank01), "; INIDISP");

    // Bank $80 is a mirror
    uint32_t addr_bank80 = 0x802100;
    EXPECT_EQ(SnesKnowledgeBase::GetHardwareAnnotation(addr_bank80), "; INIDISP");

    // Bank $81 is a mirror
    uint32_t addr_bank81 = 0x812100;
    EXPECT_EQ(SnesKnowledgeBase::GetHardwareAnnotation(addr_bank81), "; INIDISP");
}

TEST(AnnotationBugTest, DoesNotAnnotateRomBanks) {
    // Bank $40 is usually HiROM/ExHiROM, not I/O mirror (unless specific mapping)
    // But standard LoROM/HiROM maps $40-$7D as pure ROM/RAM, no I/O registers at $2100.
    // I/O is strictly $00-$3F and $80-$BF.
    
    uint32_t addr_bank40 = 0x402100;
    EXPECT_EQ(SnesKnowledgeBase::GetHardwareAnnotation(addr_bank40), "");
}
