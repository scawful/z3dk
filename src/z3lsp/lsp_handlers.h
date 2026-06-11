#pragma once

#include <optional>
#include <unordered_map>

#include "nlohmann/json.hpp"
#include "state.h"

namespace z3lsp {

using json = nlohmann::json;

std::optional<json> HandleRename(const DocumentState& doc, WorkspaceState& workspace,
                                 std::unordered_map<std::string, DocumentState>& documents,
                                 const json& params);
std::optional<json> HandleDefinition(const DocumentState& doc, const json& params);
std::optional<json> HandleHover(const DocumentState& doc, const WorkspaceState& workspace, const json& params);

}  // namespace z3lsp
