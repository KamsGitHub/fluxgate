#include "fluxgate/core/byte_buffer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>

using fluxgate::core::ByteBuffer;

namespace {

void put(ByteBuffer& buffer, std::string_view text) {
  auto space = buffer.writable();
  ASSERT_GE(space.size(), text.size());
  std::memcpy(space.data(), text.data(), text.size());
  buffer.commit(text.size());
}

std::string contents(const ByteBuffer& buffer) {
  const auto bytes = buffer.readable();
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

}  // namespace

TEST(ByteBuffer, StartsEmptyWithFullCapacityWritable) {
  ByteBuffer buffer(8);
  EXPECT_TRUE(buffer.empty());
  EXPECT_FALSE(buffer.full());
  EXPECT_EQ(buffer.writable().size(), 8U);
}

TEST(ByteBuffer, IsFifo) {
  ByteBuffer buffer(8);
  put(buffer, "abcd");
  buffer.consume(2);
  EXPECT_EQ(contents(buffer), "cd");
}

TEST(ByteBuffer, RewindsWhenDrainedSoNoCompactionIsNeeded) {
  ByteBuffer buffer(4);
  put(buffer, "abcd");
  EXPECT_TRUE(buffer.full());
  buffer.consume(4);
  EXPECT_EQ(buffer.writable().size(), 4U);
}

TEST(ByteBuffer, CompactsOnlyWhenTailIsExhausted) {
  ByteBuffer buffer(4);
  put(buffer, "abcd");
  buffer.consume(3);
  EXPECT_EQ(buffer.writable().size(), 3U);
  EXPECT_EQ(contents(buffer), "d");
  put(buffer, "xyz");
  EXPECT_EQ(contents(buffer), "dxyz");
  EXPECT_TRUE(buffer.full());
}

TEST(ByteBuffer, FullBufferOffersNoSpace) {
  ByteBuffer buffer(2);
  put(buffer, "ab");
  EXPECT_TRUE(buffer.writable().empty());
}

TEST(ByteBuffer, TruncateKeepsPrefix) {
  ByteBuffer buffer(8);
  put(buffer, "abcdef");
  buffer.consume(1);
  buffer.truncate(2);
  EXPECT_EQ(contents(buffer), "bc");
  buffer.truncate(0);
  EXPECT_TRUE(buffer.empty());
  EXPECT_EQ(buffer.writable().size(), 8U);
}
