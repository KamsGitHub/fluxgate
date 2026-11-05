#pragma once

#include "fluxgate/net/file_descriptor.hpp"
#include "fluxgate/net/stream.hpp"

namespace fluxgate::net {

class TcpStream final : public Stream {
 public:
  // `fd` must already be non-blocking.
  explicit TcpStream(FileDescriptor fd) noexcept : fd_(std::move(fd)) {}

  [[nodiscard]] IoResult read(std::span<std::byte> destination) override;
  [[nodiscard]] IoResult write(std::span<const std::byte> source) override;
  void shutdown_write() noexcept override;
  void abort_on_close() noexcept override;
  [[nodiscard]] int take_error() noexcept override;
  [[nodiscard]] int native_handle() const noexcept override { return fd_.get(); }

 private:
  FileDescriptor fd_;
};

}  // namespace fluxgate::net
