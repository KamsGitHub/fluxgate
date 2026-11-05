#include "fluxgate/http/parser.hpp"

#include <algorithm>

namespace fluxgate::http {
namespace {

// on_headers_complete return value meaning "this message has no body".
constexpr int kSkipBody = 1;

}  // namespace

// llhttp's C callbacks, kept here so that its API stays out of the class interface.
// Returning HPE_PAUSED makes llhttp_execute stop exactly at that position, which is
// how message boundaries are found without any copying.
struct Http1Parser::Callbacks {
  static Http1Parser& self(llhttp_t* parser) noexcept {
    return *static_cast<Http1Parser*>(parser->data);
  }

  static int on_method(llhttp_t* p, const char* at, std::size_t n) {
    self(p).head_.method.append(at, n);
    return 0;
  }
  static int on_url(llhttp_t* p, const char* at, std::size_t n) {
    self(p).head_.target.append(at, n);
    return 0;
  }
  static int on_status(llhttp_t* p, const char* at, std::size_t n) {
    self(p).head_.reason.append(at, n);
    return 0;
  }
  static int on_header_field(llhttp_t* p, const char* at, std::size_t n) {
    self(p).field_.append(at, n);
    return 0;
  }
  static int on_header_field_complete(llhttp_t* p) {
    Http1Parser& parser = self(p);
    if (parser.head_.headers.size() >= parser.limits_.max_headers) {
      parser.error_ = ParseError::TooManyHeaders;
      return -1;
    }
    parser.head_.headers.push_back({.name = std::move(parser.field_), .value = {}});
    parser.field_.clear();
    return 0;
  }
  static int on_header_value(llhttp_t* p, const char* at, std::size_t n) {
    self(p).head_.headers.back().value.append(at, n);
    return 0;
  }
  static int on_header_value_complete(llhttp_t* p) {
    // llhttp skips leading whitespace but keeps trailing whitespace, which RFC 9110
    // says is not part of the value.
    std::string& value = self(p).head_.headers.back().value;
    const std::size_t last = value.find_last_not_of(" \t");
    value.resize(last == std::string::npos ? 0 : last + 1);
    return 0;
  }
  static int on_headers_complete(llhttp_t* p) {
    Http1Parser& parser = self(p);
    parser.head_.version_major = p->http_major;
    parser.head_.version_minor = p->http_minor;
    if (p->type == HTTP_RESPONSE) {
      parser.head_.status = p->status_code;
    }
    if (parser.head_.find("Content-Length") != nullptr) {
      parser.head_.content_length = p->content_length;
    }
    parser.head_ready_ = true;
    // FluxGate never honours protocol upgrades: Upgrade and Connection are removed
    // before forwarding, so without this llhttp would stop at the end of the message
    // and treat what follows as a foreign protocol.
    p->upgrade = 0;
    if (parser.skip_body_) {
      // Pausing here is not possible, since that would discard the skip request.
      // The message completes immediately, and feed() reports both events in turn.
      return kSkipBody;
    }
    return HPE_PAUSED;
  }
  static int on_message_complete(llhttp_t* p) {
    self(p).message_ready_ = true;
    return HPE_PAUSED;
  }

  static const llhttp_settings_t& settings() noexcept {
    static const llhttp_settings_t instance = [] {
      llhttp_settings_t s{};
      llhttp_settings_init(&s);
      s.on_method = on_method;
      s.on_url = on_url;
      s.on_status = on_status;
      s.on_header_field = on_header_field;
      s.on_header_field_complete = on_header_field_complete;
      s.on_header_value = on_header_value;
      s.on_header_value_complete = on_header_value_complete;
      s.on_headers_complete = on_headers_complete;
      s.on_message_complete = on_message_complete;
      return s;
    }();
    return instance;
  }
};

Http1Parser::Http1Parser(Mode mode, HeadLimits limits) : limits_(limits) {
  // No llhttp_set_lenient_* call is ever made: each one relaxes a framing rule that
  // request-smuggling attacks depend on.
  llhttp_init(&parser_, mode == Mode::Request ? HTTP_REQUEST : HTTP_RESPONSE,
              &Callbacks::settings());
  parser_.data = this;
}

FeedResult Http1Parser::fail(ParseError error) noexcept {
  // A callback may already have stored a more specific reason.
  if (error_ == ParseError::None) {
    error_ = error;
  }
  return {.consumed = 0, .event = ParseEvent::Error};
}

FeedResult Http1Parser::feed(std::span<const std::byte> data) {
  if (error_ != ParseError::None) {
    return {.consumed = 0, .event = ParseEvent::Error};
  }
  if (message_ready_ && head_reported_) {
    return {.consumed = 0, .event = ParseEvent::MessageComplete};
  }

  // Offering no more than the remaining head allowance enforces the limit exactly,
  // whatever the fragmentation, and before any oversized field is accumulated.
  std::size_t length = data.size();
  if (!head_ready_) {
    length = std::min(length, limits_.max_head_bytes - head_bytes_);
  }

  static constexpr char kEmpty = '\0';  // llhttp needs a valid pointer even for no data.
  const char* begin = data.empty() ? &kEmpty : reinterpret_cast<const char*>(data.data());

  if (paused_) {
    llhttp_resume(&parser_);
    paused_ = false;
  }

  const llhttp_errno_t rc = llhttp_execute(&parser_, begin, length);
  if (rc != HPE_OK && rc != HPE_PAUSED) {
    return fail(ParseError::Malformed);
  }

  std::size_t consumed = length;
  if (rc == HPE_PAUSED) {
    paused_ = true;
    consumed = static_cast<std::size_t>(llhttp_get_error_pos(&parser_) - begin);
  }

  if (!head_ready_) {
    head_bytes_ += consumed;
    if (head_bytes_ >= limits_.max_head_bytes) {
      return fail(ParseError::HeadTooLarge);
    }
    return {.consumed = consumed, .event = ParseEvent::NeedMore};
  }
  if (!head_reported_) {
    head_reported_ = true;
    return {.consumed = consumed, .event = ParseEvent::HeadComplete};
  }
  if (message_ready_) {
    return {.consumed = consumed, .event = ParseEvent::MessageComplete};
  }
  return {.consumed = consumed, .event = ParseEvent::NeedMore};
}

bool Http1Parser::finish() {
  if (message_ready_) {
    return true;
  }
  if (paused_) {
    llhttp_resume(&parser_);
    paused_ = false;
  }
  const llhttp_errno_t rc = llhttp_finish(&parser_);
  // For close-delimited bodies llhttp runs on_message_complete from llhttp_finish,
  // so the pause requested by that callback comes back as the return value.
  if ((rc == HPE_OK || rc == HPE_PAUSED) && message_ready_) {
    return true;
  }
  if (error_ == ParseError::None) {
    error_ = ParseError::Truncated;
  }
  return false;
}

void Http1Parser::reset() {
  llhttp_reset(&parser_);
  head_ = MessageHead{};
  field_.clear();
  head_bytes_ = 0;
  error_ = ParseError::None;
  skip_body_ = false;
  paused_ = false;
  head_ready_ = false;
  head_reported_ = false;
  message_ready_ = false;
}

const char* Http1Parser::error_reason() const noexcept {
  switch (error_) {
    case ParseError::None:
      return "none";
    case ParseError::HeadTooLarge:
      return "head exceeds size limit";
    case ParseError::TooManyHeaders:
      return "too many headers";
    case ParseError::Truncated:
      return "connection closed before end of message";
    case ParseError::Malformed:
      return llhttp_get_error_reason(&parser_);
  }
  return "unknown";
}

}  // namespace fluxgate::http
