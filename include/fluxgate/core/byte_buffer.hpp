#pragma once

#include <cstddef>
#include <memory>
#include <span>

namespace fluxgate::core {

// A fixed-capacity FIFO of bytes, allocated once. The capacity never grows: this
// type is what makes per-connection memory bounded, and a full buffer is the
// signal that stops the proxy reading from the faster peer (backpressure).
class ByteBuffer {
 public:
  explicit ByteBuffer(std::size_t capacity);

  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] std::size_t size() const noexcept { return end_ - begin_; }
  [[nodiscard]] bool empty() const noexcept { return begin_ == end_; }
  [[nodiscard]] bool full() const noexcept { return size() == capacity_; }

  // Contiguous free space to read into, followed by commit(n). May move the
  // stored bytes, so it invalidates any earlier span from readable().
  [[nodiscard]] std::span<std::byte> writable() noexcept;
  void commit(std::size_t count) noexcept;

  [[nodiscard]] std::span<const std::byte> readable() const noexcept;

  // Drops `count` bytes from the front.
  void consume(std::size_t count) noexcept;

  // Keeps only the first `count` bytes.
  void truncate(std::size_t count) noexcept;

 private:
  std::size_t capacity_;
  std::unique_ptr<std::byte[]> storage_;  // NOLINT(modernize-avoid-c-arrays): runtime size.
  std::size_t begin_ = 0;
  std::size_t end_ = 0;
};

}  // namespace fluxgate::core
