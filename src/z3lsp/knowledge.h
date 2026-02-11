#ifndef Z3LSP_KNOWLEDGE_H_
#define Z3LSP_KNOWLEDGE_H_

#include <string>
#include <unordered_map>
#include <unordered_map>
#include <filesystem>

namespace z3lsp {

struct WorkspaceState;

struct KnowledgeEntry {
  std::string name;
  std::string description;
  std::string expected_state;
};

// Loads knowledge base from z3dk.knowledge.json or defaults
void LoadKnowledgeBase(WorkspaceState& workspace);

}  // namespace z3lsp

#endif  // Z3LSP_KNOWLEDGE_H_
