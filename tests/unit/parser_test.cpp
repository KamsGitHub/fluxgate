#include "fluxgate/http/parser.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

using namespace fluxgate::http;

namespace {

std::span<const std::byte> bytes(std::string_view text) {
  return std::as_bytes(std::span(text));
}

struct Parsed {
  bool head = false;
  bool complete = false;
  ParseError error = ParseError::None;
  std::size_t consumed = 0;
};

// Feeds `wire` in pieces of `chunk` bytes, always continuing after head completion
// the way a caller must, and stops at the end of the message or at an error.
Parsed run(Http1Parser& parser, std::string_view wire, std::size_t chunk) {
  Parsed parsed;
  std::size_t offset = 0;
  while (true) {
    const std::size_t end = std::min(wire.size(), offset + chunk);
    const auto result = parser.feed(bytes(wire.substr(offset, end - offset)));
    offset += result.consumed;
    parsed.consumed = offset;
    switch (result.event) {
      case ParseEvent::Error:
        parsed.error = parser.error();
        return parsed;
      case ParseEvent::HeadComplete:
        parsed.head = true;
        break;
      case ParseEvent::MessageComplete:
        parsed.complete = true;
        return parsed;
      case ParseEvent::NeedMore:
        if (offset >= wire.size()) {
          return parsed;
        }
        break;
    }
  }
}

Parsed parse_request(std::string_view wire, HeadLimits limits = {}) {
  Http1Parser parser(Http1Parser::Mode::Request, limits);
  return run(parser, wire, wire.size() + 1);
}

constexpr std::string_view kPost =
    "POST /submit?x=1 HTTP/1.1\r\n"
    "Host: example.com\r\n"
    "Content-Length: 5\r\n"
    "X-Trailing:   padded  \r\n"
    "\r\n"
    "hello";

}  // namespace

TEST(Parser, ParsesRequestFields) {
  Http1Parser parser(Http1Parser::Mode::Request, {});
  const Parsed parsed = run(parser, kPost, kPost.size());
  EXPECT_TRUE(parsed.complete);
  EXPECT_EQ(parsed.consumed, kPost.size());
  const MessageHead& head = parser.head();
  EXPECT_EQ(head.method, "POST");
  EXPECT_EQ(head.target, "/submit?x=1");
  EXPECT_EQ(head.version_major, 1);
  EXPECT_EQ(head.version_minor, 1);
  EXPECT_EQ(head.content_length, 5U);
  ASSERT_NE(head.find("host"), nullptr);
  EXPECT_EQ(head.find("HOST")->value, "example.com");
  EXPECT_EQ(head.find("x-trailing")->value, "padded");
}

// The central guarantee: a request split at any point parses identically.
TEST(Parser, EverySplitPointProducesTheSameMessage) {
  for (std::size_t split = 1; split < kPost.size(); ++split) {
    Http1Parser parser(Http1Parser::Mode::Request, {});
    std::size_t offset = 0;
    bool complete = false;
    for (const std::string_view piece : {kPost.substr(0, split), kPost.substr(split)}) {
      std::size_t used = 0;
      while (true) {
        const auto result = parser.feed(bytes(piece.substr(used)));
        used += result.consumed;
        offset += result.consumed;
        ASSERT_NE(result.event, ParseEvent::Error) << "split at " << split;
        if (result.event == ParseEvent::MessageComplete) {
          complete = true;
          break;
        }
        if (result.event == ParseEvent::NeedMore) {
          break;
        }
      }
    }
    EXPECT_TRUE(complete) << "split at " << split;
    EXPECT_EQ(offset, kPost.size()) << "split at " << split;
    EXPECT_EQ(parser.head().find("Host")->value, "example.com") << "split at " << split;
    EXPECT_EQ(parser.head().target, "/submit?x=1") << "split at " << split;
  }
}

TEST(Parser, OneByteAtATime) {
  Http1Parser parser(Http1Parser::Mode::Request, {});
  const Parsed parsed = run(parser, kPost, 1);
  EXPECT_TRUE(parsed.complete);
  EXPECT_EQ(parsed.consumed, kPost.size());
}

