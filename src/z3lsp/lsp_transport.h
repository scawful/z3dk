#ifndef Z3LSP_LSP_TRANSPORT_H_
#define Z3LSP_LSP_TRANSPORT_H_

#include <optional>
#include <nlohmann/json.hpp>

namespace z3lsp {

using json = nlohmann::json;

std::optional<json> ReadMessage();
void SendMessage(const json& message);

}  // namespace z3lsp

#endif  // Z3LSP_LSP_TRANSPORT_H_
