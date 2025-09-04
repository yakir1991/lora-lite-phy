#include <gtest/gtest.h>
#include "lora/rx/header.hpp"

using namespace lora;
using namespace lora::rx;
using namespace lora::utils;

TEST(HeaderParse, RoundTrip) {
  LocalHeader in{ .payload_len = 0x1A, .cr = CodeRate::CR47, .has_crc = true };
  auto bytes = make_local_header_with_crc(in);
  auto out = parse_local_header_with_crc(bytes.data(), bytes.size());
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->payload_len, in.payload_len);
  EXPECT_EQ(static_cast<int>(out->cr), static_cast<int>(in.cr));
  EXPECT_EQ(out->has_crc, in.has_crc);
}

TEST(HeaderParse, DetectsBitFlip) {
  LocalHeader in{ .payload_len = 0x10, .cr = CodeRate::CR45, .has_crc = true };
  auto bytes = make_local_header_with_crc(in);
  bytes[0] ^= 0x01;
  auto out = parse_local_header_with_crc(bytes.data(), bytes.size());
  EXPECT_FALSE(out.has_value());
}

