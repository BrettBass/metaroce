/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <linux/ipv6.h>
#include <linux/udp.h>
#include <libsoftmetaroce/Icrc.h>

/*
  this file is not directly a test of libsoftmetaroce; it is a
  confirmation that the method of computing and storing the mCRC is
  equivalent to the iCRC, except for the attributes of the iCRC that
  are removed (including the outer header and masking bits)
*/

/*
  the valid_roce_ack is copied from a packet capture; it includes an
  ack with trailing iCRC
*/
const unsigned char valid_roce_ack[] =
    "\002\000\000\000\000\001\002\000\000\000\000\002\206\335n \000\000\000\034\021? \001\015\270\000\000\000\000\000\000\000\000\000\000\000\001 \001\015\270\000\000\000\000\000\000\000\000\000\000\000\002\303\352\022\267\000\034\000\000\021@\377\377\000\000\001\230\000\000\000\000\000\000\000\001\022_\347\023";

/*
  this is a write-only (single packet write) packet, present for
  double-checking.
*/
const unsigned char valid_roce_writeonly[] =
    "\002\000\000\000\000\001\002\000\000\000\000\002\206\335n \000\000\000H\021? \001\015\270\000\000\000\000\000\000\000\000\000\000\000\001 \001\015\270\000\000\000\000\000\000\000\000\000\000\000\002\312:\022\267\000H\000\000\012@\377\377\000\000\001\224\200\000\000\000\000\000\177\347\300\024P \000\030\017\316\000\000\000 \000\000\240f\313\177\000\000\000\000\001\000\000v\030\000\001\000\000\000\001\000\000\000\001\000\000\000\000\000\000\000\336E\005\274";

/*
  from ibta spec, table 33 (masked byte stream) extended with
  the icrc (but not vcrc) from table 34.
*/
const unsigned char masked_byte_stream_for_icrc[] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // 7
    0x6f, 0xff, 0xff, 0xff, 0x00, 0x30, 0x00, 0xff, // 15
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x25, // 23
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x26, // 29
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x17, // 39
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x96, // 47
    0x0a, 0x20, 0x24, 0x87, 0xff, 0x87, 0xb1, 0xb3, // 55
    0x00, 0x0d, 0xec, 0x2a, 0x01, 0x71, 0x0a, 0x1c, // 63
    0x01, 0x5d, 0x40, 0x02, 0x38, 0xf2, 0x7a, 0x05, // 71
    0x00, 0x00, 0x00, 0x0e, 0xbb, 0x88, 0x4d, 0x85, // 79
    0xfd, 0x5c, 0xfb, 0xa4, 0x72, 0x8b, 0xc0, 0x69, // 87
    0x0e, 0xd4, 0x00, 0x00, // 91
    0xB6, // 92 - icrc
    0x7d, // 93 - icrc
    0x1e, // 94 - icrc
    0xd1, // 96 - icrc
};

struct RocePacketTestData {
  const char* name;
  const unsigned char* data;
  size_t size;
};

class RoceV2iCRCTest : public ::testing::TestWithParam<RocePacketTestData> {
 protected:
  void SetUp() override {}
  void TearDown() override {}
};

const struct RocePacketTestData kRocePacketTestCases[] = {
    {"Ack", valid_roce_ack, sizeof(valid_roce_ack) - 1},
    {"WriteOnly", valid_roce_writeonly, sizeof(valid_roce_writeonly) - 1},
};

