// Workspace symbol index: workspace/symbol answers only from this index, so an
// empty index means every symbol search returns nothing.
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "parser.h"
#include "state.h"

namespace fs = std::filesystem;

#define ASSERT_TRUE(a)                                            \
  if (!(a)) {                                                     \
    std::cerr << "Assertion failed: " << #a << std::endl;         \
    std::exit(1);                                                 \
  }

static void Write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path) << text;
}

static bool Indexed(const z3lsp::WorkspaceState& state, const std::string& name) {
  for (const auto& [uri, symbols] : state.symbol_index) {
    for (const auto& symbol : symbols) {
      if (symbol.name == name) return true;
    }
  }
  return false;
}

int main() {
  fs::path root = fs::temp_directory_path() / "z3lsp_workspace_index_test";
  fs::remove_all(root);
  Write(root / "Core" / "main.asm", "MainLabel:\n  rts\n");
  Write(root / "Sprites" / "npc.inc", "NpcLabel:\n  rtl\n");
  Write(root / ".claude" / "worktrees" / "copy" / "Core" / "main.asm", "HiddenCopyLabel:\n  rts\n");
  Write(root / "nested" / ".git", "gitdir: /elsewhere\n");
  Write(root / "nested" / "main.asm", "NestedCheckoutLabel:\n  rts\n");

  z3lsp::WorkspaceState state;
  state.root = root;
  z3lsp::IndexWorkspaceSymbols(&state);

  ASSERT_TRUE(Indexed(state, "MainLabel"));
  ASSERT_TRUE(Indexed(state, "NpcLabel"));
  ASSERT_TRUE(state.symbol_names.count("MainLabel") == 1);
  ASSERT_TRUE(!Indexed(state, "HiddenCopyLabel"));
  ASSERT_TRUE(!Indexed(state, "NestedCheckoutLabel"));

  fs::remove_all(root);
  std::cout << "workspace index tests passed" << std::endl;
  return 0;
}
