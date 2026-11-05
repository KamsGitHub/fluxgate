#include "fluxgate/proxy/session.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>

using namespace fluxgate::proxy;
using fluxgate::net::IoResult;
using fluxgate::net::IoStatus;
using fluxgate::net::Stream;
using namespace std::chrono_literals;

namespace {

// A stream whose every read and write outcome is scripted, which makes short writes,
// EAGAIN and failures deterministic in a way real sockets are not.
struct FakeState {
  std::deque<std::string> inbound;  // Each entry is delivered by one read().
  bool eof = false;
  int read_error = 0;
  std::string written;
  std::size_t max_write = SIZE_MAX;
  std::size_t writable_bytes = SIZE_MAX;  // Then EAGAIN until raised again.
  int write_error = 0;
  int connect_error = 0;
  bool shut_down = false;
  bool aborted = false;
  bool closed = false;
  std::size_t max_read_size = 0;
};

class FakeStream final : public Stream {
 public:
  explicit FakeStream(std::shared_ptr<FakeState> state) : state_(std::move(state)) {}
  ~FakeStream() override { state_->closed = true; }

  IoResult read(std::span<std::byte> destination) override {
    state_->max_read_size = std::max(state_->max_read_size, destination.size());
    if (!state_->inbound.empty()) {
      std::string& front = state_->inbound.front();
      const std::size_t n = std::min(front.size(), destination.size());
      std::memcpy(destination.data(), front.data(), n);
      front.erase(0, n);
      if (front.empty()) {
        state_->inbound.pop_front();
      }
      return {.status = IoStatus::Ok, .bytes = n, .error = 0};
    }
    if (state_->read_error != 0) {
      return {.status = IoStatus::Error, .bytes = 0, .error = state_->read_error};
    }
    if (state_->eof) {
      return {.status = IoStatus::Eof, .bytes = 0, .error = 0};
    }
    return {.status = IoStatus::WouldBlock, .bytes = 0, .error = 0};
  }

  IoResult write(std::span<const std::byte> source) override {
    if (state_->write_error != 0) {
      return {.status = IoStatus::Error, .bytes = 0, .error = state_->write_error};
    }
    const std::size_t n = std::min({source.size(), state_->max_write, state_->writable_bytes});
    if (n == 0) {
      return {.status = IoStatus::WouldBlock, .bytes = 0, .error = 0};
    }
    state_->written.append(reinterpret_cast<const char*>(source.data()), n);
    if (state_->writable_bytes != SIZE_MAX) {
      state_->writable_bytes -= n;
    }
    return {.status = IoStatus::Ok, .bytes = n, .error = 0};
  }

  void shutdown_write() noexcept override { state_->shut_down = true; }
  void abort_on_close() noexcept override { state_->aborted = true; }
  int take_error() noexcept override { return state_->connect_error; }
  int native_handle() const noexcept override { return 42; }

 private:
  std::shared_ptr<FakeState> state_;
};

constexpr std::string_view kGet = "GET /a?secret=1 HTTP/1.1\r\nHost: h\r\n\r\n";
constexpr std::string_view kOk = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi";

class SessionTest : public testing::Test {
 protected:
  void SetUp() override {
    options_.buffer_bytes = 1024;
    start();
  }

  void start() {
    session_ = std::make_unique<ProxySession>(
        std::make_unique<FakeStream>(client_), "10.0.0.1",
        [this]() -> ConnectResult {
          ++connects_;
          if (connect_immediate_error_ != 0) {
            return {.stream = nullptr, .error = connect_immediate_error_};
          }
          return {.stream = std::make_unique<FakeStream>(upstream_), .error = 0};
        },
        options_, now_);
  }

  Wait step(Events events = {}) { return session_->advance(now_, events); }

  // Runs with the upstream connected, as if every wait were satisfied immediately.
  Wait pump(int rounds = 50) {
    Wait wait;
    for (int i = 0; i < rounds && !session_->finished(); ++i) {
      wait = step({.client_failed = false, .upstream_writable = true});
    }
    return wait;
  }

  void finish_cleanly() {
    client_->eof = true;
    pump();
    EXPECT_TRUE(session_->finished());
  }

  [[nodiscard]] bool client_got(std::string_view text) const {
    return client_->written.find(text) != std::string::npos;
  }

