#include "fluxgate/config/config.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>

using namespace fluxgate::config;
using namespace std::chrono_literals;

namespace {

const std::string kMinimal = "upstream: {host: 127.0.0.1, port: 9000}\n";

// Returns all problems in the document, failing the test if there are none.
std::string problems_in(const std::string& yaml) {
  try {
    (void)parse_config(yaml);
  } catch (const ConfigError& error) {
    return error.what();
  }
  ADD_FAILURE() << "expected rejection of:\n" << yaml;
  return {};
}

}  // namespace

TEST(Config, MinimalFileGetsDocumentedDefaults) {
  const Config config = parse_config(kMinimal);
  EXPECT_EQ(config.listener.address, "127.0.0.1");
  EXPECT_EQ(config.listener.port, 8080);
  EXPECT_EQ(config.upstream.port, 9000);
  EXPECT_EQ(config.limits.max_header_bytes, 16384U);
  EXPECT_EQ(config.timeouts.exchange, 300s);
  EXPECT_EQ(config.log_level, fluxgate::log::Level::Info);
}

TEST(Config, ReadsEveryOverride) {
  const Config config = parse_config(R"(
listener: {address: 0.0.0.0, port: 0, backlog: 16}
upstream: {host: backend, port: 1}
limits: {max_header_bytes: 1024, max_headers: 5, max_request_body_bytes: 0, io_buffer_bytes: 2048}
timeouts:
  request_header_ms: 1
  connect_ms: 2
  response_header_ms: 3
  idle_ms: 4
  exchange_ms: 5
logging: {level: warn}
)");
  EXPECT_EQ(config.listener.port, 0);
  EXPECT_EQ(config.listener.backlog, 16);
  EXPECT_EQ(config.limits.max_headers, 5U);
  EXPECT_EQ(config.limits.max_request_body_bytes, 0U);
  EXPECT_EQ(config.timeouts.idle, 4ms);
  EXPECT_EQ(config.log_level, fluxgate::log::Level::Warn);
}

TEST(Config, RequiresUpstream) {
  const std::string problems = problems_in("listener: {port: 1}\n");
  EXPECT_NE(problems.find("upstream.host"), std::string::npos);
  EXPECT_NE(problems.find("upstream.port"), std::string::npos);
}

TEST(Config, ReportsEveryProblemAtOnce) {
  try {
    (void)parse_config("bogus: 1\nupstream: {host: h, port: 99999}\nlogging: {level: loud}\n");
    FAIL();
  } catch (const ConfigError& error) {
    EXPECT_EQ(error.problems().size(), 3U);
  }
}

TEST(Config, RejectsUnknownKeysAtEveryLevel) {
  EXPECT_NE(problems_in(kMinimal + "timeout: {}\n").find("'timeout'"), std::string::npos);
  EXPECT_NE(problems_in(kMinimal + "timeouts: {idle: 5}\n").find("timeouts.idle"),
            std::string::npos);
}

TEST(Config, RejectsDuplicateKeys) {
  EXPECT_NE(problems_in(kMinimal + "limits: {max_headers: 1, max_headers: 2}\n").find("duplicate"),
            std::string::npos);
}

TEST(Config, RejectsWrongTypesAndRanges) {
  EXPECT_NE(problems_in(kMinimal + "listener: {port: http}\n").find("integer"), std::string::npos);
  EXPECT_NE(problems_in(kMinimal + "listener: {port: -1}\n").find("between"), std::string::npos);
  EXPECT_NE(problems_in(kMinimal + "timeouts: {idle_ms: 0}\n").find("between"), std::string::npos);
  EXPECT_NE(problems_in(kMinimal + "limits: 5\n").find("mapping"), std::string::npos);
}

TEST(Config, RejectsContradictoryTimeouts) {
  EXPECT_NE(problems_in(kMinimal + "timeouts: {exchange_ms: 100}\n").find("exchange_ms"),
            std::string::npos);
}

TEST(Config, RejectsProxyingToItself) {
  EXPECT_NE(problems_in("listener: {address: a, port: 1}\nupstream: {host: a, port: 1}\n")
                .find("own listener"),
            std::string::npos);
}

TEST(Config, RejectsYamlSyntaxErrors) {
  EXPECT_NE(problems_in("upstream: [").find("YAML"), std::string::npos);
}

TEST(Config, RejectsNonMappingRoot) {
  EXPECT_NE(problems_in("- a\n- b\n").find("mapping"), std::string::npos);
}

TEST(Config, ReportsMissingFile) {
  EXPECT_THROW((void)load_config("/nonexistent/fluxgate.yaml"), ConfigError);
}
