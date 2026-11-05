#include "fluxgate/net/file_descriptor.hpp"

#include <unistd.h>

namespace fluxgate::net {

void FileDescriptor::reset(int fd) noexcept {
  if (fd_ >= 0) {
    // close() is deliberately not retried on EINTR: on Linux the descriptor is
    // already released at that point, and the number may have been reused by
    // another thread, so a retry could close an unrelated resource.
    ::close(fd_);
  }
  fd_ = fd;
}

}  // namespace fluxgate::net
