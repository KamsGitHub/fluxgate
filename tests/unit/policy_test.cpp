#include "fluxgate/http/policy.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace fluxgate::http;

namespace {

MessageHead request(std::vector<Header> headers = {{"Host", "example.com"}}) {
  MessageHead head;
  head.method = "GET";
  head.target = "/path?q=1";
  head.version_major = 1;
  head.version_minor = 1;
  head.headers = std::move(headers);
  return head;
}

MessageHead response(int status, std::vector<Header> headers) {
  MessageHead head;
  head.status = status;
  head.reason = "Reason";
  head.version_major = 1;
  head.version_minor = 1;
  head.headers = std::move(headers);
  return head;
}

bool has(const std::string& text, std::string_view needle) {
  return text.find(needle) != std::string::npos;
}

// Case-insensitive search for a header line, since name case is preserved.
bool has_header(const std::string& rendered, std::string_view name) {
  std::string lower_text = rendered;
  std::string lower_name = "\r\n" + std::string(name) + ":";
  for (auto* s : {&lower_text, &lower_name}) {
    for (char& c : *s) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
  }
  return has(lower_text, lower_name);
}

constexpr std::uint64_t kMaxBody = 1024;

}  // namespace

TEST(Validate, AcceptsPlainRequest) {
  EXPECT_EQ(validate_request(request(), kMaxBody), 0);
}

TEST(Validate, RejectsOtherHttpVersions) {
  auto head = request();
  head.version_minor = 0;
  EXPECT_EQ(validate_request(head, kMaxBody), 505);
  head.version_major = 2;
  EXPECT_EQ(validate_request(head, kMaxBody), 505);
}

TEST(Validate, RefusesConnectSoItCannotTunnel) {
  auto head = request();
  head.method = "CONNECT";
  head.target = "internal:22";
  EXPECT_EQ(validate_request(head, kMaxBody), 501);
}

TEST(Validate, RefusesTargetsThatAreNotOriginForm) {
  auto head = request();
  for (const char* target : {"http://evil.example/", "*", "evil.example:80", ""}) {
    head.target = target;
    EXPECT_EQ(validate_request(head, kMaxBody), 400) << target;
  }
}

TEST(Validate, RequiresExactlyOneValidHost) {
  EXPECT_EQ(validate_request(request({}), kMaxBody), 400);
  EXPECT_EQ(validate_request(request({{"Host", "a"}, {"host", "b"}}), kMaxBody), 400);
  EXPECT_EQ(validate_request(request({{"Host", ""}}), kMaxBody), 400);
  EXPECT_EQ(validate_request(request({{"Host", "a b"}}), kMaxBody), 400);
  EXPECT_EQ(validate_request(request({{"Host", "a/b"}}), kMaxBody), 400);
  EXPECT_EQ(validate_request(request({{"Host", "user@a"}}), kMaxBody), 400);
}

TEST(Validate, AcceptsHostForms) {
  for (const char* host : {"example.com", "example.com:8080", "[::1]:80", "127.0.0.1"}) {
    EXPECT_EQ(validate_request(request({{"Host", host}}), kMaxBody), 0) << host;
  }
}

TEST(Validate, RefusesTransferEncodingForNow) {
  EXPECT_EQ(validate_request(request({{"Host", "a"}, {"Transfer-Encoding", "chunked"}}), kMaxBody),
            501);
}

TEST(Validate, EnforcesDeclaredBodyLimit) {
  auto head = request();
  head.content_length = kMaxBody;
  EXPECT_EQ(validate_request(head, kMaxBody), 0);
  head.content_length = kMaxBody + 1;
  EXPECT_EQ(validate_request(head, kMaxBody), 413);
}

TEST(Validate, OnlyContinueExpectationIsSupported) {
  EXPECT_EQ(validate_request(request({{"Host", "a"}, {"Expect", "100-Continue"}}), kMaxBody), 0);
  EXPECT_EQ(validate_request(request({{"Host", "a"}, {"Expect", "magic"}}), kMaxBody), 417);
}

TEST(ExpectsContinue, RequiresABody) {
  auto head = request({{"Host", "a"}, {"Expect", "100-continue"}});
  EXPECT_FALSE(expects_continue(head));
  head.content_length = 10;
  EXPECT_TRUE(expects_continue(head));
}

TEST(RenderRequest, PreservesStartLineHostAndEndToEndHeaders) {
  const std::string out =
      render_upstream_request(request({{"Host", "example.com"}, {"X-Custom", "v"}}), "10.0.0.1");
  EXPECT_TRUE(out.starts_with("GET /path?q=1 HTTP/1.1\r\n"));
  EXPECT_TRUE(has(out, "\r\nHost: example.com\r\n") || has(out, "HTTP/1.1\r\nHost: example.com"));
  EXPECT_TRUE(has(out, "X-Custom: v\r\n"));
  EXPECT_TRUE(out.ends_with("\r\n\r\n"));
}

TEST(RenderRequest, AddsProxyHeaders) {
  const std::string out = render_upstream_request(request(), "10.0.0.1");
  EXPECT_TRUE(has(out, "X-Forwarded-For: 10.0.0.1\r\n"));
  EXPECT_TRUE(has(out, "X-Forwarded-Proto: http\r\n"));
  EXPECT_TRUE(has(out, "Via: 1.1 fluxgate\r\n"));
  EXPECT_TRUE(has(out, "Connection: close\r\n"));
}

