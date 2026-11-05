#include "fluxgate/proxy/outbound_channel.hpp"

#include <cassert>

namespace fluxgate::proxy {

void OutboundChannel::drop_head_bytes(std::size_t count) noexcept {
  // Heads only occur before any body bytes have been validated.
  assert(validated_ == 0);
  buffer_.consume(count);
}

void OutboundChannel::drop_validated() noexcept {
  buffer_.consume(validated_);
  validated_ = 0;
}

OutboundChannel::FlushResult OutboundChannel::flush(net::Stream& stream) {
  FlushResult result;

  while (head_sent_ < head_.size()) {
    const auto bytes = std::as_bytes(std::span(head_)).subspan(head_sent_);
    const net::IoResult io = stream.write(bytes);
    if (io.status == net::IoStatus::Error) {
      result.failed = true;
      result.error = io.error;
      return result;
    }
    if (io.status != net::IoStatus::Ok || io.bytes == 0) {
      return result;
    }
    head_sent_ += io.bytes;
    result.bytes += io.bytes;
  }
  // Interim and final heads can be queued at different times, so reset fully.
  head_.clear();
  head_sent_ = 0;

  while (validated_ > 0) {
    const net::IoResult io = stream.write(buffer_.readable().first(validated_));
    if (io.status == net::IoStatus::Error) {
      result.failed = true;
      result.error = io.error;
      return result;
    }
    if (io.status != net::IoStatus::Ok || io.bytes == 0) {
      return result;
    }
    buffer_.consume(io.bytes);
    validated_ -= io.bytes;
    result.bytes += io.bytes;
  }
  return result;
}

}  // namespace fluxgate::proxy
