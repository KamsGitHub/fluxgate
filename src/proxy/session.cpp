#include "fluxgate/proxy/session.hpp"

#include <algorithm>
#include <array>
#include <cerrno>

#include "fluxgate/http/policy.hpp"

namespace fluxgate::proxy {
namespace {

using http::Http1Parser;
using http::ParseError;
using http::ParseEvent;
using net::IoStatus;

// Bounds work per advance() so that an always-ready peer cannot hold the caller
// indefinitely and starve deadline checks. Unfinished work is reported through the
// returned interest, so the next wait completes at once.
constexpr int kMaxPassesPerAdvance = 16;

constexpr std::string_view kContinue = "HTTP/1.1 100 Continue\r\n\r\n";

int status_for(ParseError error) noexcept {
  return error == ParseError::HeadTooLarge || error == ParseError::TooManyHeaders ? 431 : 400;
}

}  // namespace

const char* to_string(Outcome outcome) noexcept {
  switch (outcome) {
    case Outcome::InProgress:
      return "in_progress";
    case Outcome::Completed:
      return "completed";
    case Outcome::ClientAborted:
      return "client_aborted";
    case Outcome::UpstreamAborted:
      return "upstream_aborted";
    case Outcome::TimedOut:
      return "timed_out";
    case Outcome::Shutdown:
      return "shutdown";
  }
  return "unknown";
}

ProxySession::ProxySession(std::unique_ptr<net::Stream> client, std::string client_ip,
                           Connector connector, SessionOptions options, Clock::time_point now)
    : options_(options),
      client_ip_(std::move(client_ip)),
      connector_(std::move(connector)),
      client_(std::move(client)),
      request_parser_(Http1Parser::Mode::Request, options.head_limits),
      response_parser_(Http1Parser::Mode::Response, options.head_limits),
      to_upstream_(options.buffer_bytes),
      to_client_(options.buffer_bytes),
      now_(now),
      last_progress_(now),
      header_deadline_(now + options.request_header_timeout),
      exchange_deadline_(now + options.exchange_timeout) {}

int ProxySession::client_handle() const noexcept {
  return client_ ? client_->native_handle() : -1;
}

int ProxySession::upstream_handle() const noexcept {
  return upstream_ ? upstream_->native_handle() : -1;
}

Wait ProxySession::advance(Clock::time_point now, Events events) {
  now_ = now;
  if (phase_ == Phase::Done) {
    return {};
  }

  if (events.client_failed) {
    abort(Outcome::ClientAborted);
    return {};
  }
  if (events.upstream_writable && upstream_state_ == UpstreamState::Connecting) {
    finish_connect();
  }
  check_deadline();

  bool progress = true;
  for (int pass = 0; progress && pass < kMaxPassesPerAdvance && phase_ != Phase::Done; ++pass) {
    progress = pump();
  }
  return current_wait();
}

bool ProxySession::pump() {
  if (phase_ == Phase::Lingering) {
    return drain_client();
  }
  // Each step can end the relay, so the phase is rechecked between steps.
  bool progress = false;
  if (phase_ == Phase::Relaying) {
    progress |= read_client();
  }
  if (phase_ == Phase::Relaying) {
    progress |= write_upstream();
  }
  if (phase_ == Phase::Relaying) {
    progress |= read_upstream();
  }
  if (phase_ == Phase::Relaying || phase_ == Phase::Responding) {
    progress |= write_client();
  }
  const bool response_delivered =
      (phase_ == Phase::Responding ||
       (phase_ == Phase::Relaying && response_state_ == ResponseState::Complete)) &&
      !to_client_.has_pending();
  if (response_delivered) {
    begin_linger();
    return true;
  }
  return progress;
}

void ProxySession::abort(Outcome outcome) {
  if (phase_ == Phase::Done) {
    return;
  }
  // Lingering starts only once the response is fully delivered, so nothing that
  // happens afterwards can change how the exchange went.
  finish(phase_ == Phase::Lingering ? stats_.outcome : outcome);
}

bool ProxySession::read_client() {
  if (request_state_ == RequestState::Complete) {
    return false;
  }
  auto space = to_upstream_.buffer().writable();
  if (space.empty()) {
    return false;  // Backpressure: the upstream is not keeping up.
  }
  const net::IoResult io = client_->read(space);
  switch (io.status) {
    case IoStatus::Ok:
      to_upstream_.buffer().commit(io.bytes);
      stats_.bytes_from_client += io.bytes;
      note_progress();
      parse_request();
      return true;
    case IoStatus::WouldBlock:
      return false;
    case IoStatus::Eof:
      // A peer that has only half-closed cannot have a truncated request completed
      // by anyone, so the exchange is over either way.
      stats_.detail = request_state_ == RequestState::ReadingHead
                          ? "client closed before sending a full head"
                          : "client closed during request body";
      abort(Outcome::ClientAborted);
      return false;
    case IoStatus::Error:
      stats_.detail = "client read failed";
      abort(Outcome::ClientAborted);
      return false;
  }
  return false;
}

void ProxySession::parse_request() {
  while (phase_ == Phase::Relaying && request_state_ != RequestState::Complete) {
    const auto result = request_parser_.feed(to_upstream_.unvalidated());
    switch (result.event) {
      case ParseEvent::Error:
        respond_with_error(status_for(request_parser_.error()), request_parser_.error_reason());
        return;
      case ParseEvent::NeedMore:
        if (request_state_ == RequestState::ReadingHead) {
          to_upstream_.drop_head_bytes(result.consumed);
        } else {
          to_upstream_.validate(result.consumed);
        }
        return;
      case ParseEvent::HeadComplete:
        to_upstream_.drop_head_bytes(result.consumed);
        on_request_head();
        break;
      case ParseEvent::MessageComplete:
        to_upstream_.validate(result.consumed);
        // Pipelined requests are not supported yet. They are dropped, never relayed,
        // and the client has been told this connection closes after one response.
        to_upstream_.drop_unvalidated();
        request_state_ = RequestState::Complete;
        break;
    }
  }
}

void ProxySession::on_request_head() {
  const http::MessageHead& request = request_parser_.head();
  stats_.method = request.method;
  stats_.path = http::target_path(request.target);
  head_request_ = request.method == "HEAD";

  if (const int status = http::validate_request(request, options_.max_request_body_bytes);
      status != 0) {
    respond_with_error(status, "request refused by policy");
    return;
  }

  request_state_ = RequestState::ReadingBody;
  if (head_request_) {
    response_parser_.expect_no_body();
  }

  // Clients waiting for this would otherwise stall for their own timeout. Absorbing
  // Expect here, as Envoy does, also spares the upstream a protocol step that a
  // connection-reusing proxy would have to track per exchange.
  if (http::expects_continue(request)) {
    to_client_.queue_head(kContinue);
  }
  to_upstream_.queue_head(http::render_upstream_request(request, client_ip_));

  ConnectResult connection = connector_();
  if (!connection.stream) {
    respond_with_error(502, net::error_description(connection.error));
    return;
  }
  upstream_ = std::move(connection.stream);
  upstream_state_ = UpstreamState::Connecting;
  connect_deadline_ = now_ + options_.connect_timeout;
}

void ProxySession::finish_connect() {
  const int error = upstream_->take_error();
  if (error != 0) {
    respond_with_error(502, net::error_description(error));
    return;
  }
  upstream_state_ = UpstreamState::Connected;
  note_progress();
}

bool ProxySession::write_upstream() {
  if (upstream_state_ != UpstreamState::Connected) {
    return false;
  }
  if (upstream_write_failed_) {
    to_upstream_.drop_validated();
    return false;
  }
  const auto result = to_upstream_.flush(*upstream_);
  if (result.failed) {
    // Not the end of the exchange: the upstream may have answered and closed without
    // reading the whole body, and that answer is still waiting to be read. Keep
    // draining the client and let read_upstream() decide.
    upstream_write_failed_ = true;
    to_upstream_.drop_validated();
    return true;
  }
  if (result.bytes > 0) {
    note_progress();
  }
  if (request_state_ == RequestState::Complete && !to_upstream_.has_pending() &&
      !response_deadline_) {
    response_deadline_ = now_ + options_.response_header_timeout;
  }
  return result.bytes > 0;
}

bool ProxySession::read_upstream() {
  if (upstream_state_ != UpstreamState::Connected || response_state_ == ResponseState::Complete) {
    return false;
  }
  auto space = to_client_.buffer().writable();
  if (space.empty()) {
    return false;  // Backpressure: the client is not keeping up.
  }
  const net::IoResult io = upstream_->read(space);
  switch (io.status) {
    case IoStatus::Ok:
      to_client_.buffer().commit(io.bytes);
      note_progress();
      parse_response();
      return true;
    case IoStatus::WouldBlock:
      return false;
    case IoStatus::Eof:
      on_upstream_eof();
      return true;
    case IoStatus::Error:
      fail_response("upstream read failed");
      return true;
  }
  return false;
}

void ProxySession::parse_response() {
  while (phase_ == Phase::Relaying && !response_truncated_ &&
         response_state_ != ResponseState::Complete) {
    const auto result = response_parser_.feed(to_client_.unvalidated());
    switch (result.event) {
      case ParseEvent::Error:
        fail_response(response_parser_.error_reason());
        return;
      case ParseEvent::NeedMore:
        if (response_state_ == ResponseState::ReadingHead) {
          to_client_.drop_head_bytes(result.consumed);
        } else {
          to_client_.validate(result.consumed);
        }
        return;
      case ParseEvent::HeadComplete:
        to_client_.drop_head_bytes(result.consumed);
        on_response_head();
        break;
      case ParseEvent::MessageComplete:
        to_client_.validate(result.consumed);
        to_client_.drop_unvalidated();
        response_state_ = ResponseState::Complete;
        // Done with the upstream: release it now rather than on client teardown.
        close_upstream();
        return;
    }
  }
}

void ProxySession::on_response_head() {
  const http::MessageHead& response = response_parser_.head();

  // Upgrade is stripped from requests, so an upstream that switches protocols anyway
  // is misbehaving, and what it sends next would not be HTTP.
  if (response.status == 101) {
    fail_response("upstream attempted a protocol upgrade");
    return;
  }
  if (response.status >= 100 && response.status < 200) {
    to_client_.queue_head(http::render_interim_response(response));
    const bool head_request = head_request_;
    response_parser_.reset();
    if (head_request) {
      response_parser_.expect_no_body();
    }
    return;
  }

  stats_.status = response.status;
  response_state_ = ResponseState::RelayingBody;
  to_client_.queue_head(http::render_client_response(response));
}

void ProxySession::on_upstream_eof() {
  if (response_state_ == ResponseState::RelayingBody && response_parser_.finish()) {
    response_state_ = ResponseState::Complete;
    close_upstream();
    return;
  }
  fail_response("upstream closed before end of response");
}

bool ProxySession::write_client() {
  const auto result = to_client_.flush(*client_);
  if (result.failed) {
    stats_.detail = "client write failed";
    abort(Outcome::ClientAborted);
    return false;
  }
  if (result.bytes == 0) {
    return false;
  }
  stats_.bytes_to_client += result.bytes;
  note_progress();
  return true;
}

bool ProxySession::drain_client() {
  std::array<std::byte, 4096> sink{};
  const net::IoResult io = client_->read(sink);
  switch (io.status) {
    case IoStatus::Ok:
      lingered_bytes_ += io.bytes;
      if (lingered_bytes_ > options_.linger_max_bytes) {
        finish(stats_.outcome);
        return false;
      }
      return true;
    case IoStatus::WouldBlock:
      return false;
    case IoStatus::Eof:
    case IoStatus::Error:
      finish(stats_.outcome);
      return false;
  }
  return false;
}

void ProxySession::respond_with_error(int status, const char* detail) {
  stats_.status = status;
  stats_.detail = detail;
  close_upstream();
  to_upstream_.drop_validated();
  to_client_.queue_head(http::render_error_response(status, head_request_));
  phase_ = Phase::Responding;
}

void ProxySession::fail_response(const char* detail) {
  if (response_state_ == ResponseState::ReadingHead) {
    respond_with_error(502, detail);
    return;
  }
  // The status line has gone out, so the only remaining honest signal is an abnormal
  // close, delivered after whatever validated bytes the client is already owed.
  stats_.detail = detail;
  response_truncated_ = true;
  response_state_ = ResponseState::Complete;
  close_upstream();
}

void ProxySession::begin_linger() {
  if (response_truncated_) {
    client_->abort_on_close();
    finish(Outcome::UpstreamAborted);
    return;
  }
  stats_.outcome = Outcome::Completed;
  close_upstream();
  // Closing with unread data pending sends RST, which can destroy the response we
  // just wrote before the client reads it. Half-closing and draining avoids that
  // when, for example, a 413 is sent while the client is still uploading.
  client_->shutdown_write();
  linger_deadline_ = now_ + options_.linger_timeout;
  phase_ = Phase::Lingering;
}

void ProxySession::finish(Outcome outcome) {
  stats_.outcome = outcome;
  close_upstream();
  client_.reset();
  phase_ = Phase::Done;
}

void ProxySession::close_upstream() noexcept {
  upstream_.reset();
  upstream_state_ = UpstreamState::None;
}

void ProxySession::check_deadline() {
  if (const auto deadline = next_deadline(); deadline && now_ >= *deadline) {
    on_timeout();
  }
}

void ProxySession::on_timeout() {
  if (phase_ == Phase::Lingering) {
    finish(stats_.outcome);
    return;
  }
  if (phase_ == Phase::Responding) {
    stats_.detail = "client did not read the error response";
    finish(Outcome::TimedOut);
    return;
  }
  if (response_state_ != ResponseState::ReadingHead) {
    stats_.detail = "deadline passed while relaying response";
    client_->abort_on_close();
    finish(Outcome::TimedOut);
    return;
  }

  // Blame whichever side the exchange is actually waiting for.
  const bool waiting_for_client = request_state_ != RequestState::Complete &&
                                  !to_upstream_.has_pending() &&
                                  upstream_state_ != UpstreamState::Connecting;
  respond_with_error(waiting_for_client ? 408 : 504, waiting_for_client
                                                         ? "timed out waiting for client"
                                                         : "timed out waiting for upstream");
}

std::optional<Clock::time_point> ProxySession::next_deadline() const noexcept {
  switch (phase_) {
    case Phase::Done:
      return std::nullopt;
    case Phase::Lingering:
      return linger_deadline_;
    case Phase::Responding:
      return std::min(exchange_deadline_, last_progress_ + options_.idle_timeout);
    case Phase::Relaying:
      break;
  }

  Clock::time_point deadline = exchange_deadline_;
  if (request_state_ == RequestState::ReadingHead) {
    return std::min(deadline, header_deadline_);
  }
  if (upstream_state_ == UpstreamState::Connecting) {
    deadline = std::min(deadline, connect_deadline_);
  }
  if (response_deadline_ && response_state_ == ResponseState::ReadingHead) {
    // Idle time does not apply: silence is expected while the upstream works.
    return std::min(deadline, *response_deadline_);
  }
  return std::min(deadline, last_progress_ + options_.idle_timeout);
}

Wait ProxySession::current_wait() const noexcept {
  Wait wait;
  wait.deadline = next_deadline();
  switch (phase_) {
    case Phase::Done:
      break;
    case Phase::Lingering:
      wait.client.read = true;
      break;
    case Phase::Responding:
      wait.client.write = true;
      break;
    case Phase::Relaying:
      wait.client.read = request_state_ != RequestState::Complete && !to_upstream_.buffer_full();
      wait.client.write = to_client_.has_pending();
      wait.upstream.write = upstream_state_ == UpstreamState::Connecting ||
                            (upstream_state_ == UpstreamState::Connected &&
                             to_upstream_.has_pending() && !upstream_write_failed_);
      wait.upstream.read = upstream_state_ == UpstreamState::Connected &&
                           response_state_ != ResponseState::Complete && !to_client_.buffer_full();
      break;
  }
  return wait;
}

}  // namespace fluxgate::proxy
