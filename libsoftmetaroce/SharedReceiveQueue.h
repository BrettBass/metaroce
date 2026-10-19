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

#include <folly/Synchronized.h>

#include <libsoftmetaroce/Constants.h>
#include <libsoftmetaroce/SgeList.h>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>

struct ibv_pd;

namespace libsoftmetaroce {

// A receive `ibv_post_srq_recv` took, held until a queue pair takes it.
struct SharedReceive {
  uint64_t workRequestId{0};
  LocalSges sges;
};

struct SharedReceiveTaker {
  uint32_t qpNum{0};
  uint8_t tagNum{0};

  bool operator==(const SharedReceiveTaker&) const = default;
};

struct SharedReceiveTake {
  // Outside the optional: a take that succeeds drops this taker's record.
  SharedReceiveTaker taker;
  // Present only when a refused take should record an RNR-Cancel request.
  std::optional<uint16_t> csnToRecordOnRefusal;
};

// The receives several queue pairs draw on, handed out oldest first. A client
// arms a limit to be told once when a take leaves fewer than that waiting, and
// a tag refused a take has its RNR canceled by the post that serves it.
class SharedReceiveQueue {
 public:
  SharedReceiveQueue(
      uint32_t maxWorkRequests,
      uint32_t maxSge,
      struct ibv_pd& protectionDomain,
      std::function<bool()> reportLimitReached = {},
      std::function<bool(SharedReceiveTaker, uint16_t)> cancelRnrForRefusedTag =
          {})
      : reportLimitReached_{std::move(reportLimitReached)},
        cancelRnrForRefusedTag_{std::move(cancelRnrForRefusedTag)},
        maxWorkRequests_{maxWorkRequests},
        maxSge_{maxSge},
        protectionDomain_{protectionDomain} {}

  [[nodiscard]] uint32_t getMaxWorkRequests() const {
    return maxWorkRequests_;
  }
  [[nodiscard]] uint32_t getMaxSge() const {
    return maxSge_;
  }
  [[nodiscard]] struct ibv_pd* getProtectionDomain() const {
    return &protectionDomain_;
  }

  // Takes the receive, or answers false where the queue already holds the
  // depth `ibv_create_srq` was given.
  [[nodiscard]] bool tryPost(SharedReceive receive);

  // Tests and takes in one step, so two callers cannot both take the last
  // receive. Takes only where more than `remaining` are posted, so a caller
  // can hold buffers back for takes it knows are coming. Where
  // `take.csnToRecordOnRefusal` holds, a refused take records the taker in the
  // same step; a take that succeeds drops the taker's record. The caller reads
  // the csn on the thread that advances it.
  [[nodiscard]] std::optional<SharedReceive> tryTakeOldestLeavingAtLeast(
      size_t remaining,
      SharedReceiveTake take);

  [[nodiscard]] size_t getNumPosted() const;

  // Asks to be told once, when a take next leaves fewer than `reportBelow`
  // receives waiting.
  void armLimit(uint32_t reportBelow);
  [[nodiscard]] uint32_t getArmedLimit() const;

  ~SharedReceiveQueue();

  SharedReceiveQueue(const SharedReceiveQueue&) = delete;
  SharedReceiveQueue& operator=(const SharedReceiveQueue&) = delete;
  SharedReceiveQueue(SharedReceiveQueue&&) = delete;
  SharedReceiveQueue& operator=(SharedReceiveQueue&&) = delete;

  // Held by each queue pair drawing its receives from this queue.
  void takeReference();
  void dropReference();
  // Drops the records of `qpNum`'s tags held here, so a later queue pair
  // reissued that number does not take their cancel.
  void forgetRefusedTags(uint32_t qpNum);
  // `ibv_destroy_srq` answers `EBUSY` while this holds.
  [[nodiscard]] bool isReferenced() const;

  // Held by an async event raised for this queue until the client acks it.
  void takeEventReference();
  void releaseEventReferences(size_t count);
  // Blocks until every event raised for this queue has been acked, which
  // `ibv_get_async_event(3)` requires of the destroy that frees it.
  void waitForEventsAcked();

 private:
  // Injected dependencies.
  // Returns whether the client was told. Called outside the lock a take
  // holds, and an event reference is held across it.
  const std::function<bool()> reportLimitReached_;
  // Returns whether a cancel reached the tag. On false the post moves on to
  // the next recorded tag its depth serves.
  const std::function<bool(SharedReceiveTaker, uint16_t)>
      cancelRnrForRefusedTag_;

  // Configuration.
  const uint32_t maxWorkRequests_;
  const uint32_t maxSge_;
  struct ibv_pd& protectionDomain_;

  // Runtime state.
  mutable std::mutex admission_;
  std::deque<SharedReceive> posted_;
  // Cleared as it fires, so a client is told once per arming.
  uint32_t armedLimit_{0};
  struct RecordedWaiter {
    SharedReceiveTaker taker;
    uint16_t csnExpected{0};
    // The `remaining` the refused take passed: a post cancels for this tag
    // once its depth clears it.
    size_t threshold{0};
  };
  // Oldest first, at most one entry per tag. A post cancels for the first
  // entry its depth serves, since the receive it adds serves one take.
  std::deque<RecordedWaiter> refusedTags_;
  struct References {
    // A destroy refuses while a queue pair can still draw from this queue.
    size_t queuePairs{0};
    // A destroy waits on an event raised for this queue and not yet acked.
    size_t outstandingEvents{0};
  };
  folly::Synchronized<References, std::mutex> references_;
  std::condition_variable allEventsAcked_;

  // The oldest recorded tag the posted depth serves, removed, or empty where it
  // serves none.
  [[nodiscard]] std::optional<RecordedWaiter> takeNextRefusedTag();
  // As above, called with `admission_` held.
  [[nodiscard]] std::optional<RecordedWaiter> takeNextRefusedTagUnderLock();
};

} // namespace libsoftmetaroce
