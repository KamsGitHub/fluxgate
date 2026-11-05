#pragma once

#include <concepts>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <variant>

namespace fluxgate::log {

enum class Level { Debug, Info, Warn, Error };

// Routes logs to stdout as one JSON object per line. Call once, before any
// logging and before starting threads.
void init(Level level);
void flush();

// A key-value pair on a log line. Only ever constructed within the full expression
// of a logging call, so it may safely borrow strings.
struct Field {
  std::string_view key;
  std::variant<std::string_view, std::int64_t, bool> value;

  Field(std::string_view k, std::string_view v) : key(k), value(v) {}
  Field(std::string_view k, const char* v) : key(k), value(std::string_view(v)) {}
  Field(std::string_view k, const std::string& v) : key(k), value(std::string_view(v)) {}
  Field(std::string_view k, bool v) : key(k), value(v) {}
  template <std::integral T>
    requires(!std::same_as<T, bool>)
  Field(std::string_view k, T v) : key(k), value(static_cast<std::int64_t>(v)) {}
};

using Fields = std::initializer_list<Field>;

void write(Level level, std::string_view event, Fields fields);

inline void debug(std::string_view event, Fields fields = {}) {
  write(Level::Debug, event, fields);
}
inline void info(std::string_view event, Fields fields = {}) {
  write(Level::Info, event, fields);
}
inline void warn(std::string_view event, Fields fields = {}) {
  write(Level::Warn, event, fields);
}
inline void error(std::string_view event, Fields fields = {}) {
  write(Level::Error, event, fields);
}

// Appends `text` as JSON string content. Logged values include client-controlled
// bytes, so everything outside printable ASCII is escaped: a hostile request
// target cannot forge log lines or produce invalid JSON.
void append_json_escaped(std::string& out, std::string_view text);

// The part of a log line after the timestamp and level, exposed for testing.
[[nodiscard]] std::string render_fields(std::string_view event, Fields fields);

}  // namespace fluxgate::log