TEST(RenderRequest, StripsHopByHopHeaders) {
  const std::string out = render_upstream_request(request({{"Host", "a"},
                                                           {"Connection", "keep-alive, X-Internal"},
                                                           {"Keep-Alive", "timeout=5"},
                                                           {"Proxy-Connection", "keep-alive"},
                                                           {"Proxy-Authorization", "Basic secret"},
                                                           {"TE", "trailers"},
                                                           {"Trailer", "X"},
                                                           {"Upgrade", "h2c"},
                                                           {"Expect", "100-continue"},
                                                           {"X-Internal", "listed in Connection"}}),
                                                  "10.0.0.1");
  for (const char* name : {"Keep-Alive", "Proxy-Connection", "Proxy-Authorization", "TE", "Trailer",
                           "Upgrade", "Expect", "X-Internal"}) {
    EXPECT_FALSE(has_header(out, name)) << name;
  }
  EXPECT_FALSE(has(out, "Connection: keep-alive"));
}

TEST(RenderRequest, ClientCannotForgeProvenance) {
  const std::string out = render_upstream_request(request({{"Host", "a"},
                                                           {"X-Forwarded-For", "1.1.1.1"},
                                                           {"x-forwarded-proto", "https"},
                                                           {"X-Forwarded-Host", "evil"},
                                                           {"X-Real-IP", "1.1.1.1"},
                                                           {"Forwarded", "for=1.1.1.1"}}),
                                                  "10.0.0.1");
  EXPECT_FALSE(has(out, "1.1.1.1"));
  EXPECT_FALSE(has(out, "evil"));
  EXPECT_FALSE(has(out, "https"));
}

TEST(RenderRequest, UnderscoreVariantsCannotForgeProvenance) {
  const std::string out =
      render_upstream_request(request({{"Host", "a"}, {"X_Forwarded_For", "1.1.1.1"}}), "10.0.0.1");
  EXPECT_FALSE(has(out, "1.1.1.1"));
}

TEST(RenderRequest, ConnectionCannotDeleteFramingOrHost) {
  auto head = request({{"Host", "a"},
                       {"Content-Length", "5"},
                       {"Connection", "Content-Length, Host, Transfer-Encoding"}});
  head.content_length = 5;
  const std::string out = render_upstream_request(head, "10.0.0.1");
  EXPECT_TRUE(has(out, "Content-Length: 5\r\n"));
  EXPECT_TRUE(has_header(out, "Host"));
}

TEST(RenderResponse, KeepsFramingAndMarksClose) {
  const std::string out = render_client_response(
      response(200, {{"Transfer-Encoding", "chunked"}, {"Trailer", "X"}, {"Server", "up"}}));
  EXPECT_TRUE(out.starts_with("HTTP/1.1 200 Reason\r\n"));
  EXPECT_TRUE(has(out, "Transfer-Encoding: chunked\r\n"));
  EXPECT_TRUE(has(out, "Trailer: X\r\n"));
  EXPECT_TRUE(has(out, "Server: up\r\n"));
  EXPECT_TRUE(has(out, "Connection: close\r\n"));
  EXPECT_TRUE(has(out, "Via: 1.1 fluxgate\r\n"));
}

TEST(RenderResponse, StripsHopByHopHeaders) {
  const std::string out = render_client_response(response(200, {{"Connection", "keep-alive, X-Hop"},
                                                                {"Keep-Alive", "t"},
                                                                {"X-Hop", "1"},
                                                                {"Upgrade", "x"},
                                                                {"Proxy-Authenticate", "Basic"},
                                                                {"Content-Length", "0"}}));
  EXPECT_FALSE(has_header(out, "Keep-Alive"));
  EXPECT_FALSE(has_header(out, "X-Hop"));
  EXPECT_FALSE(has_header(out, "Upgrade"));
  EXPECT_FALSE(has_header(out, "Proxy-Authenticate"));
  EXPECT_TRUE(has(out, "Content-Length: 0\r\n"));
  EXPECT_FALSE(has(out, "keep-alive"));
}

TEST(RenderResponse, InterimCarriesNoConnectionHeader) {
  const std::string out = render_interim_response(response(103, {{"Link", "</a>"}}));
  EXPECT_TRUE(has(out, "Link: </a>\r\n"));
  EXPECT_FALSE(has_header(out, "Connection"));
}

TEST(RenderError, IsCompleteAndClosesConnection) {
  const std::string out = render_error_response(400, false);
  EXPECT_TRUE(out.starts_with("HTTP/1.1 400 Bad Request\r\n"));
  EXPECT_TRUE(has(out, "Content-Length: 12\r\n"));
  EXPECT_TRUE(has(out, "Connection: close\r\n"));
  EXPECT_TRUE(out.ends_with("\r\n\r\nBad Request\n"));
}

TEST(RenderError, HeadResponseKeepsLengthButHasNoBody) {
  const std::string out = render_error_response(502, true);
  EXPECT_TRUE(has(out, "Content-Length: 12\r\n"));
  EXPECT_TRUE(out.ends_with("\r\n\r\n"));
}

TEST(TargetPath, DropsQueryString) {
  EXPECT_EQ(target_path("/login?token=secret"), "/login");
  EXPECT_EQ(target_path("/plain"), "/plain");
}
