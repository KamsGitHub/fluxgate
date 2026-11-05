#include "fluxgate/core/byte_buffer.hpp"

#include <cassert>
#include <cstring>

namespace fluxgate::core {

ByteBuffer::ByteBuffer(std::size_t capacity)
    : capacity_(capacity),
      // NOLINTNEXTLINE(modernize-avoid-c-arrays): uninitialised and runtime-sized.
      storage_(std::make_unique_for_overwrite<std::byte[]>(capacity)) {}

std::span<std::byte> ByteBuffer::writable() noexcept {
  // Compacting only when the tail is exhausted keeps memmove rare: in the common
  // case the buffer drains completely and consume() rewinds it for free.
  if (end_ == capacity_ && begin_ > 0) {
    std::memmove(storage_.get(), storage_.get() + begin_, size());
    end_ -= begin_;
    begin_ = 0;
  }
  return {storage_.get() + end_, capacity_ - end_};
}

void ByteBuffer::commit(std::size_t count) noexcept {
  assert(count <= capacity_ - end_);
  end_ += count;
}

std::span<const std::byte> ByteBuffer::readable() const noexcept {
  return {storage_.get() + begin_, size()};
}

void ByteBuffer::consume(std::size_t count) noexcept {
  assert(count <= size());
  begin_ += count;
  if (begin_ == end_) {
    begin_ = 0;
    end_ = 0;
  }
}

void ByteBuffer::truncate(std::size_t count) noexcept {
  assert(count <= size());
  end_ = begin_ + count;
  if (begin_ == end_) {
    begin_ = 0;
    end_ = 0;
  }
}

}  // namespace fluxgate::core