TEST(Parser, HeadEndsExactlyAfterBlankLine) {
  Http1Parser parser(Http1Parser::Mode::Request, {});
  const auto result = parser.feed(bytes(kPost));
  EXPECT_EQ(result.event, ParseEvent::HeadComplete);
  EXPECT_EQ(result.consumed, kPost.find("hello"));
}

TEST(Parser, BodylessMessageCompletesWithoutFurtherInput) {
  constexpr std::string_view wire = "GET / HTTP/1.1\r\nHost: a\r\n\r\n";
  Http1Parser parser(Http1Parser::Mode::Request, {});
  EXPECT_EQ(parser.feed(bytes(wire)).event, ParseEvent::HeadComplete);
  const auto result = parser.feed({});
  EXPECT_EQ(result.event, ParseEvent::MessageComplete);
  EXPECT_EQ(result.consumed, 0U);
}

// Bytes after the message must stay unconsumed, or they could be relayed as part of it.
TEST(Parser, NeverConsumesPastMessageBoundary) {
  const std::string wire = std::string(kPost) + "GET /smuggled HTTP/1.1\r\nHost: a\r\n\r\n";
  EXPECT_EQ(parse_request(wire).consumed, kPost.size());
}

TEST(Parser, BodyBoundaryIsExactAcrossFeeds) {
  Http1Parser parser(Http1Parser::Mode::Request, {});
  const auto head = parser.feed(bytes("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 3\r\n\r\n"));
  EXPECT_EQ(head.event, ParseEvent::HeadComplete);
  EXPECT_EQ(parser.feed(bytes("ab")).event, ParseEvent::NeedMore);
  const auto last = parser.feed(bytes("cXYZ"));
  EXPECT_EQ(last.event, ParseEvent::MessageComplete);
  EXPECT_EQ(last.consumed, 1U);
}

TEST(Parser, UpgradeHeadersDoNotStopTheMessage) {
  constexpr std::string_view wire =
      "POST / HTTP/1.1\r\nHost: a\r\nConnection: upgrade\r\nUpgrade: h2c\r\n"
      "Content-Length: 2\r\n\r\nhi";
  const Parsed parsed = parse_request(wire);
  EXPECT_TRUE(parsed.complete);
  EXPECT_EQ(parsed.consumed, wire.size());
}

TEST(Parser, UpgradeWithoutBodyCompletes) {
  const Parsed parsed =
      parse_request("GET / HTTP/1.1\r\nHost: a\r\nConnection: upgrade\r\nUpgrade: h2c\r\n\r\n");
  EXPECT_TRUE(parsed.complete);
  EXPECT_EQ(parsed.error, ParseError::None);
}

// --- Limits ---

TEST(Parser, HeadAtExactLimitIsAccepted) {
  constexpr std::string_view wire = "GET / HTTP/1.1\r\nHost: a\r\n\r\n";
  const Parsed parsed = parse_request(wire, {.max_head_bytes = wire.size(), .max_headers = 10});
  EXPECT_TRUE(parsed.complete);
}

TEST(Parser, HeadOneByteOverLimitIsRejected) {
  constexpr std::string_view wire = "GET / HTTP/1.1\r\nHost: a\r\n\r\n";
  const Parsed parsed = parse_request(wire, {.max_head_bytes = wire.size() - 1, .max_headers = 10});
  EXPECT_EQ(parsed.error, ParseError::HeadTooLarge);
}

TEST(Parser, HeadLimitHoldsWhenFedByteByByte) {
  Http1Parser parser(Http1Parser::Mode::Request, {.max_head_bytes = 32, .max_headers = 10});
  const std::string wire = "GET / HTTP/1.1\r\nX-Pad: " + std::string(1000, 'a');
  EXPECT_EQ(run(parser, wire, 1).error, ParseError::HeadTooLarge);
}

