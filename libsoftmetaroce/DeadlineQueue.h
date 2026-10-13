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

#pragma once

#include <folly/Function.h>
#include <folly/Synchronized.h>
#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/EventBase.h>
#include <libsoftmetaroce/ClockSource.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <string_view>
#include <thread>

namespace libsoftmetaroce {

/*
 * Runs work at a deadline, on its own thread.
 *
 * Pending work is sorted by deadline with the timer fd armed for the head
 * alone, which buys two things:
 *
 * - Deadlines stay absolute. The remaining wait is computed on the timer
 *   thread immediately before arming, so the hop onto that thread is spent
 *   out of the wait rather than added to it.
 * - No tick quantizes delivery. Resolution is the timer fd's.
 *
 * It expects a caller adding a constant delay or pacing at a fixed interval,
 * where each new deadline is later than every pending one, so scheduling
 * appends and dispatch pops. Deadlines that interleave still work, at a scan
 * per insert that only matters once many are in flight.
 *
 * What it gives up is canceling an arbitrary entry: there is no handle to
 * cancel one with. A caller that arms and disarms the same timeout repeatedly
 * wants `HighResTimer`.
 */
class DeadlineQueue {
 public:
  using Work = folly::Function<void()>;

  explicit DeadlineQueue(std::string_view namePrefix = "lsmrDeadline");
  ~DeadlineQueue();

  DeadlineQueue(const DeadlineQueue&) = delete;
  DeadlineQueue& operator=(const DeadlineQueue&) = delete;
  DeadlineQueue(DeadlineQueue&&) = delete;
  DeadlineQueue& operator=(DeadlineQueue&&) = delete;

  // deadlineNs is on the getNowNs64() clock, which is not steady. Work still
  // waiting at destruction is destroyed, not run.
  void scheduleAt(uint64_t deadlineNs, Work work);

  // Joins the timer thread and drops pending work, leaving this object usable
  // only for destruction. Idempotent, and what the destructor runs.
  void stop();

  void scheduleAfter(std::chrono::microseconds delayUs, Work work) {
    scheduleAt(deadlineNsFrom(delayUs), std::move(work));
  }

  size_t pendingCountForTest() const {
    return pendingByDeadline_.lock()->size();
  }

  uint64_t armCountForTest() const {
    return armCount_.load(std::memory_order_relaxed);
  }

 private:
  struct Pending {
    uint64_t deadlineNs;
    Work work;
  };

  class HeadTimeout : public folly::AsyncTimeout {
   public:
    HeadTimeout(DeadlineQueue& queue, folly::TimeoutManager* timeoutManager)
        : folly::AsyncTimeout(timeoutManager), queue_(queue) {}

    void timeoutExpired() noexcept override {
      queue_.dispatchDue();
    }

   private:
    DeadlineQueue& queue_;
  };

  static uint64_t deadlineNsFrom(std::chrono::microseconds delayUs);

  // Both run on the timer thread.
  void armForHead();
  void dispatchDue();

  // Runtime state.
  folly::Synchronized<std::deque<Pending>, std::mutex> pendingByDeadline_;
  bool stopped_{false};

  // Owned subsystems, each built from the one above it.
  folly::EventBase evb_;
  TimeoutManager timerFd_;
  HeadTimeout headTimeout_;

  // Test-only.
  std::atomic<uint64_t> armCount_{0};

  // Last of every member, so the loop it runs finds them all built.
  std::thread thread_;
};

} // namespace libsoftmetaroce
