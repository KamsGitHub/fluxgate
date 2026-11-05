#pragma once

#include <cstddef>
#include <string>

#include "fluxgate/core/byte_buffer.hpp"
#include "fluxgate/net/stream.hpp"

namespace fluxgate::proxy {

// Holds everything queued for one peer: a rendered head, then relayed body bytes.
//
// `buffer` doubles as the receive buffer of the opposite peer, so body bytes are read
// straight into it and need no copying. It is split into two regions:
//
//   [ validated: safe to send ][ unvalidated: not yet seen by the parser ]
//
// Only the validated region is ever written out, which is the code-level form of the
// rule that nothing goes to a peer until the parser has placed it inside a message.
class OutboundChannel {
 public:
  explicit OutboundChannel(std::size_t buffer_bytes) : buffer_(buffer_bytes) {}

  [[nodiscard]] core::ByteBuffer& buffer() noexcept { return buffer_; }
  [[nodiscard]] bool buffer_full() const noexcept { return buffer_.full(); }

  void queue_head(std::string_view rendered) { head_.append(rendered); }

  [[nodiscard]] std::span<const std::byte> unvalidated() const noexcept {
    return buffer_.readable().subspan(validated_);
  }
  // Marks the next `count` unvalidated bytes as part of the current message.
  void validate(std::size_t count) noexcept { validated_ += count; }
  // Drops the next `count` unvalidated bytes (consumed as a head).
  void drop_head_bytes(std::size_t count) noexcept;
  // Drops everything not yet validated (bytes past the end of the message).
  void drop_unvalidated() noexcept { buffer_.truncate(validated_); }
  // Drops everything validated (the peer can no longer accept it).
  void drop_validated() noexcept;

  [[nodiscard]] bool has_pending() const noexcept {
    return head_sent_ < head_.size() || validated_ > 0;
  }

  struct FlushResult {
    std::size_t bytes = 0;  // Includes head bytes.
    bool failed = false;
    int error = 0;
  };

  // Writes until the stream would block or nothing is left, coping with partial
  // writes by resuming exactly where the last one stopped.
  [[nodiscard]] FlushResult flush(net::Stream& stream);

 private:
  core::ByteBuffer buffer_;
  std::string head_;
  std::size_t head_sent_ = 0;
  std::size_t validated_ = 0;
};

}  // namespace fluxgate::proxy
