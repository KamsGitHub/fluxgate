#include "fluxgate/net/file_descriptor.hpp"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <utility>

using fluxgate::net::FileDescriptor;

namespace {

bool is_open(int fd) {
  return ::fcntl(fd, F_GETFD) != -1;
}

int open_descriptor() {
  const int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
  EXPECT_GE(fd, 0);
  return fd;
}

}  // namespace

TEST(FileDescriptor, DefaultIsInvalid) {
  const FileDescriptor fd;
  EXPECT_FALSE(fd);
  EXPECT_EQ(fd.get(), -1);
}

TEST(FileDescriptor, ClosesOnDestruction) {
  const int raw = open_descriptor();
  {
    const FileDescriptor fd(raw);
  }
  EXPECT_FALSE(is_open(raw));
}

TEST(FileDescriptor, MoveConstructionTransfersOwnership) {
  const int raw = open_descriptor();
  FileDescriptor source(raw);
  {
    const FileDescriptor target(std::move(source));
    EXPECT_FALSE(source);  // NOLINT(bugprone-use-after-move): defined moved-from state.
    EXPECT_EQ(target.get(), raw);
    EXPECT_TRUE(is_open(raw));
  }
  EXPECT_FALSE(is_open(raw));
}

TEST(FileDescriptor, MoveAssignmentClosesPreviousDescriptor) {
  const int first = open_descriptor();
  const int second = open_descriptor();
  FileDescriptor target(first);
  FileDescriptor source(second);
  target = std::move(source);
  EXPECT_FALSE(is_open(first));
  EXPECT_TRUE(is_open(second));
  EXPECT_EQ(target.get(), second);
}

TEST(FileDescriptor, SelfMoveAssignmentKeepsDescriptor) {
  const int raw = open_descriptor();
  FileDescriptor fd(raw);
  auto& alias = fd;
  fd = std::move(alias);
  EXPECT_TRUE(is_open(raw));
}

TEST(FileDescriptor, ReleaseGivesUpOwnership) {
  const int raw = open_descriptor();
  {
    FileDescriptor fd(raw);
    EXPECT_EQ(fd.release(), raw);
    EXPECT_FALSE(fd);
  }
  EXPECT_TRUE(is_open(raw));
  ::close(raw);
}
