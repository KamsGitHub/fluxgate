#pragma once

namespace fluxgate::net {

// Sole owner of a POSIX file descriptor. Every descriptor FluxGate opens lives in
// one of these from the moment the syscall returns, so no error path can leak it
// and no descriptor can be closed twice.
class FileDescriptor {
 public:
  constexpr FileDescriptor() noexcept = default;
  explicit constexpr FileDescriptor(int fd) noexcept : fd_(fd) {}

  ~FileDescriptor() { reset(); }

  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;

  FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.release()) {}
  FileDescriptor& operator=(FileDescriptor&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  [[nodiscard]] constexpr int get() const noexcept { return fd_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return fd_ >= 0; }
  explicit constexpr operator bool() const noexcept { return valid(); }

  // Gives up ownership without closing; the caller becomes responsible.
  [[nodiscard]] constexpr int release() noexcept {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }

  // Closes the current descriptor, if any, then takes ownership of `fd`.
  void reset(int fd = -1) noexcept;

 private:
  int fd_ = -1;
};

}  // namespace fluxgate::net
