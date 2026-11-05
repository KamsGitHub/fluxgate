#include "fluxgate/net/address.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>

#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace fluxgate::net {
namespace {

// Copying the sockaddr family out by value avoids the aliasing and alignment
// pitfalls of casting sockaddr_storage to the concrete type.
template <typename T>
T copy_as(const sockaddr_storage& storage) noexcept {
  T value{};
  std::memcpy(&value, &storage, sizeof(T));
  return value;
}

}  // namespace

const sockaddr* SocketAddress::data() const noexcept {
  return reinterpret_cast<const sockaddr*>(&storage);
}

std::string SocketAddress::ip() const {
  std::array<char, INET6_ADDRSTRLEN> text{};
  if (storage.ss_family == AF_INET) {
    const auto addr = copy_as<sockaddr_in>(storage);
    ::inet_ntop(AF_INET, &addr.sin_addr, text.data(), text.size());
  } else if (storage.ss_family == AF_INET6) {
    const auto addr = copy_as<sockaddr_in6>(storage);
    ::inet_ntop(AF_INET6, &addr.sin6_addr, text.data(), text.size());
  }
  return text.data();
}

std::uint16_t SocketAddress::port() const noexcept {
  if (storage.ss_family == AF_INET) {
    return ntohs(copy_as<sockaddr_in>(storage).sin_port);
  }
  if (storage.ss_family == AF_INET6) {
    return ntohs(copy_as<sockaddr_in6>(storage).sin6_port);
  }
  return 0;
}

std::string SocketAddress::to_string() const {
  const std::string host = ip();
  const std::string port_text = std::to_string(port());
  return storage.ss_family == AF_INET6 ? "[" + host + "]:" + port_text : host + ":" + port_text;
}

SocketAddress resolve(const std::string& host, std::uint16_t port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  addrinfo* raw_result = nullptr;
  const std::string service = std::to_string(port);
  if (const int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &raw_result); rc != 0) {
    throw std::runtime_error("cannot resolve '" + host + "': " + ::gai_strerror(rc));
  }
  const std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> result(raw_result, ::freeaddrinfo);

  SocketAddress address;
  std::memcpy(&address.storage, result->ai_addr, result->ai_addrlen);
  address.length = result->ai_addrlen;
  return address;
}

}  // namespace fluxgate::net
