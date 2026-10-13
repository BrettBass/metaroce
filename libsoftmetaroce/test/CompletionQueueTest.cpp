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

#include <folly/coro/BlockingWait.h>
#include <libsoftmetaroce/CompletionQueue.h>
#include <libsoftmetaroce/Constants.h>
#include <atomic>
#include <thread>
#include <tuple>
#include <vector>

namespace libsoftmetaroce {
namespace {

Cqe completionFor(uint64_t workReqId) {
  return Cqe{
      .workReqId = workReqId,
      .status = kSuccess,
      .maybeImmData = std::nullopt,
      .sizeBytes = 0,
      .qpn = 1,
      .tag = 0,
      .opcode = IBV_WC_SEND};
}

TEST(CompletionQueueTest, AQueueTakesCompletionsUpToItsDepth) {
  constexpr uint32_t kDepth = 2;
  CompletionQueue cq{kDepth};

  for (uint32_t taken = 0; taken < kDepth; ++taken) {
    ASSERT_TRUE(cq.tryEnqueue(completionFor(taken)))
        << "precondition: the queue takes what its depth allows; it refused at "
        << taken;
  }

  EXPECT_FALSE(cq.tryEnqueue(completionFor(kDepth)))
      << "a queue holding its depth takes no more, and says so rather than "
         "growing past what ibv_create_cq sized it for";
  EXPECT_EQ(cq.getNumCompletedWqes(), static_cast<int>(kDepth))
      << "the refused completion is dropped, not held: a client polls the "
         "depth and no more";
}

TEST(CompletionQueueTest, ARefusedCompletionIsReportedOnceEach) {
  constexpr uint32_t kDepth = 2;
  int reported = 0;
  CompletionQueue cq{kDepth, [&reported] { ++reported; }};

  for (uint32_t taken = 0; taken < kDepth; ++taken) {
    ASSERT_TRUE(cq.tryEnqueue(completionFor(taken)))
        << "precondition: the queue takes what its depth allows; it refused at "
        << taken;
  }
  EXPECT_EQ(reported, 0)
      << "a queue with room reports no overflow, so a client is told only of "
         "completions it will not see";

  EXPECT_FALSE(cq.tryEnqueue(completionFor(kDepth)));
  EXPECT_FALSE(cq.tryEnqueue(completionFor(kDepth + 1)));
  EXPECT_EQ(reported, 2)
      << "each completion the depth refuses is reported, so a client learns of "
         "every one it will not see rather than the first alone";
}

TEST(CompletionQueueTest, PollingAQueueMakesRoomAgain) {
  constexpr uint32_t kDepth = 2;
  CompletionQueue cq{kDepth};
  ASSERT_TRUE(cq.tryEnqueue(completionFor(0)))
      << "precondition: the queue takes the completion the poll below takes";
  ASSERT_TRUE(cq.tryEnqueue(completionFor(1)))
      << "precondition: the queue takes the second, which fills it";
  ASSERT_FALSE(cq.tryEnqueue(completionFor(2)))
      << "precondition: the queue is full";

  const Cqe polled = folly::coro::blockingWait(cq.pollCq());

  EXPECT_EQ(polled.workReqId, 0u)
      << "a completion queue answers in the order it took them, so the caller "
         "sees the oldest first";
  EXPECT_TRUE(cq.tryEnqueue(completionFor(3)))
      << "the entry a poll frees is one the next completion can have";
}

TEST(CompletionQueueTest, AQueueMadeWithoutADepthTakesTheDeviceMaximum) {
  CompletionQueue cq;

  for (uint32_t taken = 0; taken < kMaxCqEntries; ++taken) {
    ASSERT_TRUE(cq.tryEnqueue(completionFor(taken)))
        << "precondition: an unsized queue takes what the device advertises; "
           "it refused at "
        << taken << " of " << kMaxCqEntries;
  }

  EXPECT_FALSE(cq.tryEnqueue(completionFor(kMaxCqEntries)))
      << "a queue built without a depth is sized by what ibv_query_device "
         "reports as max_cqe, so it stops there and not at some other number";
}

TEST(CompletionQueueTest, ProducersShareTheLastEntriesWithoutOverfilling) {
  // Queue pairs sharing one completion queue, reporting at the same time.
  constexpr uint32_t kDepth = 64;
  constexpr int kProducers = 4;
  CompletionQueue cq{kDepth};
  std::atomic<uint32_t> taken{0};

  std::vector<std::thread> producers;
  producers.reserve(kProducers);
  for (int producer = 0; producer < kProducers; ++producer) {
    producers.emplace_back([&] {
      for (uint32_t attempt = 0; attempt < kDepth; ++attempt) {
        if (cq.tryEnqueue(completionFor(attempt))) {
          taken.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto& producer : producers) {
    producer.join();
  }

  EXPECT_EQ(taken.load(), kDepth)
      << "the completions taken total the depth exactly: fewer means one was "
         "refused an entry the queue had, and more means the queue grew past "
         "what ibv_create_cq sized it for";
  EXPECT_EQ(cq.getNumCompletedWqes(), static_cast<int>(kDepth))
      << "and the queue holds exactly those, so a client polls the depth";
}

TEST(CompletionQueueTest, AnEntryTakenPastTheDepthStillOccupiesTheQueue) {
  // One queue serves both directions, and this depth does not govern a receive.
  constexpr uint32_t kDepth = 2;
  CompletionQueue cq{kDepth};
  cq.enqueue(completionFor(0));
  cq.enqueue(completionFor(1));

  EXPECT_FALSE(cq.tryEnqueue(completionFor(2)))
      << "the depth counts every completion in the queue, not only the ones "
         "offered through tryEnqueue; counting only those would let a send take "
         "an entry the queue does not have";

  std::ignore = folly::coro::blockingWait(cq.pollCq());

  EXPECT_TRUE(cq.tryEnqueue(completionFor(3)))
      << "and polling one frees an entry whichever path put it there";
}

} // namespace
} // namespace libsoftmetaroce