TEST(Parser, HeaderCountLimit) {
  const Parsed parsed = parse_request("GET / HTTP/1.1\r\nA: 1\r\nB: 2\r\nC: 3\r\n\r\n",
                                      {.max_head_bytes = 1024, .max_headers = 2});
  EXPECT_EQ(parsed.error, ParseError::TooManyHeaders);
}

// --- Framing attacks. llhttp's strict mode must keep refusing each of these; a
// dependency upgrade or a lenient flag that weakened one would fail here. ---

class RejectedRequest : public testing::TestWithParam<std::string_view> {};

TEST_P(RejectedRequest, IsMalformed) {
  EXPECT_EQ(parse_request(GetParam()).error, ParseError::Malformed) << GetParam();
}

INSTANTIATE_TEST_SUITE_P(
    Smuggling, RejectedRequest,
    testing::Values(
        // Conflicting and repeated lengths.
        "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nx",
        "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx",
        "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1, 2\r\n\r\nx",
        // Content-Length alongside Transfer-Encoding, in both orders.
        "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\nContent-Length: 1\r\n\r\n",
        // Non-numeric and signed lengths.
        "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: abc\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: -1\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: +1\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 0x10\r\n\r\n",
        // Length overflowing 64 bits.
        "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 99999999999999999999999\r\n\r\n",
        // Whitespace tricks in field names.
        "POST / HTTP/1.1\r\nHost: a\r\nContent-Length : 1\r\n\r\nx",
        "POST / HTTP/1.1\r\nHost: a\r\n Content-Length: 1\r\n\r\nx",
        "POST / HTTP/1.1\r\nHost: a\r\n\tContent-Length: 1\r\n\r\nx",
        // Bare line endings, which other parsers may split differently.
        "GET / HTTP/1.1\nHost: a\n\n", "GET / HTTP/1.1\r\nHost: a\nX: 1\r\n\r\n",
        "GET / HTTP/1.1\r\nX: a\rContent-Length: 5\r\nHost: a\r\n\r\n",
        // Control bytes in values.
        std::string_view("GET / HTTP/1.1\r\nHost: a\r\nX: a\0b\r\n\r\n", 35),
        // Malformed start lines.
        "GET /a b HTTP/1.1\r\nHost: a\r\n\r\n", "GET / HTTP/1.1 \r\nHost: a\r\n\r\n",
        "GET / HTTQ/1.1\r\nHost: a\r\n\r\n", "BOGUS / HTTP/1.1\r\nHost: a\r\n\r\n",
        "\x01GET / HTTP/1.1\r\nHost: a\r\n\r\n",
        // Header without a colon.
        "GET / HTTP/1.1\r\nHost: a\r\nnocolon\r\n\r\n"));

// Unlike the cases above this is not refused: llhttp skips extra spaces before the
// target even in strict mode. It cannot desynchronise anything, because the upstream
// never sees these bytes; the start line is rebuilt from the parsed fields.
TEST(Parser, ExtraSpacesBeforeTargetAreNormalisedAway) {
  Http1Parser parser(Http1Parser::Mode::Request, {});
  EXPECT_TRUE(run(parser, "GET   /a HTTP/1.1\r\nHost: a\r\n\r\n", 64).complete);
  EXPECT_EQ(parser.head().target, "/a");
}

TEST(Parser, TrailingWhitespaceIsNotPartOfTheValue) {
  Http1Parser parser(Http1Parser::Mode::Request, {});
  EXPECT_TRUE(run(parser, "GET / HTTP/1.1\r\nHost: a \t \r\nX-Empty:   \r\n\r\n", 3).complete);
  EXPECT_EQ(parser.head().find("Host")->value, "a");
  EXPECT_EQ(parser.head().find("X-Empty")->value, "");
}

// --- Responses ---

TEST(ParserResponse, ParsesStatusAndReason) {
  Http1Parser parser(Http1Parser::Mode::Response, {});
  const auto r = parser.feed(bytes("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n"));
  EXPECT_EQ(r.event, ParseEvent::HeadComplete);
  EXPECT_EQ(parser.head().status, 404);
  EXPECT_EQ(parser.head().reason, "Not Found");
  EXPECT_EQ(parser.feed({}).event, ParseEvent::MessageComplete);
}

