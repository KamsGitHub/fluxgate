#include "fluxgate/net/tcp_stream.hpp"

#include <sys/socket.h>

#include <cerrno>
#include <cstring>

namespace fluxgate::net {

const char* error_description(int error) noexcept {
  const char* description = ::strerrordesc_np(error);
  return description != nullptr ? description : "unknown error";
}

IoResult TcpStream::read(std::span<std::byte> destination) {
  while (true) {
    const ssize_t n = ::recv(fd_.get(), destination.data(), destination.size(), 0);
    if (n > 0) {
      return {.status = IoStatus::Ok, .bytes = static_cast<std::size_t>(n), .error = 0};
    }
    if (n == 0) {
      return {.status = IoStatus::Eof, .bytes = 0, .error = 0};
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return {.status = IoStatus::WouldBlock, .bytes = 0, .error = 0};
    }
    return {.status = IoStatus::Error, .bytes = 0, .error = errno};
  }
}

IoResult TcpStream::write(std::span<const std::byte> source) {
  while (true) {
    // MSG_NOSIGNAL: a peer that has gone away must surface as EPIPE on this one
    // connection, not as a process-wide SIGPIPE.
    const ssize_t n = ::send(fd_.get(), source.data(), source.size(), MSG_NOSIGNAL);
    if (n >= 0) {
      return {.status = IoStatus::Ok, .bytes = static_cast<std::size_t>(n), .error = 0};
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return {.status = IoStatus::WouldBlock, .bytes = 0, .error = 0};
    }
    return {.status = IoStatus::Error, .bytes = 0, .error = errno};
  }
}

void TcpStream::shutdown_write() noexcept {
  ::shutdown(fd_.get(), SHUT_WR);
}

void TcpStream::abort_on_close() noexcept {
  const linger option{.l_onoff = 1, .l_linger = 0};
  ::setsockopt(fd_.get(), SOL_SOCKET, SO_LINGER, &option, sizeof(option));
}

int TcpStream::take_error() noexcept {
  int error = 0;
  socklen_t length = sizeof(error);
  if (::getsockopt(fd_.get(), SOL_SOCKET, SO_ERROR, &error, &length) != 0) {
    return errno;
  }
  return error;
}

}  // namespace fluxgate::net
