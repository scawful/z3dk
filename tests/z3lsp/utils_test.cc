// Create a simple test runner since we don't have GTest
#include <iostream>
#include <vector>
#include <string>
#include <functional>

#define ASSERT_EQ(a, b) \
    if ((a) != (b)) { \
        std::cerr << "Assertion failed: " << #a << " == " << #b \
                  << " (" << (a) << " vs " << (b) << ")" << std::endl; \
        std::exit(1); \
    }

#define ASSERT_TRUE(a) \
    if (!(a)) { \
        std::cerr << "Assertion failed: " << #a << std::endl; \
        std::exit(1); \
    }

// Forward declarations
void TestFindReferences();

int main() {
    std::cout << "Running z3lsp utils tests..." << std::endl;
    TestFindReferences();
    std::cout << "All tests passed!" << std::endl;
    return 0;
}

#include "utils.h"

void TestFindReferences() {
    std::string text = R"(
Label:
    lda #$00
    jsr Label
    ; This is a comment about Label
    db "String with Label inside"
    dw Label
)";
    std::string token = "Label";
    /*
    Expected matches:
    Line 1: Label:
    Line 3: jsr Label
    Line 6: dw Label
    
    Ignored:
    Line 4: ; comment
    Line 5: "string"
    */

    auto refs = z3lsp::FindReferencesInText(text, token);
    
    ASSERT_EQ(refs.size(), 3);
    
    // Check line numbers (0-indexed)
    // Label: (line 1, which is index 1 because R"( starts with newline?)
    // Actually R"( followed by newline means first char is newline.
    // Line 0: empty (newline)
    // Line 1: Label:
    ASSERT_EQ(refs[0].line, 1);
    ASSERT_EQ(refs[0].column, 0);

    // Line 3: jsr Label (4 spaces indentation)
    ASSERT_EQ(refs[1].line, 3);
    
    // Line 6: dw Label
    ASSERT_EQ(refs[2].line, 6);
}
