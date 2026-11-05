#include "fluxgate/http/policy.hpp"

#include <algorithm>
#include <array>
#include <vector>

namespace fluxgate::http {
namespace {

constexpr std::string_view kCrlf = "\r\n";
constexpr std::string_view kVia = "Via: 1.1 fluxgate\r\n";

// Lowercased, with '_' folded to '-'. Some backends (CGI-derived ones in particular)
// cannot tell X_Forwarded_For from X-Forwarded-For, so a spoofing check that matched
// only the hyphenated form could be bypassed.
std::string normalize_name(std::string_view name) {
  std::string out(name);
  std::ranges::transform(out, out.begin(), [](char c) {
    if (c == '_') {
      return '-';
    }
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  });
  return out;
}

template <std::size_t N>
bool contains(const std::array<std::string_view, N>& names, std::string_view name) noexcept {
  return std::find(names.begin(), names.end(), name) != names.end();
}

constexpr std::array<std::string_view, 8> kRequestHopByHop = {
    "connection", "keep-alive", "proxy-connection",  "proxy-authorization",
    "te",         "trailer",    "transfer-encoding", "upgrade"};

constexpr std::array<std::string_view, 6> kResponseHopByHop = {
    "connection", "keep-alive", "proxy-connection", "proxy-authenticate", "te", "upgrade"};

// Provenance headers are only meaningful when written by FluxGate itself. It has no
// trusted downstream yet, so anything a client sends here is treated as forged.
constexpr std::array<std::string_view, 5> kClientProvenance = {
    "forwarded", "x-forwarded-for", "x-forwarded-host", "x-forwarded-proto", "x-real-ip"};

// A client must not be able to delete these by naming them in Connection. Removing
// Content-Length that way would leave the upstream parsing the body as a new request.
constexpr std::array<std::string_view, 3> kProtectedFromConnection = {"content-length", "host",
                                                                      "transfer-encoding"};

// The extra names listed in Connection headers, which are hop-by-hop by declaration.
std::vector<std::string> connection_listed_names(const MessageHead& head) {
  std::vector<std::string> names;
  for (const Header& header : head.headers) {
    if (!iequals(header.name, "Connection")) {
      continue;
    }
    std::string_view rest = header.value;
    while (!rest.empty()) {
      const std::size_t comma = rest.find(',');
      std::string_view token = rest.substr(0, comma);
      rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
      const std::size_t first = token.find_first_not_of(" \t");
      if (first == std::string_view::npos) {
        continue;
      }
      token = token.substr(first, token.find_last_not_of(" \t") - first + 1);
      std::string name = normalize_name(token);
      if (!contains(kProtectedFromConnection, name)) {
        names.push_back(std::move(name));
      }
    }
  }
  return names;
}

bool is_listed(const std::vector<std::string>& names, std::string_view name) noexcept {
  return std::ranges::find(names, name) != names.end();
}

void append_header(std::string& out, const Header& header) {
  out.append(header.name).append(": ").append(header.value).append(kCrlf);
}

void append_response_status_line(std::string& out, const MessageHead& response) {
  out.append("HTTP/1.1 ").append(std::to_string(response.status)).append(" ");
  out.append(response.reason).append(kCrlf);
}

void append_response_headers(std::string& out, const MessageHead& response) {
  const auto listed = connection_listed_names(response);
  for (const Header& header : response.headers) {
    const std::string name = normalize_name(header.name);
    if (!contains(kResponseHopByHop, name) && !is_listed(listed, name)) {
      append_header(out, header);
    }
  }
}

// Host carries only the upstream's idea of an authority, so anything other than
// host:port characters is refused rather than passed to backends that may use it to
// build URLs or select virtual hosts.
bool is_valid_host(std::string_view host) noexcept {
  if (host.empty()) {
    return false;
  }
  return std::ranges::all_of(host, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
           c == '-' || c == '_' || c == ':' || c == '[' || c == ']';
  });
}

}  // namespace

int validate_request(const MessageHead& request, std::uint64_t max_body_bytes) {
  if (request.version_major != 1 || request.version_minor != 1) {
    return 505;
  }
  if (request.method == "CONNECT") {
    return 501;
  }
  // Only origin-form. Absolute-form could otherwise look like a request to proxy to
  // an arbitrary destination, and "*" is not needed yet.
  if (request.target.empty() || request.target.front() != '/') {
    return 400;
  }
  const Header* host = request.find("Host");
  if (host == nullptr || request.count("Host") != 1 || !is_valid_host(host->value)) {
    return 400;
  }
  if (request.find("Transfer-Encoding") != nullptr) {
    return 501;
  }
  if (request.content_length.value_or(0) > max_body_bytes) {
    return 413;
  }
  if (const Header* expect = request.find("Expect");
      expect != nullptr && !iequals(expect->value, "100-continue")) {
    return 417;
  }
  return 0;
}

bool expects_continue(const MessageHead& request) noexcept {
  return request.find("Expect") != nullptr && request.content_length.value_or(0) > 0;
}

std::string render_upstream_request(const MessageHead& request, std::string_view client_ip) {
  std::string out;
  out.reserve(1024);
  out.append(request.method).append(" ").append(request.target).append(" HTTP/1.1\r\n");

  const auto listed = connection_listed_names(request);
  for (const Header& header : request.headers) {
    const std::string name = normalize_name(header.name);
    // Expect is answered by the proxy itself, so it ends here.
    if (!contains(kRequestHopByHop, name) && !contains(kClientProvenance, name) &&
        name != "expect" && !is_listed(listed, name)) {
      append_header(out, header);
    }
  }

  out.append("Connection: close\r\n");
  out.append("X-Forwarded-For: ").append(client_ip).append(kCrlf);
  out.append("X-Forwarded-Proto: http\r\n");
  out.append(kVia).append(kCrlf);
  return out;
}

std::string render_client_response(const MessageHead& response) {
  std::string out;
  out.reserve(1024);
  append_response_status_line(out, response);
  append_response_headers(out, response);
  out.append("Connection: close\r\n");
  out.append(kVia).append(kCrlf);
  return out;
}

std::string render_interim_response(const MessageHead& response) {
  std::string out;
  append_response_status_line(out, response);
  append_response_headers(out, response);
  out.append(kVia).append(kCrlf);
  return out;
}

std::string render_error_response(int status, bool omit_body) {
  const std::string_view reason = status_reason(status);
  const std::string body = std::string(reason) + "\n";

  std::string out = "HTTP/1.1 " + std::to_string(status) + " ";
  out.append(reason).append(kCrlf);
  out.append("Content-Type: text/plain; charset=utf-8\r\n");
  out.append("Content-Length: ").append(std::to_string(body.size())).append(kCrlf);
  out.append("Connection: close\r\n");
  out.append(kVia).append(kCrlf);
  if (!omit_body) {
    out.append(body);
  }
  return out;
}

std::string_view status_reason(int status) noexcept {
  switch (status) {
    case 400:
      return "Bad Request";
    case 408:
      return "Request Timeout";
    case 413:
      return "Content Too Large";
    case 417:
      return "Expectation Failed";
    case 431:
      return "Request Header Fields Too Large";
    case 501:
      return "Not Implemented";
    case 502:
      return "Bad Gateway";
    case 504:
      return "Gateway Timeout";
    case 505:
      return "HTTP Version Not Supported";
    default:
      return "Error";
  }
}

std::string_view target_path(std::string_view target) noexcept {
  return target.substr(0, target.find('?'));
}

}  // namespace fluxgate::http
