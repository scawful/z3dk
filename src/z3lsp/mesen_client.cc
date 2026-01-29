#include "mesen_client.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <cstring>
#include "logging.h"
#include "utils.h"

namespace z3lsp {

MesenClient g_mesen;

MesenClient::MesenClient() : socket_fd_(-1) {}
MesenClient::~MesenClient() { Disconnect(); }

bool MesenClient::Connect() {
  if (IsConnected()) return true;
  
  std::string path = FindLatestSocket();
  if (path.empty()) return false;

  socket_path_ = path;
  socket_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (socket_fd_ < 0) return false;

  int flags = fcntl(socket_fd_, F_GETFL, 0);
  fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK);

  struct sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

  if (connect(socket_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    if (errno != EINPROGRESS) {
      close(socket_fd_);
      socket_fd_ = -1;
      return false;
    }
    
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(socket_fd_, &wset);
    if (select(socket_fd_ + 1, NULL, &wset, NULL, &tv) <= 0) {
      close(socket_fd_);
      socket_fd_ = -1;
      return false;
    }
  }

  fcntl(socket_fd_, F_SETFL, flags);
  
  struct timeval rtv;
  rtv.tv_sec = 0;
  rtv.tv_usec = 200000;
  setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));

  return true;
}

void MesenClient::Disconnect() {
  if (socket_fd_ >= 0) {
    close(socket_fd_);
    socket_fd_ = -1;
  }
  socket_path_.clear();
}

bool MesenClient::IsConnected() const { return socket_fd_ >= 0; }

std::optional<uint8_t> MesenClient::ReadByte(uint32_t addr) {
  if (!Connect()) return std::nullopt;

  json cmd = {{"type", "READ"}, {"addr", "0x" + ToHexString(addr, 6)}};
  auto response = SendCommand(cmd);
  if (response.has_value() && response->value("success", false)) {
    return response->value("data", uint8_t(0));
  }
  return std::nullopt;
}

std::optional<json> MesenClient::SendCommand(const json& cmd) {
  if (!IsConnected()) return std::nullopt;

  std::string request = cmd.dump() + "\n";
  if (send(socket_fd_, request.c_str(), request.length(), 0) < 0) {
    Disconnect();
    return std::nullopt;
  }

  char buffer[4096];
  std::string response;
  ssize_t received = recv(socket_fd_, buffer, sizeof(buffer), 0);
  if (received <= 0) {
    Disconnect();
    return std::nullopt;
  }
  response.append(buffer, received);
  
  try {
    return json::parse(response);
  } catch (const std::exception& e) {
    Log("MesenClient JSON parse error: " + std::string(e.what()));
    return std::nullopt;
  } catch (...) {
    Log("MesenClient JSON parse error: unknown exception");
    return std::nullopt;
  }
}

std::string MesenClient::FindLatestSocket() {
  DIR* dir = opendir("/tmp");
  if (!dir) return "";

  std::string latest;
  long latest_mtime = 0;
  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    std::string name = entry->d_name;
    if (name.find("mesen2-") == 0 && name.find(".sock") != std::string::npos) {
      std::string full_path = "/tmp/" + name;
      struct stat st;
      if (stat(full_path.c_str(), &st) == 0) {
        if (st.st_mtime > latest_mtime) {
          latest_mtime = st.st_mtime;
          latest = full_path;
        }
      }
    }
  }
  closedir(dir);
  return latest;
}

}  // namespace z3lsp
