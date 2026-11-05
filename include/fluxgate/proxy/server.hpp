#pragma once

#include <cstdint>

#include "fluxgate/config/config.hpp"
#include "fluxgate/net/address.hpp"
#include "fluxgate/net/file_descriptor.hpp"
#include "fluxgate/net/socket.hpp"
#include "fluxgate/proxy/session.hpp"

namespace fluxgate::proxy {

// Accepts connections and drives each ProxySession with poll(), one at a time.
//
// Serving a single connection at a time is the deliberate limit of this stage: while
// one session runs, others wait in the listen backlog, for at most exchange_timeout
// each. Replacing this class with an epoll event loop gives concurrency without
// touching the session, which already expresses all its waiting as data.
class Server {
 public:
  // Resolves the upstream and starts listening. Throws on failure. `stop_fd` is
  // watched but not owned; run() returns once it becomes readable.
  Server(const config::Config& config, int stop_fd);

  [[nodiscard]] std::uint16_t port() const noexcept { return listen_address_.port(); }

  void run();

 private:
  // Returns false if a stop was requested.
  bool serve(net::AcceptedSocket socket);
  void log_exchange(const SessionStats& stats, const std::string& client_ip,
                    Clock::duration elapsed) const;

  SessionOptions options_;
  net::SocketAddress upstream_address_;
  net::FileDescriptor listener_;
  net::SocketAddress listen_address_;
  int stop_fd_;
};

}  // namespace fluxgate::proxy
