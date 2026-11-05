#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "fluxgate/log/log.hpp"

namespace fluxgate::config {

struct ListenerConfig {
  std::string address = "127.0.0.1";
  std::uint16_t port = 8080;  // 0 asks the OS for a free port.
  int backlog = 128;
};

struct UpstreamConfig {
  std::string host;  // Required. Resolved once, at startup.
  std::uint16_t port = 0;
};

struct LimitsConfig {
  std::size_t max_header_bytes = std::size_t{16} * 1024;
  std::size_t max_headers = 100;
  std::uint64_t max_request_body_bytes = 10ULL * 1024 * 1024;
  std::size_t io_buffer_bytes = std::size_t{64} * 1024;
};

struct TimeoutsConfig {
  std::chrono::milliseconds request_header{10'000};
  std::chrono::milliseconds connect{2'000};
  std::chrono::milliseconds response_header{30'000};
  std::chrono::milliseconds idle{30'000};
  std::chrono::milliseconds exchange{300'000};
};

struct Config {
  ListenerConfig listener;
  UpstreamConfig upstream;
  LimitsConfig limits;
  TimeoutsConfig timeouts;
  log::Level log_level = log::Level::Info;
};

// Carries every problem found, so one run reports them all at once.
class ConfigError : public std::runtime_error {
 public:
  explicit ConfigError(std::vector<std::string> problems);
  [[nodiscard]] const std::vector<std::string>& problems() const noexcept { return problems_; }

 private:
  std::vector<std::string> problems_;
};

// Both parse and fully validate. Unknown keys are errors: a misspelt timeout that
// silently kept its default is exactly the kind of mistake validation is for.
[[nodiscard]] Config parse_config(std::string_view yaml);
[[nodiscard]] Config load_config(const std::filesystem::path& path);

}  // namespace fluxgate::config
