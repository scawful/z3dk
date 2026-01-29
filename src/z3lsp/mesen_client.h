#ifndef Z3LSP_MESEN_CLIENT_H_
#define Z3LSP_MESEN_CLIENT_H_

#include <string>
#include <optional>
#include <nlohmann/json.hpp>

namespace z3lsp {

using json = nlohmann::json;

class MesenClient {
 public:
  MesenClient();
  ~MesenClient();

  bool Connect();
  void Disconnect();
  bool IsConnected() const;

  std::optional<uint8_t> ReadByte(uint32_t addr);
  std::optional<json> SendCommand(const json& cmd);

 private:
  std::string FindLatestSocket();

  int socket_fd_;
  std::string socket_path_;
};

extern MesenClient g_mesen;

}  // namespace z3lsp

#endif  // Z3LSP_MESEN_CLIENT_H_
