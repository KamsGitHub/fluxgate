#include "fluxgate/net/socket.hpp"

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <cerrno>
#include <string>
#include <system_error>

namespace fluxgate::net {
namespace {

[[noreturn]] void throw_errno(const std::string& what) {
  throw std::system_error(errno, std::generic_category(), what);
}

void disable_nagle(int fd) noexcept {
  // Best effort: a failure costs latency, not correctness. Heads and bodies are
  // often written separately, which Nagle plus delayed ACK would stall.
  const int enabled = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
}

}  // namespace

FileDescriptor listen_tcp(const SocketAddress& address, int backlog) {
  FileDescriptor fd(
      ::socket(address.storage.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  if (!fd) {
    throw_errno("socket");
  }
  const int enabled = 1;
  if (::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0) {
    throw_errno("setsockopt(SO_REUSEADDR)");
  }
  if (::bind(fd.get(), address.data(), address.length) != 0) {
    throw_errno("bind " + address.to_string());
  }
  if (::listen(fd.get(), backlog) != 0) {
    throw_errno("listen " + address.to_string());
  }
  return fd;
}

SocketAddress local_address(int fd) {
  SocketAddress address;
  address.length = sizeof(address.storage);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address.storage), &address.length) != 0) {
    throw_errno("getsockname");
  }
  return address;
}

AcceptOutcome accept_tcp(int listener_fd) {
  AcceptedSocket accepted;
  accepted.peer.length = sizeof(accepted.peer.storage);
  accepted.fd.reset(::accept4(listener_fd, reinterpret_cast<sockaddr*>(&accepted.peer.storage),
                              &accepted.peer.length, SOCK_NONBLOCK | SOCK_CLOEXEC));
  if (accepted.fd) {
    disable_nagle(accepted.fd.get());
    return {.socket = std::move(accepted), .error = 0};
  }
  switch (errno) {
    case EAGAIN:
    case EINTR:
    case ECONNABORTED:
    // Linux reports network errors already pending on the new socket through
    // accept(); accept(2) says to treat them like EAGAIN.
    case EPROTO:
    case ENETDOWN:
    case ENOPROTOOPT:
    case EHOSTDOWN:
    case EHOSTUNREACH:
    case EOPNOTSUPP:
    case ENETUNREACH:
      return {};
    default:
      return {.socket = std::nullopt, .error = errno};
  }
}

ConnectOutcome connect_tcp(const SocketAddress& address) {
  FileDescriptor fd(
      ::socket(address.storage.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  if (!fd) {
    return {.fd = {}, .error = errno};
  }
  disable_nagle(fd.get());
  if (::connect(fd.get(), address.data(), address.length) != 0 && errno != EINPROGRESS) {
    return {.fd = {}, .error = errno};
  }
  return {.fd = std::move(fd), .error = 0};
}

}  // namespace fluxgate::net
