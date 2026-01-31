#include <gtest/gtest.h>
#include "z3dk_core/snes_diagnostics.h"
#include "z3dk_core/snes_knowledge_base.h"

TEST(SnesDiagnosticsTest, DetectsQuirkInMirroredBanks) {
    // CGDATA ($2122) in bank $80
    std::string code = "STA $802122\n";
    auto diags = z3dk::DiagnoseRegisterQuirks(code, "test.asm");
    
    ASSERT_EQ(diags.size(), 1);
    EXPECT_NE(diags[0].message.find("CGDATA"), std::string::npos);
}

TEST(SnesDiagnosticsTest, DetectsQuirkWithIndexedAddressing) {
    // STA $2122,X
    std::string code = "STA $2122,X\n";
    auto diags = z3dk::DiagnoseRegisterQuirks(code, "test.asm");
    
    ASSERT_EQ(diags.size(), 1);
    EXPECT_NE(diags[0].message.find("CGDATA"), std::string::npos);
}

TEST(SnesDiagnosticsTest, DetectsQuirkWithLongPrefix) {
    // STA >$2122
    std::string code = "STA >$2122\n";
    auto diags = z3dk::DiagnoseRegisterQuirks(code, "test.asm");
    
    ASSERT_EQ(diags.size(), 1);
}

