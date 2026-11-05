#include "fluxgate/config/config.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>

namespace fluxgate::config {
namespace {

using Problems = std::vector<std::string>;
using Keys = std::initializer_list<std::string_view>;

std::string join(const Problems& problems) {
  std::string out = "invalid configuration:";
  for (const std::string& problem : problems) {
    out.append("\n  - ").append(problem);
  }
  return out;
}

// Rejects unknown and repeated keys. yaml-cpp keeps duplicates, and lookups would
// quietly use one of them, so a repeat must be caught while iterating.
void check_keys(const YAML::Node& map, const std::string& path, Keys allowed, Problems& problems) {
  std::set<std::string> seen;
  for (const auto& entry : map) {
    const auto key = entry.first.as<std::string>();
    if (std::ranges::find(allowed, key) == allowed.end()) {
      problems.push_back("unknown key '" + path + key + "'");
    }
    if (!seen.insert(key).second) {
      problems.push_back("duplicate key '" + path + key + "'");
    }
  }
}

// Returns the section, or an empty node if it is absent or not a mapping.
YAML::Node section(const YAML::Node& root, const char* name, Problems& problems) {
  const YAML::Node node = root[name];
  if (node && !node.IsMap()) {
    problems.push_back(std::string("'") + name + "' must be a mapping");
    return {};
  }
  return node;
}

// Reads an integer in [min, max] into `out`, which keeps its default if absent.
template <typename T>
void read_integer(const YAML::Node& map, const char* key, const std::string& path, std::int64_t min,
                  std::int64_t max, T& out, Problems& problems) {
  if (!map || !map[key]) {
    return;
  }
  const std::string name = path + key;
  try {
    const auto value = map[key].as<std::int64_t>();
    if (value < min || value > max) {
      problems.push_back("'" + name + "' must be between " + std::to_string(min) + " and " +
                         std::to_string(max));
      return;
    }
    out = static_cast<T>(value);
  } catch (const YAML::Exception&) {
    problems.push_back("'" + name + "' must be an integer");
  }
}

void read_string(const YAML::Node& map, const char* key, const std::string& path, std::string& out,
                 Problems& problems) {
  if (!map || !map[key]) {
    return;
  }
  if (!map[key].IsScalar()) {
    problems.push_back("'" + path + key + "' must be a string");
    return;
  }
  out = map[key].as<std::string>();
}

constexpr std::int64_t kMaxTimeoutMs = 24LL * 60 * 60 * 1000;
constexpr std::int64_t kMaxPort = std::numeric_limits<std::uint16_t>::max();

void read_timeout(const YAML::Node& map, const char* key, std::chrono::milliseconds& out,
                  Problems& problems) {
  std::int64_t value = out.count();
  read_integer(map, key, "timeouts.", 1, kMaxTimeoutMs, value, problems);
  out = std::chrono::milliseconds(value);
}

void read_listener(const YAML::Node& root, Config& config, Problems& problems) {
  const YAML::Node node = section(root, "listener", problems);
  if (node) {
    check_keys(node, "listener.", {"address", "port", "backlog"}, problems);
  }
  read_string(node, "address", "listener.", config.listener.address, problems);
  read_integer(node, "port", "listener.", 0, kMaxPort, config.listener.port, problems);
  read_integer(node, "backlog", "listener.", 1, 65535, config.listener.backlog, problems);
  if (config.listener.address.empty()) {
    problems.emplace_back("'listener.address' must not be empty");
  }
}

void read_upstream(const YAML::Node& root, Config& config, Problems& problems) {
  const YAML::Node node = section(root, "upstream", problems);
  if (node) {
    check_keys(node, "upstream.", {"host", "port"}, problems);
  }
  read_string(node, "host", "upstream.", config.upstream.host, problems);
  read_integer(node, "port", "upstream.", 1, kMaxPort, config.upstream.port, problems);
  if (config.upstream.host.empty()) {
    problems.emplace_back("'upstream.host' is required");
  }
  if (config.upstream.port == 0 && !(node && node["port"])) {
    problems.emplace_back("'upstream.port' is required");
  }
}

void read_limits(const YAML::Node& root, Config& config, Problems& problems) {
  const YAML::Node node = section(root, "limits", problems);
  if (node) {
    check_keys(node, "limits.",
               {"max_header_bytes", "max_headers", "max_request_body_bytes", "io_buffer_bytes"},
               problems);
  }
  auto& limits = config.limits;
  read_integer(node, "max_header_bytes", "limits.", 256, std::int64_t{1024} * 1024,
               limits.max_header_bytes, problems);
  read_integer(node, "max_headers", "limits.", 1, 10'000, limits.max_headers, problems);
  read_integer(node, "max_request_body_bytes", "limits.", 0,
               std::numeric_limits<std::int64_t>::max(), limits.max_request_body_bytes, problems);
  read_integer(node, "io_buffer_bytes", "limits.", 1024, std::int64_t{16} * 1024 * 1024,
               limits.io_buffer_bytes, problems);
}

void read_timeouts(const YAML::Node& root, Config& config, Problems& problems) {
  const YAML::Node node = section(root, "timeouts", problems);
  if (node) {
    check_keys(node, "timeouts.",
               {"request_header_ms", "connect_ms", "response_header_ms", "idle_ms", "exchange_ms"},
               problems);
  }
  auto& timeouts = config.timeouts;
  read_timeout(node, "request_header_ms", timeouts.request_header, problems);
  read_timeout(node, "connect_ms", timeouts.connect, problems);
  read_timeout(node, "response_header_ms", timeouts.response_header, problems);
  read_timeout(node, "idle_ms", timeouts.idle, problems);
  read_timeout(node, "exchange_ms", timeouts.exchange, problems);

  if (timeouts.exchange < timeouts.request_header || timeouts.exchange < timeouts.connect ||
      timeouts.exchange < timeouts.response_header) {
    problems.emplace_back(
        "'timeouts.exchange_ms' caps the whole exchange, so it must not be shorter than "
        "request_header_ms, connect_ms or response_header_ms");
  }
}

void read_logging(const YAML::Node& root, Config& config, Problems& problems) {
  const YAML::Node node = section(root, "logging", problems);
  if (node) {
    check_keys(node, "logging.", {"level"}, problems);
  }
  std::string level = "info";
  read_string(node, "level", "logging.", level, problems);
  if (level == "debug") {
    config.log_level = log::Level::Debug;
  } else if (level == "info") {
    config.log_level = log::Level::Info;
  } else if (level == "warn") {
    config.log_level = log::Level::Warn;
  } else if (level == "error") {
    config.log_level = log::Level::Error;
  } else {
    problems.emplace_back("'logging.level' must be one of debug, info, warn, error");
  }
}

}  // namespace

ConfigError::ConfigError(Problems problems)
    : std::runtime_error(join(problems)), problems_(std::move(problems)) {}

Config parse_config(std::string_view yaml) {
  YAML::Node root;
  try {
    root = YAML::Load(std::string(yaml));
  } catch (const YAML::Exception& error) {
    throw ConfigError({std::string("YAML syntax error: ") + error.what()});
  }
  if (!root.IsMap()) {
    throw ConfigError({"the top level of the configuration must be a mapping"});
  }

  Config config;
  Problems problems;
  check_keys(root, "", {"listener", "upstream", "limits", "timeouts", "logging"}, problems);
  read_listener(root, config, problems);
  read_upstream(root, config, problems);
  read_limits(root, config, problems);
  read_timeouts(root, config, problems);
  read_logging(root, config, problems);

  // A textual comparison only: it cannot see through DNS names or wildcard binds, but
  // it stops the obvious self-proxying loop.
  if (config.listener.address == config.upstream.host &&
      config.listener.port == config.upstream.port) {
    problems.emplace_back("'upstream' must not point at the proxy's own listener");
  }

  if (!problems.empty()) {
    throw ConfigError(std::move(problems));
  }
  return config;
}

Config load_config(const std::filesystem::path& path) {
  std::ifstream file(path);
  if (!file) {
    throw ConfigError({"cannot read configuration file '" + path.string() + "'"});
  }
  std::ostringstream content;
  content << file.rdbuf();
  return parse_config(content.str());
}

}  // namespace fluxgate::config
