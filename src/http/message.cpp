#include "fluxgate/http/message.hpp"

#include <algorithm>

namespace fluxgate::http {
namespace {

// ASCII only and locale independent, as HTTP field names require.
constexpr char lower(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

}  // namespace

bool iequals(std::string_view a, std::string_view b) noexcept {
  return std::ranges::equal(a, b, [](char x, char y) { return lower(x) == lower(y); });
}

const Header* MessageHead::find(std::string_view name) const noexcept {
  const auto it =
      std::ranges::find_if(headers, [name](const Header& h) { return iequals(h.name, name); });
  return it == headers.end() ? nullptr : &*it;
}

std::size_t MessageHead::count(std::string_view name) const noexcept {
  return static_cast<std::size_t>(std::count_if(
      headers.begin(), headers.end(), [name](const Header& h) { return iequals(h.name, name); }));
}

}  // namespace fluxgate::http
