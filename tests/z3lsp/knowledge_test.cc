
// Create a simple test runner since we don't have GTest
#include <iostream>
#include <vector>
#include <string>
#include <functional>
#include <filesystem>
#include <fstream>
#include "state.h"
#include "knowledge.h"

#define ASSERT_TRUE(a) \
    if (!(a)) { \
        std::cerr << "Assertion failed: " << #a << std::endl; \
        std::exit(1); \
    }

#define ASSERT_EQ(a, b) \
    if ((a) != (b)) { \
        std::cerr << "Assertion failed: " << #a << " == " << #b \
                  << " (" << (a) << " vs " << (b) << ")" << std::endl; \
        std::exit(1); \
    }

namespace fs = std::filesystem;

void TestLoadKnowledgeBase() {
    z3lsp::WorkspaceState workspace;
    workspace.root = fs::temp_directory_path();
    
    // Test default knowledge
    z3lsp::LoadKnowledgeBase(workspace);
    ASSERT_TRUE(!workspace.knowledge_base.empty());
    auto it = workspace.knowledge_base.find(0x008000); // Reset
    ASSERT_TRUE(it != workspace.knowledge_base.end());
    ASSERT_EQ(it->second.name, "Reset");

    // Test external knowledge
    fs::path knowledge_file = workspace.root / "z3dk.knowledge.json";
    std::ofstream f(knowledge_file);
    f << R"({
        "routines": [
            {
                "address": "0xC000",
                "name": "CustomRoutine",
                "description": "A test routine",
                "expects": "M=8"
            }
        ]
    })";
    f.close();

    z3lsp::LoadKnowledgeBase(workspace);
    
    // Should still have vanilla
    ASSERT_TRUE(workspace.knowledge_base.find(0x008000) != workspace.knowledge_base.end());
    
    // Should have custom
    auto custom_it = workspace.knowledge_base.find(0xC000);
    ASSERT_TRUE(custom_it != workspace.knowledge_base.end());
    ASSERT_EQ(custom_it->second.name, "CustomRoutine");
    ASSERT_EQ(custom_it->second.description, "A test routine");

    // Cleanup
    fs::remove(knowledge_file);
}

int main() {
    std::cout << "Running z3lsp knowledge tests..." << std::endl;
    TestLoadKnowledgeBase();
    std::cout << "All tests passed!" << std::endl;
    return 0;
}
