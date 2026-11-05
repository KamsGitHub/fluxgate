#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "fluxgate/http/parser.hpp"
#include "fluxgate/net/stream.hpp"
#include "fluxgate/proxy/outbound_channel.hpp"

namespace fluxgate::proxy {

using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;

struct SessionOptions {
  http::HeadLimits head_limits;
  std::uint64_t max_request_body_bytes = 10ULL * 1024 * 1024;
  std::size_t buffer_bytes = std::size_t{64} * 1024;
  std::size_t linger_max_bytes = std::size_t{1024} * 1024;

  Milliseconds request_header_timeout{10'000};  // Accept to full head; never extended.
  Milliseconds connect_timeout{2'000};
  Milliseconds response_header_timeout{30'000};  // Request sent to response head.
  Milliseconds idle_timeout{30'000};             // No bytes moved in either direction.
  Milliseconds exchange_timeout{300'000};        // Hard cap on the whole exchange.
  Milliseconds linger_timeout{1'000};
};

struct ConnectResult {
  std::unique_ptr<net::Stream> stream;
  int error = 0;
};

// Starts a connection attempt to the upstream. The stream may still be connecting.
using Connector = std::function<ConnectResult()>;

struct Interest {
  bool read = false;
  bool write = false;
};

// What the session needs before it can make further progress.
struct Wait {
  Interest client;
  Interest upstream;
  std::optional<Clock::time_point> deadline;
};

// Conditions only the event source can observe.
struct Events {
  bool client_failed = false;      // POLLERR/POLLHUP: both directions are gone.
  bool upstream_writable = false;  // Includes error, as that also ends a connect.
};

enum class Outcome {
  InProgress,
  Completed,        // A full response, from upstream or generated here, was delivered.
  ClientAborted,    // Client went away first.
  UpstreamAborted,  // Upstream failed after the response had started.
  TimedOut,         // Deadline passed after the response had started.
  Shutdown,         // Process stopping.
};

[[nodiscard]] const char* to_string(Outcome outcome) noexcept;

struct SessionStats {
  std::string method;
  std::string path;  // Excludes the query string.
  int status = 0;    // 0 if no response was started.
  std::uint64_t bytes_from_client = 0;
  std::uint64_t bytes_to_client = 0;
  std::string detail;  // Static-origin reason text for failures; never request data.
  Outcome outcome = Outcome::InProgress;
};

// One client connection and its single request/response exchange, as a non-blocking
// state machine. It owns both streams and does no waiting of its own: an event source
// calls advance(), which moves data until nothing further is possible and then says
// what to wait for. It touches no global state, so any readiness source can drive it.
//
// The two directions progress independently, because they must. An upstream may
// answer before reading the whole request body, and if each side were processed in
// turn with both buffers full, the proxy would deadlock.
class ProxySession {
 public:
  ProxySession(std::unique_ptr<net::Stream> client, std::string client_ip, Connector connector,
               SessionOptions options, Clock::time_point now);

  [[nodiscard]] Wait advance(Clock::time_point now, Events events = {});

  // Terminates at once, closing both peers.
  void abort(Outcome outcome);

  [[nodiscard]] bool finished() const noexcept { return phase_ == Phase::Done; }
  [[nodiscard]] const SessionStats& stats() const noexcept { return stats_; }
  [[nodiscard]] int client_handle() const noexcept;
  // -1 when no upstream connection exists.
  [[nodiscard]] int upstream_handle() const noexcept;

 private:
  enum class Phase {
    Relaying,    // Normal operation.
    Responding,  // Delivering an error generated here; the upstream is gone.
    Lingering,   // Response delivered; draining the client so close() does not RST.
    Done,
  };
  enum class RequestState { ReadingHead, ReadingBody, Complete };
  enum class ResponseState { ReadingHead, RelayingBody, Complete };
  enum class UpstreamState { None, Connecting, Connected };

  bool pump();
  bool read_client();
  void parse_request();
  void on_request_head();
  bool write_upstream();
  bool read_upstream();
  void parse_response();
  void on_response_head();
  void on_upstream_eof();
  bool write_client();
  bool drain_client();

  void finish_connect();
  void check_deadline();
  void on_timeout();
  void respond_with_error(int status, const char* detail);
  void fail_response(const char* detail);
  void begin_linger();
  void finish(Outcome outcome);
  void close_upstream() noexcept;
  void note_progress() noexcept { last_progress_ = now_; }

  [[nodiscard]] std::optional<Clock::time_point> next_deadline() const noexcept;
  [[nodiscard]] Wait current_wait() const noexcept;

  SessionOptions options_;
  std::string client_ip_;
  Connector connector_;
  std::unique_ptr<net::Stream> client_;
  std::unique_ptr<net::Stream> upstream_;

  http::Http1Parser request_parser_;
  http::Http1Parser response_parser_;
  OutboundChannel to_upstream_;
  OutboundChannel to_client_;

  Phase phase_ = Phase::Relaying;
  RequestState request_state_ = RequestState::ReadingHead;
  ResponseState response_state_ = ResponseState::ReadingHead;
  UpstreamState upstream_state_ = UpstreamState::None;
  bool upstream_write_failed_ = false;
  bool head_request_ = false;
  // The upstream failed mid-response: flush what is owed, then close abnormally.
  bool response_truncated_ = false;

  Clock::time_point now_;
  Clock::time_point last_progress_;
  Clock::time_point header_deadline_;
  Clock::time_point exchange_deadline_;
  Clock::time_point connect_deadline_;
  std::optional<Clock::time_point> response_deadline_;
  Clock::time_point linger_deadline_;
  std::size_t lingered_bytes_ = 0;

  SessionStats stats_;
};

}  // namespace fluxgate::proxy
