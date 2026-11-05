#pragma once

#include <llhttp.h>

#include <cstddef>
#include <span>
#include <string>

#include "fluxgate/http/message.hpp"

namespace fluxgate::http {

struct HeadLimits {
  std::size_t max_head_bytes = std::size_t{16} * 1024;  // Start line plus all header lines.
  std::size_t max_headers = 100;
};

enum class ParseEvent {
  NeedMore,         // Everything offered was consumed; the message is still open.
  HeadComplete,     // head() is ready. `consumed` ends exactly after the blank line.
  MessageComplete,  // `consumed` ends exactly at the last byte of this message.
  Error,            // Terminal; see error().
};

enum class ParseError {
  None,
  Malformed,
  HeadTooLarge,
  TooManyHeaders,
  Truncated,  // The peer closed before the framing said the message had ended.
};

struct FeedResult {
  std::size_t consumed = 0;
  ParseEvent event = ParseEvent::NeedMore;
};

// Incremental, sans-IO HTTP/1.x parser around llhttp, in its strict mode.
//
// It reports message boundaries rather than delivering decoded bodies. The caller
// keeps the raw wire bytes, and bytes offered after MessageComplete are never
// consumed: forwarding only `consumed` bytes is what guarantees that nothing past
// a validated message boundary can reach the other peer.
//
// After HeadComplete the caller must call feed() again even with no new bytes,
// because a message without a body completes without consuming any input.
//
// llhttp keeps a pointer back to this object, so it can be neither copied nor moved.
class Http1Parser {
 public:
  enum class Mode { Request, Response };

  Http1Parser(Mode mode, HeadLimits limits);
  ~Http1Parser() = default;
  Http1Parser(const Http1Parser&) = delete;
  Http1Parser& operator=(const Http1Parser&) = delete;

  [[nodiscard]] FeedResult feed(std::span<const std::byte> data);

  // Signals EOF from the peer. Returns true if that legitimately ends the message,
  // as with a close-delimited response. Otherwise records ParseError::Truncated.
  [[nodiscard]] bool finish();

  // Responses only: the next message has no body whatever its headers say, as for
  // the response to a HEAD request. Call before feeding the response.
  void expect_no_body() noexcept { skip_body_ = true; }

  // Forgets the current message and starts on a new one.
  void reset();

  [[nodiscard]] const MessageHead& head() const noexcept { return head_; }
  [[nodiscard]] ParseError error() const noexcept { return error_; }
  // Static description of the failure, safe to log.
  [[nodiscard]] const char* error_reason() const noexcept;

 private:
  struct Callbacks;

  [[nodiscard]] FeedResult fail(ParseError error) noexcept;

  llhttp_t parser_{};
  HeadLimits limits_;
  MessageHead head_;
  std::string field_;
  std::size_t head_bytes_ = 0;
  ParseError error_ = ParseError::None;
  bool skip_body_ = false;
  bool paused_ = false;
  bool head_ready_ = false;
  bool head_reported_ = false;
  bool message_ready_ = false;
};

}  // namespace fluxgate::http
