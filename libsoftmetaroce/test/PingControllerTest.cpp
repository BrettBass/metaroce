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

#include <folly/coro/BlockingWait.h>
#include <folly/coro/Task.h>
#include <folly/executors/GlobalExecutor.h>
#include <folly/io/Cursor.h>
#include <folly/io/async/AsyncUDPSocket.h>
#include <folly/portability/GMock.h>
#include <gtest/gtest.h>
#include <libsoftmetaroce/PingController.h>
#include <libsoftmetaroce/codec/BaseHeader.h>
#include <libsoftmetaroce/codec/ControlHeader.h>
#include <libsoftmetaroce/test/MockUDPSocket.h>
#include <libsoftmetaroce/test/SourcePortRecorder.h>
#include <chrono>
#include <thread>

using namespace testing;
namespace libsoftmetaroce {

namespace {
constexpr uint16_t kTestPort = 0xace;
const auto kLocalAddr = folly::IPAddress("fc00::1");
const auto kRemoteAddr = folly::IPAddress("fc00::2");
constexpr auto kPingTimeout = std::chrono::milliseconds{5000};
constexpr auto kPendingPingDeadline = std::chrono::seconds{10};
} // namespace

class PingControllerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto socketMaker = [this](folly::EventBase* evb) {
      auto sock = std::make_unique<NiceMock<MockUDPSocket>>(evb);
      mockSocket_ = sock.get();
      return std::make_pair(
          std::unique_ptr<folly::AsyncUDPSocket>(std::move(sock)), nullptr);
    };
    controller_ =
        std::make_unique<PingController>(std::move(socketMaker), kTestPort);
  }

  folly::SemiFuture<PingResult> startPing(
      folly::IPAddress remoteAddr = kRemoteAddr,
      uint32_t remoteQpNum = 100,
      std::chrono::milliseconds timeout = kPingTimeout) {
    return folly::coro::co_withExecutor(
               folly::getGlobalCPUExecutor(),
               controller_->co_sendPing(
                   kLocalAddr,
                   std::move(remoteAddr),
                   ibv_gid_entry{},
                   /*sourceQpNum=*/1,
                   remoteQpNum,
                   /*tagNum=*/0,
                   /*flowId=*/0,
                   /*supportedFlowCount=*/8,
                   timeout))
        .start();
  }

  [[nodiscard]] bool waitForPendingPings(size_t expected) {
    const auto deadline =
        std::chrono::steady_clock::now() + kPendingPingDeadline;
    while (controller_->getNumPendingPings() < expected &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    return controller_->getNumPendingPings() >= expected;
  }

  NiceMock<MockUDPSocket>* mockSocket_ = nullptr;
  std::unique_ptr<PingController> controller_;
};

TEST_F(PingControllerTest, PingTimesOut) {
  auto result = folly::coro::blockingWait(controller_->co_sendPing(
      kLocalAddr,
      kRemoteAddr,
      ibv_gid_entry{},
      /*sourceQpNum=*/1,
      /*remoteQpNum=*/100,
      /*tagNum=*/0,
      /*flowId=*/0,
      /*supportedFlowCount=*/8,
      std::chrono::milliseconds{100}));

  EXPECT_EQ(result.outcome, PingOutcome::TIMED_OUT)
      << "unanswered ping should time out";
  EXPECT_EQ(controller_->getNumPendingPings(), 0)
      << "timed-out ping should be removed from pending set";
}

TEST_F(PingControllerTest, SendPingAddsPendingEntry) {
  auto sf = startPing();

  ASSERT_TRUE(waitForPendingPings(1))
      << "precondition: the 1 started ping registers within "
      << kPendingPingDeadline.count() << "s";
  EXPECT_EQ(controller_->getNumPendingPings(), 1)
      << "a single pending ping should be registered";

  controller_->cancelAllPendingPings();
  auto result = std::move(sf).get();
  EXPECT_EQ(result.outcome, PingOutcome::CANCELLED)
      << "cancelled ping should report CANCELLED outcome";
}

TEST_F(PingControllerTest, PingIsUnreliableControlWithNoMeth) {
  // The socket, and so the mock, exists only from the first send onward.
  auto inspected = std::make_shared<std::atomic<bool>>(false);
  auto socketMaker = [inspected](folly::EventBase* evb) {
    auto sock = std::make_unique<NiceMock<MockUDPSocket>>(evb);
    EXPECT_CALL(*sock, write(_, _))
        .WillRepeatedly([inspected](
                            const folly::SocketAddress&,
                            const std::unique_ptr<folly::IOBuf>& buf) {
          folly::io::Cursor cursor(buf.get());
          BaseHeader baseHeader(cursor);
          EXPECT_EQ(
              baseHeader.getOpcode(),
              BaseHeader::Opcode(
                  BaseHeader::Opcode::OperationType::UNRELIABLE_CONTROL))
              << "a ping is never retransmitted and holds no place in the "
                 "flow sequence space, so it must not claim the reliable "
                 "control opcode, which Spec table 8.1 pairs with a METH this "
                 "packet does not carry";

          ControlHeader controlHeader(cursor);
          EXPECT_EQ(controlHeader.getType(), ControlPacketType::PING)
              << "the control header must sit directly behind the base "
                 "header; finding it at that offset is what shows no METH "
                 "was written between the two";

          inspected->store(true);
          return buf->computeChainDataLength();
        });
    return std::make_pair(
        std::unique_ptr<folly::AsyncUDPSocket>(std::move(sock)), nullptr);
  };
  controller_ =
      std::make_unique<PingController>(std::move(socketMaker), kTestPort);

  auto sf = startPing();
  ASSERT_TRUE(waitForPendingPings(1))
      << "precondition: the 1 started ping registers within "
      << kPendingPingDeadline.count() << "s";
  controller_->cancelAllPendingPings();
  EXPECT_EQ(std::move(sf).get().outcome, PingOutcome::CANCELLED)
      << "the send path must reach the socket and then unwind, or the header "
         "assertions describe a packet the controller never completed";
  EXPECT_TRUE(inspected->load())
      << "the controller wrote no ping, so the header assertions never ran";
}

