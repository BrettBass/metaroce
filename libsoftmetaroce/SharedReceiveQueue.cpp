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

#include <libsoftmetaroce/SharedReceiveQueue.h>

#include <algorithm>

#include <glog/logging.h>

namespace libsoftmetaroce {

std::optional<SharedReceiveQueue::RecordedWaiter>
SharedReceiveQueue::takeNextRefusedTag() {
  const std::lock_guard<std::mutex> admitted{admission_};
  return takeNextRefusedTagUnderLock();
}

std::optional<SharedReceiveQueue::RecordedWaiter>
SharedReceiveQueue::takeNextRefusedTagUnderLock() {
  const auto servable = std::ranges::find_if(
      refusedTags_, [depth = posted_.size()](const RecordedWaiter& recorded) {
        return depth > recorded.threshold;
      });
  if (servable == refusedTags_.end()) {
    return std::nullopt;
  }
  const RecordedWaiter next = *servable;
  refusedTags_.erase(servable);
  return next;
}

bool SharedReceiveQueue::tryPost(SharedReceive receive) {
  std::optional<RecordedWaiter> next;
  {
    const std::lock_guard<std::mutex> admitted{admission_};
    if (posted_.size() >= maxWorkRequests_) {
      return false;
    }
    posted_.push_back(std::move(receive));
    // Selected in the same step: two posts must not both claim the depth one
    // of them added.
    if (cancelRnrForRefusedTag_ != nullptr) {
      next = takeNextRefusedTagUnderLock();
    }
  }
  // A tag the cancel cannot reach loses its record.
  while (next.has_value()) {
    if (cancelRnrForRefusedTag_(next->taker, next->csnExpected)) {
      break;
    }
    next = takeNextRefusedTag();
  }
  return true;
}

std::optional<SharedReceive> SharedReceiveQueue::tryTakeOldestLeavingAtLeast(
    size_t remaining,
    SharedReceiveTake take) {
  SharedReceive oldest;
  bool fellBelowLimit = false;
  {
    const std::lock_guard<std::mutex> admitted{admission_};
    if (posted_.size() <= remaining) {
      if (!take.csnToRecordOnRefusal.has_value()) {
        return std::nullopt;
      }
      // Recorded under the take's lock, so the refusal and the record are
      // one step.
      const auto waiting =
          std::ranges::find(refusedTags_, take.taker, &RecordedWaiter::taker);
      if (waiting != refusedTags_.end()) {
        // A retry needing more keeps the smaller threshold, so the post that
        // would have served the first refusal still cancels.
        waiting->csnExpected = *take.csnToRecordOnRefusal;
        waiting->threshold = std::min(waiting->threshold, remaining);
      } else {
        refusedTags_.push_back(
            RecordedWaiter{
                .taker = take.taker,
                .csnExpected = *take.csnToRecordOnRefusal,
                .threshold = remaining});
      }
      return std::nullopt;
    }
    oldest = std::move(posted_.front());
    posted_.pop_front();
    std::erase_if(refusedTags_, [&take](const RecordedWaiter& recorded) {
      return recorded.taker == take.taker;
    });
    if (armedLimit_ > 0 && posted_.size() < armedLimit_) {
      armedLimit_ = 0;
      fellBelowLimit = reportLimitReached_ != nullptr;
      if (fellBelowLimit) {
        // A destroy waits on this reference, so the queue is still here for
        // the report below.
        takeEventReference();
      }
    }
  }
  if (fellBelowLimit && !reportLimitReached_()) {
    releaseEventReferences(1);
  }
  return oldest;
}

void SharedReceiveQueue::armLimit(uint32_t reportBelow) {
  const std::lock_guard<std::mutex> admitted{admission_};
  armedLimit_ = reportBelow;
}

uint32_t SharedReceiveQueue::getArmedLimit() const {
  const std::lock_guard<std::mutex> admitted{admission_};
  return armedLimit_;
}

size_t SharedReceiveQueue::getNumPosted() const {
  const std::lock_guard<std::mutex> admitted{admission_};
  return posted_.size();
}

SharedReceiveQueue::~SharedReceiveQueue() {
  auto locked = references_.lock();
  CHECK_EQ(locked->queuePairs, 0u)
      << "a queue pair still holds this shared receive queue, so the free "
         "releases receives it can still take";
  CHECK_EQ(locked->outstandingEvents, 0u)
      << "an event raised for this shared receive queue is still outstanding, "
         "so a client can still read the queue out of it";
}

void SharedReceiveQueue::takeReference() {
  ++references_.lock()->queuePairs;
}

void SharedReceiveQueue::forgetRefusedTags(uint32_t qpNum) {
  const std::lock_guard<std::mutex> admitted{admission_};
  std::erase_if(refusedTags_, [qpNum](const RecordedWaiter& recorded) {
    return recorded.taker.qpNum == qpNum;
  });
}

void SharedReceiveQueue::dropReference() {
  auto locked = references_.lock();
  CHECK_GT(locked->queuePairs, 0u)
      << "a queue pair dropped a reference to this shared receive queue "
         "without holding one";
  --locked->queuePairs;
}

bool SharedReceiveQueue::isReferenced() const {
  return references_.lock()->queuePairs > 0;
}

void SharedReceiveQueue::takeEventReference() {
  ++references_.lock()->outstandingEvents;
}

void SharedReceiveQueue::releaseEventReferences(size_t count) {
  auto locked = references_.lock();
  // The invariant that events are acked once is the caller's responsibility.
  if (locked->outstandingEvents < count) {
    LOG_FIRST_N(ERROR, 20)
        << "more events were given back to this shared receive queue than were "
           "raised for it, so a destroy waiting here is released early";
    locked->outstandingEvents = 0;
  } else {
    locked->outstandingEvents -= count;
  }
  if (locked->outstandingEvents == 0) {
    // The wait reacquires this lock to return, so the notify completes first.
    allEventsAcked_.notify_all();
  }
}

void SharedReceiveQueue::waitForEventsAcked() {
  auto locked = references_.lock();
  allEventsAcked_.wait(
      locked.as_lock(), [&locked] { return locked->outstandingEvents == 0; });
}

} // namespace libsoftmetaroce