  std::shared_ptr<FakeState> client_ = std::make_shared<FakeState>();
  std::shared_ptr<FakeState> upstream_ = std::make_shared<FakeState>();
  SessionOptions options_;
  Clock::time_point now_{};
  int connects_ = 0;
  int connect_immediate_error_ = 0;
  std::unique_ptr<ProxySession> session_;
};

TEST_F(SessionTest, ForwardsRequestAndResponse) {
  client_->inbound.emplace_back(kGet);
  upstream_->inbound.emplace_back(kOk);
  upstream_->eof = true;
  pump();

  EXPECT_TRUE(upstream_->written.starts_with("GET /a?secret=1 HTTP/1.1\r\n"));
  EXPECT_NE(upstream_->written.find("X-Forwarded-For: 10.0.0.1\r\n"), std::string::npos);
  EXPECT_TRUE(client_got("HTTP/1.1 200 OK\r\n"));
  EXPECT_TRUE(client_->written.ends_with("\r\n\r\nhi"));
  EXPECT_TRUE(upstream_->closed);

  finish_cleanly();
  EXPECT_EQ(session_->stats().status, 200);
  EXPECT_EQ(session_->stats().outcome, Outcome::Completed);
  EXPECT_EQ(session_->stats().path, "/a");  // No query string.
}

TEST_F(SessionTest, HandlesRequestDeliveredOneByteAtATime) {
  for (const char c : kGet) {
    client_->inbound.emplace_back(1, c);
  }
  upstream_->inbound.emplace_back(kOk);
  pump();
  EXPECT_TRUE(upstream_->written.starts_with("GET /a?secret=1 HTTP/1.1\r\nHost: h\r\n"));
  EXPECT_TRUE(client_got("\r\n\r\nhi"));
  EXPECT_EQ(connects_, 1);
}

TEST_F(SessionTest, HandlesResponseDeliveredOneByteAtATime) {
  client_->inbound.emplace_back(kGet);
  for (const char c : kOk) {
    upstream_->inbound.emplace_back(1, c);
  }
  pump();
  EXPECT_TRUE(client_->written.ends_with("\r\n\r\nhi"));
}

TEST_F(SessionTest, BodyIsRelayedExactlyUnderOneByteWrites) {
  const std::string body(5000, 'x');
  const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 5000\r\n\r\n" + body;
  client_->inbound.emplace_back(kGet);
  upstream_->inbound.emplace_back(response);
  upstream_->max_write = 1;
  client_->max_write = 1;
  pump(20'000);
  EXPECT_TRUE(upstream_->written.starts_with("GET /a"));
  EXPECT_TRUE(client_->written.ends_with("\r\n\r\n" + body));
}

TEST_F(SessionTest, ResumesExactlyAfterEagainMidWrite) {
  const std::string body(3000, 'y');
  client_->inbound.emplace_back(kGet);
  upstream_->inbound.emplace_back("HTTP/1.1 200 OK\r\nContent-Length: 3000\r\n\r\n" + body);
  client_->writable_bytes = 7;  // Stalls in the middle of the response head.
  pump();
  EXPECT_EQ(client_->written.size(), 7U);
  EXPECT_TRUE(step().client.write);

  client_->writable_bytes = 500;
  pump();
  EXPECT_EQ(client_->written.size(), 507U);

  client_->writable_bytes = SIZE_MAX;
  pump();
  EXPECT_TRUE(client_->written.ends_with(body));
}

TEST_F(SessionTest, SlowClientStopsUpstreamReads) {
  const std::string body(100'000, 'z');
  client_->inbound.emplace_back(kGet);
  upstream_->inbound.emplace_back("HTTP/1.1 200 OK\r\nContent-Length: 100000\r\n\r\n" + body);
  client_->writable_bytes = 0;

  const Wait wait = pump();
  EXPECT_FALSE(wait.upstream.read);  // Buffer full: reading must have stopped.
  EXPECT_TRUE(wait.client.write);
  EXPECT_FALSE(upstream_->inbound.empty());  // Most of the body is still unread.

  client_->writable_bytes = SIZE_MAX;
  pump(2000);
  EXPECT_TRUE(client_->written.ends_with(body));
}

TEST_F(SessionTest, SlowUpstreamStopsClientReads) {
  const std::string body(50'000, 'b');
  client_->inbound.emplace_back("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 50000\r\n\r\n" +
                                body);
  upstream_->writable_bytes = 0;

  const Wait wait = pump();
  EXPECT_FALSE(wait.client.read);
  EXPECT_TRUE(wait.upstream.write);

  upstream_->writable_bytes = SIZE_MAX;
  pump(2000);
  EXPECT_TRUE(upstream_->written.ends_with(body));
}

TEST_F(SessionTest, EarlyResponseWhileBodyIsStuckDoesNotDeadlock) {
  // The upstream answers without reading while the client's upload is still flowing:
  // the classic deadlock for a proxy that finishes the request before reading.
  client_->inbound.emplace_back("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 90000\r\n\r\n" +
                                std::string(90'000, 'b'));
  upstream_->writable_bytes = 0;
  upstream_->inbound.emplace_back("HTTP/1.1 413 Too Large\r\nContent-Length: 0\r\n\r\n");
  pump();
  EXPECT_TRUE(client_got("HTTP/1.1 413 Too Large\r\n"));
  EXPECT_TRUE(upstream_->closed);
  EXPECT_TRUE(client_->shut_down);
}

TEST_F(SessionTest, UpstreamResponseSurvivesFailedRequestWrite) {
  client_->inbound.emplace_back("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\nhello");
  upstream_->write_error = EPIPE;
  upstream_->inbound.emplace_back(kOk);
  pump();
  EXPECT_TRUE(client_got("HTTP/1.1 200 OK\r\n"));
}

TEST_F(SessionTest, FailedRequestWriteWithoutResponseIs502) {
  client_->inbound.emplace_back(kGet);
  upstream_->write_error = EPIPE;
  upstream_->eof = true;
  pump();
  EXPECT_TRUE(client_got("HTTP/1.1 502 Bad Gateway\r\n"));
}

TEST_F(SessionTest, BytesPastTheRequestNeverReachTheUpstream) {
  client_->inbound.emplace_back(
      "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 2\r\n\r\nhi"
      "GET /smuggled HTTP/1.1\r\nHost: h\r\n\r\n");
  pump();
  EXPECT_TRUE(upstream_->written.ends_with("\r\n\r\nhi"));
  EXPECT_EQ(upstream_->written.find("smuggled"), std::string::npos);
}

TEST_F(SessionTest, BytesPastTheResponseNeverReachTheClient) {
  client_->inbound.emplace_back(kGet);
  upstream_->inbound.emplace_back(std::string(kOk) + "HTTP/1.1 200 OK\r\n\r\ninjected");
  pump();
  EXPECT_TRUE(client_->written.ends_with("\r\n\r\nhi"));
  EXPECT_EQ(client_->written.find("injected"), std::string::npos);
}

TEST_F(SessionTest, MalformedRequestGets400AndNoUpstream) {
  client_->inbound.emplace_back("GARBAGE\r\n\r\n");
  pump();
  EXPECT_TRUE(client_got("HTTP/1.1 400 Bad Request\r\n"));
  EXPECT_EQ(connects_, 0);
}

TEST_F(SessionTest, OversizedHeadGets431) {
  options_.head_limits.max_head_bytes = 64;
  start();
  client_->inbound.emplace_back("GET / HTTP/1.1\r\nX: " + std::string(200, 'a'));
  pump();
  EXPECT_TRUE(client_got("HTTP/1.1 431 "));
  EXPECT_EQ(connects_, 0);
}

TEST_F(SessionTest, OversizedDeclaredBodyGets413BeforeConnecting) {
  options_.max_request_body_bytes = 4;
  start();
  client_->inbound.emplace_back("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\n");
  pump();
  EXPECT_TRUE(client_got("HTTP/1.1 413 "));
  EXPECT_EQ(connects_, 0);
}

TEST_F(SessionTest, ContinueIsAnsweredLocallyAndExpectNotForwarded) {
  client_->inbound.emplace_back(
      "POST / HTTP/1.1\r\nHost: h\r\nExpect: 100-continue\r\nContent-Length: 2\r\n\r\n");
  pump();
  EXPECT_EQ(client_->written, "HTTP/1.1 100 Continue\r\n\r\n");
  EXPECT_EQ(upstream_->written.find("Expect"), std::string::npos);

  client_->inbound.emplace_back("ok");
  upstream_->inbound.emplace_back(kOk);
  pump();
  EXPECT_TRUE(upstream_->written.ends_with("\r\n\r\nok"));
  EXPECT_TRUE(client_got("HTTP/1.1 200 OK\r\n"));
}

TEST_F(SessionTest, InterimResponseIsRelayedBeforeFinal) {
  client_->inbound.emplace_back(kGet);
  upstream_->inbound.emplace_back("HTTP/1.1 103 Early Hints\r\nLink: </a>\r\n\r\n" +
                                  std::string(kOk));
  pump();
  const auto interim = client_->written.find("HTTP/1.1 103 ");
  const auto final_head = client_->written.find("HTTP/1.1 200 ");
  ASSERT_NE(interim, std::string::npos);
  ASSERT_NE(final_head, std::string::npos);
  EXPECT_LT(interim, final_head);
  EXPECT_EQ(session_->stats().status, 200);
}

TEST_F(SessionTest, UpstreamProtocolSwitchIs502) {
  client_->inbound.emplace_back(kGet);
  upstream_->inbound.emplace_back("HTTP/1.1 101 Switching Protocols\r\nUpgrade: x\r\n\r\n");
  pump();
  EXPECT_TRUE(client_got("HTTP/1.1 502 "));
  EXPECT_FALSE(client_got("101"));
}

TEST_F(SessionTest, HeadRequestDoesNotWaitForDeclaredBody) {
  client_->inbound.emplace_back("HEAD / HTTP/1.1\r\nHost: h\r\n\r\n");
  upstream_->inbound.emplace_back("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n");
  pump();
  EXPECT_TRUE(client_->written.ends_with("\r\n\r\n"));
  EXPECT_TRUE(client_->shut_down);
}

TEST_F(SessionTest, HeadRequestErrorHasNoBody) {
  connect_immediate_error_ = ECONNREFUSED;
  client_->inbound.emplace_back("HEAD / HTTP/1.1\r\nHost: h\r\n\r\n");
  pump();
  EXPECT_TRUE(client_got("HTTP/1.1 502 "));
  EXPECT_TRUE(client_->written.ends_with("\r\n\r\n"));
}

TEST_F(SessionTest, ImmediateConnectFailureIs502) {
  connect_immediate_error_ = EMFILE;
  client_->inbound.emplace_back(kGet);
  pump();
  EXPECT_TRUE(client_got("HTTP/1.1 502 "));
}

TEST_F(SessionTest, AsyncConnectFailureIs502) {
  upstream_->connect_error = ECONNREFUSED;
  client_->inbound.emplace_back(kGet);
  pump();
  EXPECT_TRUE(client_got("HTTP/1.1 502 "));
  EXPECT_NE(session_->stats().detail.find("refused"), std::string::npos);
}

TEST_F(SessionTest, NothingIsWrittenUpstreamUntilConnected) {
  client_->inbound.emplace_back(kGet);
  const Wait wait = step();
  EXPECT_TRUE(upstream_->written.empty());
  EXPECT_TRUE(wait.upstream.write);  // Writability is what signals completion.
  step({.client_failed = false, .upstream_writable = true});
  EXPECT_FALSE(upstream_->written.empty());
}

TEST_F(SessionTest, UpstreamEofBeforeResponseIs502) {
  client_->inbound.emplace_back(kGet);
  upstream_->eof = true;
  pump();
  EXPECT_TRUE(client_got("HTTP/1.1 502 "));
}

TEST_F(SessionTest, MalformedResponseIs502) {
  client_->inbound.emplace_back(kGet);
  upstream_->inbound.emplace_back("not http at all\r\n\r\n");
  pump();
  EXPECT_TRUE(client_got("HTTP/1.1 502 "));
}

TEST_F(SessionTest, TruncatedBodyResetsClientAfterDeliveringWhatArrived) {
  client_->inbound.emplace_back(kGet);
  upstream_->inbound.emplace_back("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc");
  upstream_->eof = true;
  pump();
  EXPECT_TRUE(client_->written.ends_with("abc"));
  EXPECT_TRUE(client_->aborted);
  EXPECT_FALSE(client_->shut_down);
  EXPECT_TRUE(session_->finished());
  EXPECT_EQ(session_->stats().outcome, Outcome::UpstreamAborted);
}

TEST_F(SessionTest, CloseDelimitedResponseEndsCleanlyAtEof) {
  client_->inbound.emplace_back(kGet);
  upstream_->inbound.emplace_back("HTTP/1.0 200 OK\r\n\r\nbody");
  upstream_->eof = true;
  pump();
  EXPECT_TRUE(client_->written.ends_with("body"));
  EXPECT_FALSE(client_->aborted);
  EXPECT_TRUE(client_->shut_down);
}

TEST_F(SessionTest, ClientDisconnectDuringBodyClosesUpstream) {
  client_->inbound.emplace_back("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 9\r\n\r\npartial");
  client_->eof = true;
  pump();
  EXPECT_TRUE(session_->finished());
  EXPECT_EQ(session_->stats().outcome, Outcome::ClientAborted);
  EXPECT_TRUE(upstream_->closed);
}

TEST_F(SessionTest, ClientWriteFailureEndsSession) {
  client_->inbound.emplace_back(kGet);
  client_->write_error = ECONNRESET;
  upstream_->inbound.emplace_back(kOk);
  pump();
  EXPECT_TRUE(session_->finished());
  EXPECT_EQ(session_->stats().outcome, Outcome::ClientAborted);
}

TEST_F(SessionTest, ClientFailureEventEndsSession) {
  client_->inbound.emplace_back(kGet);
  pump();
  step({.client_failed = true, .upstream_writable = false});
  EXPECT_TRUE(session_->finished());
  EXPECT_TRUE(upstream_->closed);
}

TEST_F(SessionTest, HalfClosedClientStillGetsItsResponse) {
  client_->inbound.emplace_back(kGet);
  client_->eof = true;  // The FIN arrives directly behind the complete request.
  pump();
  upstream_->inbound.emplace_back(kOk);
  pump();
  EXPECT_TRUE(client_got("HTTP/1.1 200 OK\r\n"));
}

// --- Deadlines ---

TEST_F(SessionTest, SlowHeadTimesOutDespiteSteadyProgress) {
  // Slowloris: each byte is progress, but the head deadline must not be extended.
  const std::string partial = "GET / HTTP/1.1\r\nX-Pad: aaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  for (const char c : partial) {
    now_ += 500ms;
    client_->inbound.emplace_back(1, c);
    step();
    if (client_got("HTTP/1.1 408 ")) {
      break;
    }
  }
  EXPECT_TRUE(client_got("HTTP/1.1 408 "));
}

TEST_F(SessionTest, WaitReportsTheHeadDeadline) {
  EXPECT_EQ(step().deadline, now_ + options_.request_header_timeout);
}

TEST_F(SessionTest, ConnectTimeoutIs504) {
  client_->inbound.emplace_back(kGet);
  step();
  now_ += options_.connect_timeout;
  step();
  EXPECT_TRUE(client_got("HTTP/1.1 504 "));
}

TEST_F(SessionTest, SilentUpstreamTimesOutWith504) {
  client_->inbound.emplace_back(kGet);
  pump();
  now_ += options_.response_header_timeout;
  step();
  EXPECT_TRUE(client_got("HTTP/1.1 504 "));
}

TEST_F(SessionTest, StalledClientBodyTimesOutWith408) {
  client_->inbound.emplace_back("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 9\r\n\r\nab");
  pump();
  now_ += options_.idle_timeout;
  step();
  EXPECT_TRUE(client_got("HTTP/1.1 408 "));
  EXPECT_TRUE(upstream_->closed);
}

TEST_F(SessionTest, StalledUpstreamReadOfBodyTimesOutWith504) {
  client_->inbound.emplace_back("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 9\r\n\r\nab");
  upstream_->writable_bytes = 0;
  pump();
  now_ += options_.idle_timeout;
  step();
  EXPECT_TRUE(client_got("HTTP/1.1 504 "));
}

TEST_F(SessionTest, TimeoutAfterResponseStartedResetsClient) {
  client_->inbound.emplace_back(kGet);
  upstream_->inbound.emplace_back("HTTP/1.1 200 OK\r\nContent-Length: 9\r\n\r\nab");
  pump();
  now_ += options_.idle_timeout;
  step();
  EXPECT_TRUE(session_->finished());
  EXPECT_TRUE(client_->aborted);
  EXPECT_EQ(session_->stats().outcome, Outcome::TimedOut);
  EXPECT_EQ(session_->stats().status, 200);
}

TEST_F(SessionTest, ProgressExtendsIdleDeadline) {
  client_->inbound.emplace_back(kGet);
  upstream_->inbound.emplace_back("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\na");
  pump();
  now_ += options_.idle_timeout - 1ms;
  upstream_->inbound.emplace_back("b");
  pump();
  EXPECT_TRUE(client_->written.ends_with("ab"));
  EXPECT_FALSE(client_->aborted);
}

TEST_F(SessionTest, ExchangeTimeoutCapsATricklingResponse) {
  options_.idle_timeout = 10s;
  options_.exchange_timeout = 25s;
  start();
  client_->inbound.emplace_back(kGet);
  upstream_->inbound.emplace_back("HTTP/1.1 200 OK\r\nContent-Length: 99\r\n\r\n");
  pump();
  for (int i = 0; i < 3 && !session_->finished(); ++i) {
    now_ += 9s;
    upstream_->inbound.emplace_back("x");
    pump();
  }
  EXPECT_TRUE(session_->finished());
  EXPECT_EQ(session_->stats().outcome, Outcome::TimedOut);
}

TEST_F(SessionTest, ClientNotReadingErrorResponseTimesOut) {
  client_->inbound.emplace_back("GARBAGE\r\n\r\n");
  client_->writable_bytes = 0;
  pump();
  now_ += options_.idle_timeout;
  step();
  EXPECT_TRUE(session_->finished());
  EXPECT_EQ(session_->stats().outcome, Outcome::TimedOut);
}

// --- Closing ---

TEST_F(SessionTest, LingersUntilClientCloses) {
  client_->inbound.emplace_back("GARBAGE\r\n\r\n");
  const Wait wait = pump();
  EXPECT_TRUE(client_->shut_down);
  EXPECT_FALSE(session_->finished());
  EXPECT_TRUE(wait.client.read);
  client_->inbound.emplace_back("late bytes the client was still sending");
  pump();
  EXPECT_FALSE(session_->finished());
  client_->eof = true;
  pump();
  EXPECT_TRUE(session_->finished());
  EXPECT_FALSE(client_->aborted);
}

TEST_F(SessionTest, LingerIsBoundedInTime) {
  client_->inbound.emplace_back("GARBAGE\r\n\r\n");
  pump();
  now_ += options_.linger_timeout;
  step();
  EXPECT_TRUE(session_->finished());
}

TEST_F(SessionTest, LingerIsBoundedInBytes) {
  options_.linger_max_bytes = 1000;
  start();
  client_->inbound.emplace_back("GARBAGE\r\n\r\n");
  pump();
  client_->inbound.emplace_back(std::string(2000, 'x'));
  pump();
  EXPECT_TRUE(session_->finished());
}

TEST_F(SessionTest, ReadsNeverExceedTheConfiguredBuffer) {
  const std::string body(200'000, 'm');
  client_->inbound.emplace_back("POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 200000\r\n\r\n" +
                                body);
  pump(5000);
  // Checked before the response, since lingering reads into a fixed discard buffer.
  EXPECT_TRUE(upstream_->written.ends_with(body));
  EXPECT_LE(client_->max_read_size, options_.buffer_bytes);

  upstream_->inbound.emplace_back("HTTP/1.1 200 OK\r\nContent-Length: 200000\r\n\r\n" + body);
  pump(5000);
  EXPECT_TRUE(client_->written.ends_with(body));
  EXPECT_LE(upstream_->max_read_size, options_.buffer_bytes);
}

TEST_F(SessionTest, AbortWhileLingeringKeepsTheCompletedOutcome) {
  client_->inbound.emplace_back(kGet);
  upstream_->inbound.emplace_back(kOk);
  pump();
  ASSERT_FALSE(session_->finished());
  session_->abort(Outcome::Shutdown);
  EXPECT_EQ(session_->stats().outcome, Outcome::Completed);
}

TEST_F(SessionTest, AbortClosesBothPeers) {
  client_->inbound.emplace_back(kGet);
  pump();
  session_->abort(Outcome::Shutdown);
  EXPECT_TRUE(session_->finished());
  EXPECT_TRUE(client_->closed);
  EXPECT_TRUE(upstream_->closed);
  EXPECT_EQ(session_->stats().outcome, Outcome::Shutdown);
}

}  // namespace
