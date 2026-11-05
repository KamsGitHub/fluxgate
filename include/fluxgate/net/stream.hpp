#pragma once

#include <cstddef>
#include <span>

namespace fluxgate::net {

// Thread-safe description of an errno value, with static storage duration.
[[nodiscard]] const char* error_description(int error) noexcept;

enum class IoStatus {
  Ok,          // `bytes` were transferred; may be fewer than requested.
  WouldBlock,  // Nothing transferred; wait for readiness and retry.
  Eof,         // Peer finished sending (reads only).
  Error,       // Connection failed; `error` holds the errno.
};

struct IoResult {
  IoStatus status = IoStatus::WouldBlock;
  std::size_t bytes = 0;
  int error = 0;
};

// A non-blocking byte stream. The session logic is written against this rather
// than a socket so tests can script short writes and EAGAIN exactly, and so TLS
// can later be added as another implementation without touching protocol code.
class Stream {
 public:
  Stream() = default;
  virtual ~Stream() = default;
  Stream(const Stream&) = delete;
  Stream& operator=(const Stream&) = delete;

  [[nodiscard]] virtual IoResult read(std::span<std::byte> destination) = 0;
  [[nodiscard]] virtual IoResult write(std::span<const std::byte> source) = 0;

  // Sends FIN; the receive direction stays open.
  virtual void shutdown_write() noexcept = 0;

  // Arranges for the eventual close to send RST instead of FIN, which is the only
  // way to tell a peer that a close-delimited response was cut short.
  virtual void abort_on_close() noexcept = 0;

  // Clears and returns SO_ERROR. Used to learn how a connect attempt ended.
  [[nodiscard]] virtual int take_error() noexcept = 0;

  // Descriptor to wait on; never owned by the caller.
  [[nodiscard]] virtual int native_handle() const noexcept = 0;
};

}  // namespace fluxgate::net
