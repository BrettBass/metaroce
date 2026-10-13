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

// Reports through LOG(INFO), so run with GLOG_logtostderr=1 GLOG_minloglevel=0.

#include <folly/init/Init.h>
#include <folly/synchronization/Baton.h>
#include <libsoftmetaroce/DeadlineQueue.h>
#include <libsoftmetaroce/test/TimerDelayProbe.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

namespace libsoftmetaroce {
namespace {

constexpr size_t kFirings = 200;
constexpr auto kDelay = std::chrono::microseconds(10'000);

// One deadline at a time, re-armed from its own dispatch.
struct DeadlineSlot {
  DeadlineSlot(std::chrono::microseconds d, size_t firings)
      : delay(d), remaining(firings) {}

  void arm() {
    deadlineNs = getNowNs64() +
        static_cast<uint64_t>(
                     std::chrono::duration_cast<std::chrono::nanoseconds>(delay)
                         .count());
    queue.scheduleAt(deadlineNs, [this] {
      overshootNs.push_back(
          static_cast<int64_t>(getNowNs64()) -
          static_cast<int64_t>(deadlineNs));
      if (--remaining == 0) {
        done.post();
      } else {
        arm();
      }
    });
  }

  std::chrono::microseconds delay;
  size_t remaining;
  std::vector<int64_t> overshootNs;
  uint64_t deadlineNs{0};
  folly::Baton<> done;
  // Declared last so it is destroyed first, joining its thread before the
  // members its work touches go away.
  DeadlineQueue queue;
};

} // namespace
} // namespace libsoftmetaroce

int main(int argc, char** argv) {
  folly::Init init(&argc, &argv);

  libsoftmetaroce::DeadlineSlot slot(
      libsoftmetaroce::kDelay, libsoftmetaroce::kFirings);
  slot.arm();
  slot.done.wait();

  std::vector<int64_t> overshootUs;
  overshootUs.reserve(slot.overshootNs.size());
  for (const auto ns : slot.overshootNs) {
    overshootUs.push_back(ns / 1000);
  }
  std::sort(overshootUs.begin(), overshootUs.end());
  libsoftmetaroce::reportOvershoot(
      "deadlinequeue",
      libsoftmetaroce::OvershootMeasurement{
          .overshootUs = std::move(overshootUs),
          .delayUs = libsoftmetaroce::kDelay.count()});
  return 0;
}