TEST_F(PingControllerTest, APingSocketBindsToAPortInTheSpecRange) {
  SourcePortRecorder ports;
  controller_ =
      std::make_unique<PingController>(ports.socketMaker(), kTestPort);

  auto sf = startPing();
  ASSERT_TRUE(waitForPendingPings(1))
      << "precondition: the 1 started ping registers within "
      << kPendingPingDeadline.count() << "s";
  controller_->cancelAllPendingPings();
  std::move(sf).get();

  ASSERT_EQ(ports.bound.size(), 1u)
      << "precondition: the first ping makes the one send socket";
  EXPECT_GE(ports.bound[0], SourcePortCounter::kFirstPort)
      << "a ping is sent like any other MetaRoCE packet, so its source port "
         "comes from the same 49152-65535 range";
}

TEST_F(PingControllerTest, CancelAllPendingPings) {
  auto sf1 = startPing();
  auto sf2 = startPing(folly::IPAddress("fc00::3"), /*remoteQpNum=*/200);

  ASSERT_TRUE(waitForPendingPings(2))
      << "precondition: the 2 started pings register within "
      << kPendingPingDeadline.count() << "s";
  EXPECT_EQ(controller_->getNumPendingPings(), 2)
      << "both pings should be pending";

  controller_->cancelAllPendingPings();

  auto result1 = std::move(sf1).get();
  auto result2 = std::move(sf2).get();

  EXPECT_EQ(result1.outcome, PingOutcome::CANCELLED);
  EXPECT_EQ(result2.outcome, PingOutcome::CANCELLED);
  EXPECT_EQ(controller_->getNumPendingPings(), 0)
      << "no pings should remain after cancel";
}

TEST_F(PingControllerTest, DestructorCancelsPendingPings) {
  auto sf = startPing();

  ASSERT_TRUE(waitForPendingPings(1))
      << "precondition: the 1 started ping registers within "
      << kPendingPingDeadline.count() << "s";
  EXPECT_EQ(controller_->getNumPendingPings(), 1)
      << "ping should be pending before controller destruction";

  controller_.reset();

  auto result = std::move(sf).get();
  EXPECT_EQ(result.outcome, PingOutcome::CANCELLED)
      << "destroying the controller should cancel pending pings";
}

TEST_F(PingControllerTest, ConcurrentPingsHandledCorrectly) {
  constexpr int kNumPings = 100;
  constexpr int kNumResponded = 50;
  std::vector<folly::SemiFuture<PingResult>> futures;
  futures.reserve(kNumPings);

  for (int i = 0; i < kNumPings; i++) {
    futures.push_back(startPing(
        kRemoteAddr, /*remoteQpNum=*/i, std::chrono::milliseconds{200}));
  }

  ASSERT_TRUE(waitForPendingPings(kNumPings))
      << "precondition: the " << kNumPings << " started pings register within "
      << kPendingPingDeadline.count() << "s";
  EXPECT_EQ(controller_->getNumPendingPings(), kNumPings)
      << "all pings should be pending before responding";

  for (uint16_t i = 0; i < kNumResponded; i++) {
    controller_->handlePingResponse(i, PingErrorState::NO_ERROR);
  }

  int completedCount = 0;
  int timedOutCount = 0;
  for (auto& f : futures) {
    auto result = std::move(f).get();
    if (result.outcome == PingOutcome::COMPLETED) {
      completedCount++;
      EXPECT_EQ(result.errorState, PingErrorState::NO_ERROR);
      EXPECT_GT(result.rtt.count(), 0)
          << "RTT should be positive for completed pings";
    } else {
      EXPECT_EQ(result.outcome, PingOutcome::TIMED_OUT);
      timedOutCount++;
    }
  }

  EXPECT_EQ(completedCount, kNumResponded) << "responded pings should complete";
  EXPECT_EQ(timedOutCount, kNumPings - kNumResponded)
      << "unresponded pings should time out";
  EXPECT_EQ(controller_->getNumPendingPings(), 0)
      << "no pings should remain after all resolve";
}

} // namespace libsoftmetaroce
