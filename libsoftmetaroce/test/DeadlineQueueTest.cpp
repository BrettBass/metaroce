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

#include <folly/synchronization/Baton.h>
#include <gtest/gtest.h>
#include <libsoftmetaroce/DeadlineQueue.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace libsoftmetaroce {

namespace {

constexpr auto kWaitForDispatch = std::chrono::seconds(30);

uint64_t nsFrom(std::chrono::microseconds delay) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(delay).count());
}

} // namespace

TEST(DeadlineQueueTest, RunsWorkNoEarlierThanItsDeadline) {
  DeadlineQueue queue;
  const uint64_t deadlineNs =
      getNowNs64() + nsFrom(std::chrono::microseconds(1'000));
  std::atomic<uint64_t> ranAtNs{0};
  folly::Baton<> ran;
  queue.scheduleAt(deadlineNs, [&] {
    ranAtNs = getNowNs64();
    ran.post();
  });

  ASSERT_TRUE(ran.try_wait_for(kWaitForDispatch))
      << "work whose deadline has passed must run, or the queue drops rather "
         "than delays";
  EXPECT_GE(ranAtNs.load(), deadlineNs)
      << "the deadline is the earliest release time a caller can rely on; "
         "releasing before it means a pacing interval was not honored";
}

TEST(DeadlineQueueTest, DispatchesInDeadlineOrderWhateverTheInsertionOrder) {
  DeadlineQueue queue;
  // Far enough out that every insert lands before the first dispatch, so the
  // observed order is the queue's ordering and not a race with the clock.
  const uint64_t baseNs =
      getNowNs64() + nsFrom(std::chrono::microseconds(2'000));
  std::vector<int> releaseOrder;
  std::mutex releaseOrderMutex;
  folly::Baton<> allRan;

  for (int label : {3, 1, 4, 2}) {
    queue.scheduleAt(
        baseNs + static_cast<uint64_t>(label) * 500'000, [&, label] {
          std::lock_guard<std::mutex> guard(releaseOrderMutex);
          releaseOrder.push_back(label);
          if (releaseOrder.size() == 4) {
            allRan.post();
          }
        });
  }

  ASSERT_TRUE(allRan.try_wait_for(kWaitForDispatch))
      << "every entry must run, or the out-of-order insert lost one";
  EXPECT_EQ(releaseOrder, (std::vector<int>{1, 2, 3, 4}))
      << "an insert that belongs before entries already queued must take its "
         "place among them; a caller that reorders relies on this, and an "
         "append-only queue would release them 3, 1, 4, 2";
}

TEST(DeadlineQueueTest, OneWakeupReleasesEveryEntryAlreadyDue) {
  constexpr size_t kEntries = 8;
  DeadlineQueue queue;
  const uint64_t deadlineNs =
      getNowNs64() + nsFrom(std::chrono::microseconds(3'000));
  std::atomic<size_t> pendingSeenByFirst{kEntries};
  std::atomic<size_t> ranCount{0};
  folly::Baton<> allRan;

  for (size_t i = 0; i < kEntries; ++i) {
    queue.scheduleAt(deadlineNs, [&] {
      if (ranCount.fetch_add(1) == 0) {
        pendingSeenByFirst = queue.pendingCountForTest();
      }
      if (ranCount.load() == kEntries) {
        allRan.post();
      }
    });
  }

  ASSERT_TRUE(allRan.try_wait_for(kWaitForDispatch))
      << "every entry sharing the deadline must run";
  EXPECT_EQ(pendingSeenByFirst.load(), 0u)
      << "a wakeup must take every entry whose deadline has passed, not just "
         "the head; draining one at a time charges each later entry a fresh "
         "arm and wakeup it should not need";
}

TEST(DeadlineQueueTest, AppendsBehindTheHeadCostNoArm) {
  constexpr size_t kAppends = 8;
  DeadlineQueue queue;
  const uint64_t anHourOutNs = getNowNs64() + nsFrom(std::chrono::hours(1));
  folly::Baton<> nearestRan;

  for (size_t i = 0; i < kAppends; ++i) {
    queue.scheduleAt(anHourOutNs + i, [] {});
  }
  queue.scheduleAt(getNowNs64(), [&] { nearestRan.post(); });

  ASSERT_TRUE(nearestRan.try_wait_for(kWaitForDispatch))
      << "the entry at the nearest deadline must run; arming is posted to the "
         "timer thread in order, so its running is what makes the count below "
         "final rather than a sample of a race";
  EXPECT_EQ(queue.armCountForTest(), 3u)
      << "the timer is armed for the first entry, again for the nearer head, "
         "and once more after that head drains -- never for the seven appends "
         "that landed behind the head. Arming per entry is what made a "
         "timeout per packet expensive";
  EXPECT_EQ(queue.pendingCountForTest(), kAppends)
      << "the appends are an hour out and must still be waiting, or the count "
         "above covered something other than eight untouched appends";
}

TEST(DeadlineQueueTest, WorkThatThrowsDoesNotStopTheBatch) {
  constexpr size_t kEntries = 4;
  DeadlineQueue queue;
  const uint64_t deadlineNs =
      getNowNs64() + nsFrom(std::chrono::microseconds(1'000));
  std::atomic<size_t> ranCount{0};
  folly::Baton<> allRan;

  for (size_t i = 0; i < kEntries; ++i) {
    queue.scheduleAt(deadlineNs, [&, i] {
      if (++ranCount == kEntries) {
        allRan.post();
      }
      if (i == 0) {
        throw std::runtime_error("work failed");
      }
    });
  }

  ASSERT_TRUE(allRan.try_wait_for(kWaitForDispatch))
      << "dispatch runs under a noexcept timeoutExpired, so work that throws "
         "would end the process rather than fail this wait";
  EXPECT_EQ(ranCount.load(), kEntries)
      << "one entry throwing must not strand the entries queued behind it in "
         "the same batch";
}

TEST(DeadlineQueueTest, DestructionFreesPendingWorkRatherThanRunningIt) {
  std::atomic<bool> ran{false};
  auto payload = std::make_shared<int>(0);
  {
    DeadlineQueue queue;
    queue.scheduleAt(
        getNowNs64() + nsFrom(std::chrono::hours(1)),
        [&ran, payload] { ran = true; });
  }

  EXPECT_FALSE(ran.load())
      << "destruction must cancel pending work rather than run it: the work "
         "reaches into objects that are being torn down alongside the queue";
  EXPECT_EQ(payload.use_count(), 1)
      << "the canceled work must also be destroyed, or the packets it holds "
         "are retained for the life of the process";
}

TEST(DeadlineQueueTest, RearmsFromItsOwnDispatch) {
  constexpr size_t kFirings = 3;
  constexpr auto kDelay = std::chrono::microseconds(1'000);
  DeadlineQueue queue;
  std::atomic<size_t> remaining{kFirings};
  std::atomic<bool> releasedEarly{false};
  folly::Baton<> done;

  // Self-rescheduling work: dispatch arms the head before running work, so
  // work that queues more work must not deadlock or lose a firing.
  std::function<void()> armNext = [&] {
    const uint64_t deadlineNs = getNowNs64() + nsFrom(kDelay);
    queue.scheduleAt(deadlineNs, [&, deadlineNs] {
      if (getNowNs64() < deadlineNs) {
        releasedEarly = true;
      }
      if (--remaining == 0) {
        done.post();
      } else {
        armNext();
      }
    });
  };
  armNext();

  ASSERT_TRUE(done.try_wait_for(kWaitForDispatch))
      << "work that reschedules from inside its own dispatch must keep "
         "firing; a queue that armed only before dispatching would stall here";
  EXPECT_FALSE(releasedEarly.load())
      << "no release may precede its deadline, including the ones armed from "
         "inside a dispatch";
}

TEST(DeadlineQueueTest, StopIsIdempotentAndDropsPendingWork) {
  DeadlineQueue queue;

  std::atomic<bool> ran{false};
  queue.scheduleAt(
      getNowNs64() + nsFrom(std::chrono::hours(1)), [&] { ran = true; });

  // Twice: a caller that stops the queue and is then destroyed runs this
  // path twice, and the second call must not wait on a loop that has ended.
  queue.stop();
  queue.stop();

  EXPECT_EQ(queue.pendingCountForTest(), 0u)
      << "stop must release work still waiting, so nothing it captured is "
         "kept alive past the point the caller stopped the queue";
  EXPECT_FALSE(ran.load())
      << "stop must not run work whose deadline has not arrived; a caller "
         "stops the queue precisely to keep that work from reaching it";
}

} // namespace libsoftmetaroce
