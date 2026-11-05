#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fluxgate::http {

struct Header {
  std::string name;
  std::string value;
};

// The parsed start line and headers of one HTTP message. Request-only and
// response-only fields are left empty for the other kind.
struct MessageHead {
  std::string method;
  std::string target;
  int status = 0;
  std::string reason;
  int version_major = 0;
  int version_minor = 0;
  std::vector<Header> headers;
  std::optional<std::uint64_t> content_length;

  // First header with this name (case-insensitive), or nullptr.
  [[nodiscard]] const Header* find(std::string_view name) const noexcept;
  [[nodiscard]] std::size_t count(std::string_view name) const noexcept;
};

[[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept;

}  // namespace fluxgate::http
