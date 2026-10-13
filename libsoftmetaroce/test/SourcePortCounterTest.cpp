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

#include <folly/io/async/AsyncSocketException.h>
#include <folly/io/async/EventBase.h>
#include <gtest/gtest.h>
#include <libsoftmetaroce/PacketSocketFactory.h>
#include <libsoftmetaroce/SourcePortCounter.h>
#include <libsoftmetaroce/test/SourcePortRecorder.h>
#include <vector>

namespace libsoftmetaroce {
namespace {

const auto kLocalAddr = folly::IPAddress("fc00::1");

class SourcePortCounterTest : public ::testing::Test {
 protected:
  void bindSockets(SourcePortCounter& counter, int count) {
    for (int made = 0; made < count; ++made) {
      auto socket = makePacketModifierSocket(
          &evb_, ibv_gid_entry{}, ports_.socketMaker());
      counter.bindNext(*socket, kLocalAddr);
    }
  }

  folly::EventBase evb_;
  SourcePortRecorder ports_;
};

TEST_F(SourcePortCounterTest, PortsCountUpFromWhereTheCounterStarts) {
  SourcePortCounter counter{50000};
  bindSockets(counter, 3);

  EXPECT_EQ(ports_.bound, (std::vector<uint16_t>{50000, 50001, 50002}))
      << "each socket takes the counter's next port, so successive sockets "
         "take successive ports";
}

TEST_F(SourcePortCounterTest, APortAlreadyInUseIsSkipped) {
  ports_.inUse = [](uint16_t port) { return port == 50000; };
  SourcePortCounter counter{50000};
  bindSockets(counter, 2);

  EXPECT_EQ(ports_.bound, (std::vector<uint16_t>{50001, 50002}))
      << "another process holding a port must not fail the socket; the "
         "counter moves past it";
}

TEST_F(SourcePortCounterTest, PortsWrapToTheStartOfTheRange) {
  SourcePortCounter counter{65535};
  bindSockets(counter, 2);

  EXPECT_EQ(ports_.bound, (std::vector<uint16_t>{65535, 49152}))
      << "the counter wraps from the top of the range to its start, never to "
         "a port below 49152";
}

TEST_F(SourcePortCounterTest, EveryPortInUseFailsTheBind) {
  ports_.inUse = [](uint16_t) { return true; };
  SourcePortCounter counter{50000};

  EXPECT_THROW(bindSockets(counter, 1), folly::AsyncSocketException)
      << "once every port in the range has been tried, the bind fails rather "
         "than reaching outside it";
}

TEST_F(SourcePortCounterTest, ARandomStartLiesInTheRange) {
  SourcePortCounter counter;
  bindSockets(counter, 1);

  ASSERT_EQ(ports_.bound.size(), 1u) << "precondition: the one socket is bound";
  EXPECT_GE(ports_.bound[0], SourcePortCounter::kFirstPort)
      << "the random start picks only where the range already wraps, so the "
         "first port is in 49152-65535 like every later one";
}

} // namespace
} // namespace libsoftmetaroce
