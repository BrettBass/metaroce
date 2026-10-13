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

#include <libsoftmetaroce/DeadlineQueue.h>

#include <folly/Synchronized.h>
#include <folly/system/ThreadName.h>
#include <glog/logging.h>
#include <exception>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace libsoftmetaroce {

namespace {

std::string nextQueueName(std::string_view prefix) {
  static folly::Synchronized<std::map<std::string, uint32_t>> nextIdByPrefix;
  std::string name(prefix);
  const uint32_t id = (*nextIdByPrefix.wlock())[name]++;
  return name + std::to_string(id);
}

std::chrono::microseconds microsecondsUntil(
    uint64_t deadlineNs,
    uint64_t nowNs) {
  static_assert(
      std::numeric_limits<uint64_t>::max() / 1000 <
          static_cast<uint64_t>(std::chrono::microseconds::max().count()),
      "the divide below is what makes the cast to the signed rep safe");
  if (deadlineNs <= nowNs) {
    return std::chrono::microseconds(0);
  }
  const uint64_t remainingNs = deadlineNs - nowNs;
  // Rounded up: truncating would arm before the deadline.
  const uint64_t remainingUs = remainingNs / 1000 + (remainingNs % 1000 != 0);
  return std::chrono::microseconds(static_cast<int64_t>(remainingUs));
}

} // namespace

uint64_t DeadlineQueue::deadlineNsFrom(std::chrono::microseconds delayUs) {
  const uint64_t nowNs = getNowNs64();
  if (delayUs.count() <= 0) {
    return nowNs;
  }
  // Bounds both the multiply and the sum. The largest delay a caller can
  // configure is a uint32 of microseconds, a millionth of this.
  DCHECK_LE(
      static_cast<uint64_t>(delayUs.count()),
      (std::numeric_limits<uint64_t>::max() - nowNs) / 1000);
  return nowNs + static_cast<uint64_t>(delayUs.count()) * 1000;
}

DeadlineQueue::DeadlineQueue(std::string_view namePrefix)
    : timerFd_(&evb_),
      headTimeout_(*this, &timerFd_),
      thread_([this, name = nextQueueName(namePrefix)]() {
        folly::setThreadName(name);
        evb_.loopForever();
      }) {}

DeadlineQueue::~DeadlineQueue() {
  stop();
}

void DeadlineQueue::stop() {
  if (stopped_) {
    return;
  }
  stopped_ = true;
  evb_.runInEventBaseThreadAndWait([this] {
    headTimeout_.cancelTimeout();
    evb_.terminateLoopSoon();
  });
  if (thread_.joinable()) {
    thread_.join();
  }
  // Destroyed rather than run: the work reaches into objects a caller is free
  // to tear down once it has stopped the queue.
  pendingByDeadline_.lock()->clear();
}

void DeadlineQueue::scheduleAt(uint64_t deadlineNs, Work work) {
  // A stopped queue has no thread left to reach this work.
  DCHECK(!stopped_);
  {
    auto locked = pendingByDeadline_.lock();
    // Backward from the end: a constant delay or a fixed interval puts every
    // new deadline last, which this reaches in one comparison.
    auto it = locked->end();
    while (it != locked->begin() && std::prev(it)->deadlineNs > deadlineNs) {
      --it;
    }
    it = locked->insert(it, Pending{deadlineNs, std::move(work)});
    const bool isNewHead = (it == locked->begin());
    // Only a new head changes when the queue must next wake. The empty queue's
    // first entry is a head.
    if (isNewHead) {
      evb_.runInEventBaseThread([this] { armForHead(); });
    }
  }
}

void DeadlineQueue::armForHead() {
  uint64_t deadlineNs = 0;
  bool haveHead = false;
  {
    auto locked = pendingByDeadline_.lock();
    if (!locked->empty()) {
      deadlineNs = locked->front().deadlineNs;
      haveHead = true;
    }
  }
  // Read out and unlocked first: arming reaches a timerfd syscall, and a
  // scheduleAt would wait on the lock held across it.
  if (!haveHead) {
    headTimeout_.cancelTimeout();
    return;
  }
  armCount_.fetch_add(1, std::memory_order_relaxed);
  headTimeout_.scheduleTimeoutHighRes(
      microsecondsUntil(deadlineNs, getNowNs64()));
}

void DeadlineQueue::dispatchDue() {
  std::vector<Work> dueWork;
  {
    auto locked = pendingByDeadline_.lock();
    const uint64_t nowNs = getNowNs64();
    for (; !locked->empty() && locked->front().deadlineNs <= nowNs;
         locked->pop_front()) {
      dueWork.push_back(std::move(locked->front().work));
    }
  }
  // One batch per wakeup: this runs on the event base thread, so looping over
  // a backlog here would starve the post stop() waits on.
  armForHead();
  for (auto& work : dueWork) {
    // Contained per entry: this runs under a noexcept timeoutExpired, where an
    // escaping exception would end the process.
    try {
      work();
    } catch (const std::exception& e) {
      LOG(ERROR) << "DeadlineQueue: dropped work that threw: " << e.what();
    } catch (...) {
      LOG(ERROR) << "DeadlineQueue: dropped work that threw a non-exception";
    }
  }
}

} // namespace libsoftmetaroce
