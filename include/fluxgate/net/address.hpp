#pragma once

#include <sys/socket.h>

#include <cstdint>
#include <string>

namespace fluxgate::net {

// An IPv4 or IPv6 endpoint in the form the socket syscalls consume.
struct SocketAddress {
  sockaddr_storage storage{};
  socklen_t length = 0;

  [[nodiscard]] const sockaddr* data() const noexcept;
  [[nodiscard]] std::string ip() const;
  [[nodiscard]] std::uint16_t port() const noexcept;
  // "1.2.3.4:80" or "[::1]:80".
  [[nodiscard]] std::string to_string() const;
};

// Resolves `host` to its first usable address. This blocks in getaddrinfo(), so
// it must only run during startup, never on the data path.
// Throws std::runtime_error if the name cannot be resolved.
[[nodiscard]] SocketAddress resolve(const std::string& host, std::uint16_t port);

}  // namespace fluxgate::net
