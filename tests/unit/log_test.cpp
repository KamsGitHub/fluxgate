#include "fluxgate/log/log.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

using namespace fluxgate::log;

TEST(Log, RendersTypedFields) {
  const std::string name = "value";
  EXPECT_EQ(render_fields("evt", {{"s", name}, {"n", 42}, {"neg", std::int64_t{-1}}, {"b", true}}),
            R"("event":"evt","s":"value","n":42,"neg":-1,"b":true)");
}

TEST(Log, EscapesJsonMetacharacters) {
  std::string out;
  append_json_escaped(out, R"(a"b\c)");
  EXPECT_EQ(out, R"(a\"b\\c)");
}

TEST(Log, NewlinesCannotForgeLogLines) {
  std::string out;
  append_json_escaped(out, "x\r\n{\"event\":\"forged\"}");
  EXPECT_EQ(out.find('\n'), std::string::npos);
  EXPECT_EQ(out.find('\r'), std::string::npos);
}

TEST(Log, EscapesControlAndNonAsciiBytes) {
  std::string out;
  append_json_escaped(out, std::string("\x00\x1b\x7f\xff", 4));
  EXPECT_EQ(out, "\\u0000\\u001b\\u007f\\u00ff");
}
