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

#include <gtest/gtest.h>
#include <libsoftmetaroce/congestion_control/StaticQwnd.h>

using namespace testing;
namespace libsoftmetaroce {

class StaticQwndTest : public ::testing::Test {};

/*
 * Simple test of static fwnd.  Create flow, send packets, ack packets, confirm
 * awnd is correct along the way.
 */
TEST_F(StaticQwndTest, Basic) {
  int expectedWindow;
  const int fwndLimit = 5;
  StaticQwnd cc(fwndLimit);

  const uint16_t flowId = 0;
  cc.onFlowAdded(flowId);

  EXPECT_EQ(cc.getAvailableWindowSize(flowId), fwndLimit);
  for (expectedWindow = fwndLimit - 1; expectedWindow >= 0; expectedWindow--) {
    cc.onPacketSent(flowId);
    EXPECT_EQ(cc.getAvailableWindowSize(flowId), expectedWindow);
  }
  for (expectedWindow = 1; expectedWindow <= fwndLimit; expectedWindow++) {
    cc.onPacketAck(
        flowId, CongestionController::AckEvent{.numAckedPackets = 1});
    EXPECT_EQ(cc.getAvailableWindowSize(flowId), expectedWindow);
  }

  // it would be invalid to get more acks than there are in flight packets,
  // but let's confirm that the window doesn't exceed the max.
  cc.onPacketAck(flowId, CongestionController::AckEvent{.numAckedPackets = 1});
  EXPECT_EQ(cc.getAvailableWindowSize(flowId), fwndLimit);
}

} // namespace libsoftmetaroce
