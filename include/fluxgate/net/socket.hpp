#pragma once

#include <optional>

#include "fluxgate/net/address.hpp"
#include "fluxgate/net/file_descriptor.hpp"

namespace fluxgate::net {

// All sockets created here are non-blocking and close-on-exec.

// Binds and listens. Throws std::system_error on failure.
[[nodiscard]] FileDescriptor listen_tcp(const SocketAddress& address, int backlog);

// Returns the address a socket is actually bound to (resolves port 0).
[[nodiscard]] SocketAddress local_address(int fd);

struct AcceptedSocket {
  FileDescriptor fd;
  SocketAddress peer;
};

struct AcceptOutcome {
  std::optional<AcceptedSocket> socket;
  // 0 when a socket was accepted, or when the queue was merely empty or the
  // connection vanished before accept (both harmless, the caller just retries).
  // Otherwise a resource errno such as EMFILE, which needs backoff: the listener
  // stays readable, so retrying at once would spin.
  int error = 0;
};

[[nodiscard]] AcceptOutcome accept_tcp(int listener_fd);

struct ConnectOutcome {
  FileDescriptor fd;
  int error = 0;  // Nonzero if the attempt failed before the handshake started.
};

// Starts a non-blocking connect. Success only means the handshake is under way:
// completion is signalled by writability, and the result read from SO_ERROR.
[[nodiscard]] ConnectOutcome connect_tcp(const SocketAddress& address);

}  // namespace fluxgate::net
