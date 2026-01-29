#include "lsp_transport.h"
#include <iostream>
#include <string>
#include "logging.h"

namespace z3lsp {

std::optional<json> ReadMessage() {
  std::string line;
  int content_length = 0;
  while (std::getline(std::cin, line)) {
    if (line.rfind("Content-Length:", 0) == 0) {
      if (line.size() > 15) {
        content_length = std::stoi(line.substr(15));
      }
    } else if (line == "\r" || line.empty()) {
      break;
    }
  }

  if (content_length <= 0) {
    return std::nullopt;
  }

  std::string payload(content_length, '\0');
  std::cin.read(payload.data(), content_length);
  try {
    return json::parse(payload);
  } catch (const std::exception& e) {
    Log("LSP ReadMessage JSON parse error: " + std::string(e.what()));
    return std::nullopt;
  } catch (...) {
    Log("LSP ReadMessage JSON parse error: unknown exception");
    return std::nullopt;
  }
}

void SendMessage(const json& message) {
  std::string payload = message.dump();
  std::cout << "Content-Length: " << payload.size() << "\r\n\r\n" << payload;
  std::cout.flush();
}

}  // namespace z3lsp