TEST(ParserResponse, ChunkedBodyEndsAtTerminator) {
  constexpr std::string_view message =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n";
  const std::string wire = std::string(message) + "garbage";
  Http1Parser parser(Http1Parser::Mode::Response, {});
  const Parsed parsed = run(parser, wire, 1);
  EXPECT_TRUE(parsed.complete);
  EXPECT_EQ(parsed.consumed, message.size());
}

TEST(ParserResponse, InvalidChunkSizeIsAnError) {
  Http1Parser parser(Http1Parser::Mode::Response, {});
  EXPECT_EQ(run(parser, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n", 64).error,
            ParseError::Malformed);
}

TEST(ParserResponse, HeadResponseIgnoresDeclaredLength) {
  constexpr std::string_view wire = "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n";
  Http1Parser parser(Http1Parser::Mode::Response, {});
  parser.expect_no_body();
  const auto head = parser.feed(bytes(wire));
  EXPECT_EQ(head.event, ParseEvent::HeadComplete);
  EXPECT_EQ(head.consumed, wire.size());
  EXPECT_EQ(parser.head().status, 200);
  EXPECT_EQ(parser.feed({}).event, ParseEvent::MessageComplete);
}

TEST(ParserResponse, Status204And304HaveNoBody) {
  for (const std::string_view wire : {"HTTP/1.1 204 No Content\r\n\r\n",
                                      "HTTP/1.1 304 Not Modified\r\nContent-Length: 9\r\n\r\n"}) {
    Http1Parser parser(Http1Parser::Mode::Response, {});
    EXPECT_TRUE(run(parser, wire, wire.size()).complete) << wire;
  }
}

TEST(ParserResponse, CloseDelimitedBodyEndsAtEof) {
  Http1Parser parser(Http1Parser::Mode::Response, {});
  EXPECT_EQ(parser.feed(bytes("HTTP/1.1 200 OK\r\n\r\n")).event, ParseEvent::HeadComplete);
  const auto body = parser.feed(bytes("data"));
  EXPECT_EQ(body.event, ParseEvent::NeedMore);
  EXPECT_EQ(body.consumed, 4U);
  EXPECT_TRUE(parser.finish());
}

TEST(ParserResponse, EofInsideDeclaredLengthIsTruncation) {
  Http1Parser parser(Http1Parser::Mode::Response, {});
  (void)parser.feed(bytes("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n"));
  (void)parser.feed(bytes("short"));
  EXPECT_FALSE(parser.finish());
  EXPECT_EQ(parser.error(), ParseError::Truncated);
}

TEST(ParserResponse, EofInsideChunkedBodyIsTruncation) {
  Http1Parser parser(Http1Parser::Mode::Response, {});
  (void)parser.feed(bytes("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"));
  (void)parser.feed(bytes("5\r\nhel"));
  EXPECT_FALSE(parser.finish());
}

TEST(ParserResponse, ConflictingFramingIsRejected) {
  Http1Parser parser(Http1Parser::Mode::Response, {});
  EXPECT_EQ(
      run(parser, "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n", 64).error,
      ParseError::Malformed);
}

TEST(ParserResponse, ResetAllowsANewMessageAfterInterimResponse) {
  Http1Parser parser(Http1Parser::Mode::Response, {});
  EXPECT_EQ(parser.feed(bytes("HTTP/1.1 103 Early Hints\r\nLink: </a>\r\n\r\n")).event,
            ParseEvent::HeadComplete);
  EXPECT_EQ(parser.head().status, 103);
  parser.reset();
  EXPECT_TRUE(parser.head().headers.empty());
  EXPECT_EQ(parser.feed(bytes("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")).event,
            ParseEvent::HeadComplete);
  EXPECT_EQ(parser.head().status, 200);
}