using namespace testing;
namespace libsoftmetaroce {

TEST_P(RoceV2iCRCTest, ValidateRoce) {
  const auto& testData = GetParam();

  // in order to perform the iCRC, we must overwite some bits.
  // copy the valid_roce_ack to a mutable array of bytes; no need
  // for trailing null-termination.
  unsigned char* dup = (unsigned char*)malloc(testData.size);
  memcpy(dup, testData.data, testData.size);

  // find the ipv6 header, we do not check the ether.
  const unsigned int ether_len = 14;
  const size_t ipv6_len = testData.size - ether_len;
  unsigned char* ipv6start = dup + ether_len;
  ASSERT_EQ(*ipv6start, 0x6e) << "failed to find ipv6 header in test fixture";

  // overwite not-invariant bits with 1: hop_limit, priority and flow label
  // (ecn).
  struct ipv6hdr* ip6 = (struct ipv6hdr*)ipv6start;
  EXPECT_EQ(ipv6_len, htons(ip6->payload_len) + sizeof(struct ipv6hdr))
      << "length disagremeent with ipv6";
  ip6->hop_limit = 0xff;
  memset(&ip6->flow_lbl, 0xff, 3);
  ip6->priority = 0xf;

  // find the udp header and overwrite the checksum.
  struct udphdr* udp = (struct udphdr*)(ip6 + 1);
  EXPECT_EQ(udp->dest, htons(4791))
      << "did not find expected roce udp header dport";
  EXPECT_EQ(ipv6_len, htons(udp->len) + sizeof(struct ipv6hdr))
      << "length disagreement with UDP";
  udp->check = 0xffff;

  // reserved high byte of 32-bit field used for 24-bit qpn gets 1'd.
  char* rocebth = (char*)(udp + 1);
  rocebth[4] = 0xff;

  // the crc32 output of a bunch of 1's in an LRH is the following.
  uint32_t initial = 0xdebb20e3;

  // unfortunately, this does not eval to zero, but does evaluate to initial.
  // this does not need to be a test, it just surprises.
  uint32_t icrcz = folly::crc32(ipv6start, ipv6_len, initial);
  EXPECT_EQ(icrcz, initial) << std::hex << " complete icrc-ing-> " << icrcz;

  // compute with the invert at the end.
  uint32_t icrc = ~folly::crc32(ipv6start, ipv6_len - 4, initial);
  // find the value stored in the reference packet
  uint32_t expected = *(uint32_t*)(testData.data + testData.size - 4);
  EXPECT_EQ(icrc, expected) << std::hex << icrc << "!=" << expected;

  // a bit of debugging help for the packet where we have bashed the
  // invariants.
  for (unsigned char* p = ipv6start; p < ipv6start + ipv6_len; p++) {
    printf("%02x%s", *p, (p - ipv6start) % 2 ? " " : "");
  }
  printf("\n");

  // compute through the end to check match with check value
  icrc = ~folly::crc32(ipv6start, ipv6_len, initial);
  uint32_t check = 0x2144df1c;
  EXPECT_EQ(icrc, check) << "computed crc over the frame+crc " << std::hex
                         << icrc << " != " << check
                         << ", which is the expected check value";

  // compute using libsoftmetaroce, with given initial to cover the LRH.
  auto iobuf = folly::IOBuf::copyBuffer(ipv6start, ipv6_len);
  folly::io::Cursor cursor(iobuf.get());
  uint32_t computedICRC = Icrc::computeICRC(cursor, initial);
  EXPECT_EQ(computedICRC, check)
      << "lsmr computed crc over the frame+crc " << std::hex << icrc
      << " != " << check << ", which is the expected check value";

  free(dup);
}

INSTANTIATE_TEST_SUITE_P(
    RoceV2iCRCTestSuite,
    RoceV2iCRCTest,
    ::testing::ValuesIn(kRocePacketTestCases),
    [](const ::testing::TestParamInfo<RocePacketTestData>& info) {
      return info.param.name;
    });

TEST(RoceV2iCRC, IbtaReference) {
  // The reference stream includes the 0xff's that the tests for the
  // packet trace frames had to mask out. The reference packet can
  // be validated starting with -1 initial crc value.
  uint32_t check = 0x2144df1c;
  auto iobuf = folly::IOBuf::copyBuffer(
      masked_byte_stream_for_icrc, sizeof(masked_byte_stream_for_icrc));
  folly::io::Cursor cursor(iobuf.get());
  uint32_t computedICRC = Icrc::computeICRC(cursor);
  EXPECT_EQ(computedICRC, check)
      << "lsmr computed crc over the frame+crc " << std::hex << computedICRC
      << " != " << check << ", which is the expected check value";

  // the validation function from metaroce can be called here, since we
  // do not need to start from the different initial value that represents
  // the partial CRC over the LRH of 0xff's.
  uint32_t received, computed;
  EXPECT_TRUE(Icrc::extractAndValidateICRC(iobuf, received, computed))
      << "mismatch between received " << std::hex << received
      << " and computed " << computed;
}

} // namespace libsoftmetaroce
