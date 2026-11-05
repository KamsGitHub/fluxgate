#include "fluxgate/log/log.hpp"

#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

#include <array>

namespace fluxgate::log {
namespace {

spdlog::level::level_enum to_spdlog(Level level) noexcept {
  switch (level) {
    case Level::Debug:
      return spdlog::level::debug;
    case Level::Info:
      return spdlog::level::info;
    case Level::Warn:
      return spdlog::level::warn;
    case Level::Error:
      return spdlog::level::err;
  }
  return spdlog::level::info;
}

}  // namespace

void init(Level level) {
  auto logger = spdlog::stdout_logger_mt("fluxgate");
  logger->set_level(to_spdlog(level));
  // Everything except the timestamp and level comes pre-rendered in the message,
  // which keeps spdlog's formatter away from client-controlled text.
  logger->set_pattern(R"({"ts":"%Y-%m-%dT%H:%M:%S.%e%z","level":"%l",%v})");
  // Each line reaches the OS at once so a crash loses nothing. That is one write
  // per request on the data path; moving to an async sink is a planned change.
  logger->flush_on(spdlog::level::debug);
  spdlog::set_default_logger(std::move(logger));
}

void flush() {
  spdlog::default_logger()->flush();
}

void write(Level level, std::string_view event, Fields fields) {
  const auto spdlog_level = to_spdlog(level);
  if (!spdlog::should_log(spdlog_level)) {
    return;
  }
  const std::string line = render_fields(event, fields);
  spdlog::log(spdlog_level, "{}", line);
}

void append_json_escaped(std::string& out, std::string_view text) {
  static constexpr std::array<char, 16> kHex = {'0', '1', '2', '3', '4', '5', '6', '7',
                                                '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (byte >= 0x20 && byte < 0x7f) {
      out.push_back(c);
    } else {
      out.append("\\u00");
      out.push_back(kHex[byte >> 4U]);
      out.push_back(kHex[byte & 0xfU]);
    }
  }
}

std::string render_fields(std::string_view event, Fields fields) {
  std::string out = "\"event\":\"";
  append_json_escaped(out, event);
  out.push_back('"');
  for (const Field& field : fields) {
    out.append(",\"");
    append_json_escaped(out, field.key);
    out.append("\":");
    if (const auto* text = std::get_if<std::string_view>(&field.value)) {
      out.push_back('"');
      append_json_escaped(out, *text);
      out.push_back('"');
    } else if (const auto* number = std::get_if<std::int64_t>(&field.value)) {
      out.append(std::to_string(*number));
    } else {
      out.append(std::get<bool>(field.value) ? "true" : "false");
    }
  }
  return out;
}

}  // namespace fluxgate::log
