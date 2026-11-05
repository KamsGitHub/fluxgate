#include "fluxgate/net/tcp_stream.hpp"

#include <gtest/gtest.h>
#include <sys/socket.h>

#include <array>
#include <string_view>
#include <vector>

#include "fluxgate/net/socket.hpp"

using namespace fluxgate::net;

namespace {

struct SocketPair {
  FileDescriptor a;
  FileDescriptor b;
};

SocketPair make_pair() {
  std::array<int, 2> fds{};
  EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds.data()), 0);
  return {FileDescriptor(fds[0]), FileDescriptor(fds[1])};
}

std::span<const std::byte> bytes(std::string_view text) {
  return std::as_bytes(std::span(text));
}

}  // namespace

TEST(TcpStream, ReadOnEmptySocketWouldBlock) {
  auto [a, b] = make_pair();
  TcpStream stream(std::move(a));
  std::array<std::byte, 8> buffer{};
  EXPECT_EQ(stream.read(buffer).status, IoStatus::WouldBlock);
}

TEST(TcpStream, ReadReturnsDataThenEof) {
  auto [a, b] = make_pair();
  TcpStream reader(std::move(a));
  {
    TcpStream writer(std::move(b));
    EXPECT_EQ(writer.write(bytes("hello")).bytes, 5U);
  }
  std::array<std::byte, 8> buffer{};
  const IoResult first = reader.read(buffer);
  EXPECT_EQ(first.status, IoStatus::Ok);
  EXPECT_EQ(first.bytes, 5U);
  EXPECT_EQ(reader.read(buffer).status, IoStatus::Eof);
}

TEST(TcpStream, WriteToFullSocketEventuallyWouldBlock) {
  auto [a, b] = make_pair();
  TcpStream writer(std::move(a));
  const std::vector<std::byte> chunk(64 * 1024);
  IoResult result;
  for (int i = 0; i < 10'000; ++i) {
    result = writer.write(chunk);
    if (result.status != IoStatus::Ok) {
      break;
    }
  }
  EXPECT_EQ(result.status, IoStatus::WouldBlock);
}

TEST(TcpStream, WriteAfterPeerClosedIsAnErrorNotASignal) {
  auto [a, b] = make_pair();
  TcpStream writer(std::move(a));
  b.reset();
  const IoResult result = writer.write(bytes("x"));
  EXPECT_EQ(result.status, IoStatus::Error);
  EXPECT_EQ(result.error, EPIPE);
}

TEST(Socket, ConnectToClosedPortReportsErrorThroughTakeError) {
  // Bind, note the port, then close, leaving a port with no listener.
  const SocketAddress any = resolve("127.0.0.1", 0);
  SocketAddress target;
  {
    const FileDescriptor listener = listen_tcp(any, 1);
    target = local_address(listener.get());
  }
  ConnectOutcome outcome = connect_tcp(target);
  if (outcome.fd) {
    TcpStream stream(std::move(outcome.fd));
    // Loopback resolves synchronously enough that a short wait is unnecessary for the
    // kernel, but a failed handshake can be reported by either route.
    std::array<std::byte, 1> buffer{};
    for (int i = 0; i < 1000; ++i) {
      if (stream.read(buffer).status == IoStatus::Error) {
        return;
      }
    }
    FAIL() << "connection to a closed port did not fail";
  }
  EXPECT_EQ(outcome.error, ECONNREFUSED);
}

TEST(Socket, ListenerReportsEphemeralPort) {
  const FileDescriptor listener = listen_tcp(resolve("127.0.0.1", 0), 1);
  EXPECT_NE(local_address(listener.get()).port(), 0);
}

TEST(Socket, AcceptOnEmptyQueueIsNotAnError) {
  const FileDescriptor listener = listen_tcp(resolve("127.0.0.1", 0), 1);
  const AcceptOutcome outcome = accept_tcp(listener.get());
  EXPECT_FALSE(outcome.socket.has_value());
  EXPECT_EQ(outcome.error, 0);
}
