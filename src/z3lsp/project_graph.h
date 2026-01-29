#ifndef Z3LSP_PROJECT_GRAPH_H_
#define Z3LSP_PROJECT_GRAPH_H_

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <limits>
#include "state.h"

namespace z3lsp {

struct ProjectGraph {
  std::unordered_map<std::string, std::unordered_set<std::string>> child_to_parents;
  std::unordered_map<std::string, std::unordered_set<std::string>> parent_to_children;

  void RegisterDependency(const std::string& parent_uri, const std::string& child_uri);
  std::unordered_set<std::string> GetParents(const std::string& uri) const;
  std::unordered_map<std::string, int> GetAncestorDistances(const std::string& uri) const;
  std::string SelectRoot(const std::string& uri,
                         const std::unordered_set<std::string>& preferred_roots) const;
};

extern ProjectGraph g_project_graph;

}  // namespace z3lsp

#endif  // Z3LSP_PROJECT_GRAPH_H_
