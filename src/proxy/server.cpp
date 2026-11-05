#include "fluxgate/proxy/server.hpp"

#include <poll.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <system_error>

#include "fluxgate/log/log.hpp"
#include "fluxgate/net/socket.hpp"
#include "fluxgate/net/tcp_stream.hpp"

namespace fluxgate::proxy {
namespace {

// How long to back off when accept() fails for lack of resources. The listener stays
// readable throughout, so without a pause the loop would spin at full CPU.
constexpr int kAcceptBackoffMs = 100;

SessionOptions make_session_options(const config::Config& config) {
  SessionOptions options;
  options.head_limits.max_head_bytes = config.limits.max_header_bytes;
  options.head_limits.max_headers = config.limits.max_headers;
  options.max_request_body_bytes = config.limits.max_request_body_bytes;
  options.buffer_bytes = config.limits.io_buffer_bytes;
  options.request_header_timeout = config.timeouts.request_header;
  options.connect_timeout = config.timeouts.connect;
  options.response_header_timeout = config.timeouts.response_header;
  options.idle_timeout = config.timeouts.idle;
  options.exchange_timeout = config.timeouts.exchange;
  return options;
}

short poll_events(Interest interest) noexcept {
  short events = 0;
  if (interest.read) {
    events |= POLLIN;
  }
  if (interest.write) {
    events |= POLLOUT;
  }
  return events;
}

// Rounds up: rounding down would wake early and spin for the final millisecond.
int poll_timeout_ms(const std::optional<Clock::time_point>& deadline, Clock::time_point now) {
  if (!deadline) {
    return -1;
  }
  if (*deadline <= now) {
    return 0;
  }
  const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(*deadline - now);
  return static_cast<int>(remaining.count());
}

}  // namespace

Server::Server(const config::Config& config, int stop_fd)
    : options_(make_session_options(config)),
      upstream_address_(net::resolve(config.upstream.host, config.upstream.port)),
      listener_(net::listen_tcp(net::resolve(config.listener.address, config.listener.port),
                                config.listener.backlog)),
      listen_address_(net::local_address(listener_.get())),
      stop_fd_(stop_fd) {}

void Server::run() {
  log::info("server.listening", {{"address", listen_address_.to_string()},
                                 {"port", listen_address_.port()},
                                 {"upstream", upstream_address_.to_string()}});
  std::array<pollfd, 2> fds{{{.fd = stop_fd_, .events = POLLIN, .revents = 0},
                             {.fd = listener_.get(), .events = POLLIN, .revents = 0}}};
  while (true) {
    // During backoff the listener is taken out of the set. It stays readable while
    // connections are queued, so merely adding a timeout would return immediately.
    const bool backing_off = fds[1].fd < 0;
    const int ready = ::poll(fds.data(), fds.size(), backing_off ? kAcceptBackoffMs : -1);
    if (ready < 0 && errno != EINTR) {
      throw std::system_error(errno, std::generic_category(), "poll");
    }
    if (ready > 0 && fds[0].revents != 0) {
      break;
    }
    if (backing_off) {
      fds[1].fd = ready == 0 ? listener_.get() : fds[1].fd;
      continue;
    }
    if (ready <= 0 || fds[1].revents == 0) {
      continue;
    }
    net::AcceptOutcome accepted = net::accept_tcp(listener_.get());
    if (accepted.error != 0) {
      log::error("server.accept_failed", {{"error", net::error_description(accepted.error)}});
      fds[1].fd = -1;
      continue;
    }
    if (accepted.socket && !serve(std::move(*accepted.socket))) {
      break;
    }
  }
  log::info("server.stopped");
}

bool Server::serve(net::AcceptedSocket socket) {
  const Clock::time_point started = Clock::now();
  std::string client_ip = socket.peer.ip();
  Connector connector = [this]() -> ConnectResult {
    net::ConnectOutcome outcome = net::connect_tcp(upstream_address_);
    if (!outcome.fd) {
      return {.stream = nullptr, .error = outcome.error};
    }
    return {.stream = std::make_unique<net::TcpStream>(std::move(outcome.fd)), .error = 0};
  };

  ProxySession session(std::make_unique<net::TcpStream>(std::move(socket.fd)), client_ip,
                       std::move(connector), options_, started);

  bool stop_requested = false;
  Wait wait = session.advance(Clock::now());
  while (!session.finished()) {
    const int upstream_fd = session.upstream_handle();
    const short upstream_events = poll_events(wait.upstream);
    std::array<pollfd, 3> fds{
        {{.fd = stop_fd_, .events = POLLIN, .revents = 0},
         // Watched even with no interest, as POLLERR and POLLHUP are always delivered
         // and are how a vanished client is noticed while waiting on the upstream.
         {.fd = session.client_handle(), .events = poll_events(wait.client), .revents = 0},
         // Ignored when idle: a failed upstream the session is not ready to read from
         // would otherwise report POLLERR continuously and spin this loop.
         {.fd = upstream_events != 0 ? upstream_fd : -1, .events = upstream_events, .revents = 0}}};

    const int ready = ::poll(fds.data(), fds.size(), poll_timeout_ms(wait.deadline, Clock::now()));
    if (ready < 0 && errno != EINTR) {
      throw std::system_error(errno, std::generic_category(), "poll");
    }
    if (ready > 0 && fds[0].revents != 0) {
      stop_requested = true;
      session.abort(Outcome::Shutdown);
      break;
    }

    Events events;
    if (ready > 0) {
      events.client_failed = (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
      events.upstream_writable = (fds[2].revents & (POLLOUT | POLLERR | POLLHUP)) != 0;
    }
    wait = session.advance(Clock::now(), events);
  }

  log_exchange(session.stats(), client_ip, Clock::now() - started);
  return !stop_requested;
}

void Server::log_exchange(const SessionStats& stats, const std::string& client_ip,
                          Clock::duration elapsed) const {
  const auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
  // A connection that closed without sending anything is not an exchange.
  if (stats.method.empty() && stats.status == 0 && stats.bytes_from_client == 0) {
    return;
  }
  log::info("access", {{"client", client_ip},
                       {"method", stats.method},
                       {"path", stats.path},
                       {"status", stats.status},
                       {"outcome", to_string(stats.outcome)},
                       {"detail", stats.detail},
                       {"bytes_in", stats.bytes_from_client},
                       {"bytes_out", stats.bytes_to_client},
                       {"duration_us", duration_us}});
}

}  // namespace fluxgate::proxy
