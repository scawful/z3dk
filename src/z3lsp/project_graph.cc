#include "project_graph.h"

namespace z3lsp {

ProjectGraph g_project_graph;

void ProjectGraph::RegisterDependency(const std::string& parent_uri, const std::string& child_uri) {
  child_to_parents[child_uri].insert(parent_uri);
  parent_to_children[parent_uri].insert(child_uri);
}

std::unordered_set<std::string> ProjectGraph::GetParents(const std::string& uri) const {
  auto it = child_to_parents.find(uri);
  if (it == child_to_parents.end()) {
    return {};
  }
  return it->second;
}

std::unordered_map<std::string, int> ProjectGraph::GetAncestorDistances(const std::string& uri) const {
  std::unordered_map<std::string, int> distances;
  if (uri.empty()) {
    return distances;
  }
  std::queue<std::string> pending;
  distances[uri] = 0;
  pending.push(uri);
  while (!pending.empty()) {
    std::string current = pending.front();
    pending.pop();
    int current_distance = distances[current];
    auto it = child_to_parents.find(current);
    if (it == child_to_parents.end()) {
      continue;
    }
    for (const auto& parent : it->second) {
      if (distances.find(parent) != distances.end()) {
        continue;
      }
      distances[parent] = current_distance + 1;
      pending.push(parent);
    }
  }
  return distances;
}

std::string ProjectGraph::SelectRoot(const std::string& uri,
                       const std::unordered_set<std::string>& preferred_roots) const {
  if (uri.empty()) {
    return uri;
  }
  auto distances = GetAncestorDistances(uri);
  if (distances.empty()) {
    return uri;
  }

  auto pick_best = [&](const std::vector<std::string>& candidates) -> std::string {
    std::string best;
    int best_distance = std::numeric_limits<int>::max();
    for (const auto& candidate : candidates) {
      auto it = distances.find(candidate);
      if (it == distances.end()) {
        continue;
      }
      int distance = it->second;
      if (distance < best_distance ||
          (distance == best_distance && (best.empty() || candidate < best))) {
        best = candidate;
        best_distance = distance;
      }
    }
    return best.empty() ? uri : best;
  };

  if (!preferred_roots.empty()) {
    std::vector<std::string> preferred;
    preferred.reserve(preferred_roots.size());
    for (const auto& entry : distances) {
      if (preferred_roots.count(entry.first)) {
        preferred.push_back(entry.first);
      }
    }
    if (!preferred.empty()) {
      return pick_best(preferred);
    }
  }

  std::vector<std::string> roots;
  roots.reserve(distances.size());
  for (const auto& entry : distances) {
    auto it = child_to_parents.find(entry.first);
    if (it == child_to_parents.end() || it->second.empty()) {
      roots.push_back(entry.first);
    }
  }
  if (!roots.empty()) {
    return pick_best(roots);
  }
  return uri;
}

}  // namespace z3lsp
