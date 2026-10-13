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
#include <folly/synchronization/Baton.h>
#include <folly/system/HardwareConcurrency.h>
#include <glog/logging.h>
#include <libsoftmetaroce/ClockSource.h>
#include <libsoftmetaroce/HighResTimer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace libsoftmetaroce {

// Measures how late a HighResTimer delivers a timeout, spanning the caller's
// scheduleTimeout to the callback, so the hop onto the timer thread is in it.

constexpr std::chrono::microseconds kAccuracyDelay{10'000};

// Bounds a measurement's wait at this multiple of the time its firings need,
// low enough that a case full of them fits the vmtest timeout.
constexpr int kOvershootWaitSlack = 4;

// One timer re-arming itself, recording how late each firing was.
struct TimerSlot {
  TimerSlot(std::chrono::microseconds d, size_t firings)
      : delay(d), remaining(firings) {
    CHECK_GT(firings, 0u) << "a slot with no firings never posts its baton";
    // One less allocation in the timed callback.
    overshootUs.lock()->reserve(firings);
  }

  void arm() {
    armedAtNs = getNowNs64();
    timer.scheduleTimeout(
        delay, HighResTimer::Callback([this] {
          // Read on the clock HighResTimer derives its deadline from. Timing
          // this against a steady clock instead would fold any drift between
          // the two into the measurement.
          const int64_t elapsedNs = static_cast<int64_t>(getNowNs64()) -
              static_cast<int64_t>(armedAtNs);
          const int64_t delayNs =
              std::chrono::duration_cast<std::chrono::nanoseconds>(delay)
                  .count();
          // Subtracted before the divide: truncating elapsedNs first biases
          // every sample toward zero by up to a microsecond.
          overshootUs.lock()->push_back((elapsedNs - delayNs) / 1000);
          if (--remaining == 0) {
            done.post();
          } else {
            arm();
          }
        }));
  }

  // Configuration.
  const std::chrono::microseconds delay;

  // Runtime state. A slot whose wait expired is still firing while its samples
  // are read, so the lock is what makes that read safe.
  size_t remaining;
  folly::Synchronized<std::vector<int64_t>, std::mutex> overshootUs;
  uint64_t armedAtNs{0};

  // Test-only synchronization.
  folly::Baton<> done;

  // Owned subsystems, declared last so the timer is destroyed first, joining
  // its thread before the members its callback touches go away.
  HighResTimer timer;
};

struct OvershootMeasurement {
  // Sorted, and short of numTimers * firingsPerTimer by whatever the unfinished
  // slots had left to fire.
  std::vector<int64_t> overshootUs;

  // What each sample is an overshoot past, so a reported line carries the
  // interval its microseconds are relative to.
  int64_t delayUs{0};

  // Slots whose samples were still incomplete when taken; finishing late does
  // not count. Nonzero means the run reported is a short one.
  size_t unfinishedSlots{0};
};

inline OvershootMeasurement measureOvershoot(
    size_t numTimers,
    size_t firingsPerTimer,
    std::chrono::microseconds delay) {
  std::deque<TimerSlot> slots;
  for (size_t i = 0; i < numTimers; ++i) {
    slots.emplace_back(delay, firingsPerTimer);
  }
  for (auto& s : slots) {
    s.arm();
  }
  // Slots fire concurrently, so this does not scale with their count.
  const auto deadline = std::chrono::steady_clock::now() +
      delay * firingsPerTimer * kOvershootWaitSlack + std::chrono::seconds(10);
  OvershootMeasurement measurement;
  measurement.delayUs = delay.count();
  for (auto& s : slots) {
    if (!s.done.try_wait_until(deadline)) {
      ++measurement.unfinishedSlots;
    }
  }
  auto& all = measurement.overshootUs;
  all.reserve(numTimers * firingsPerTimer);
  for (auto& s : slots) {
    // Copied rather than moved out: an unfinished slot is still firing, and
    // emptying its vector would put an allocation back in the timed callback.
    const auto locked = s.overshootUs.lock();
    all.insert(all.end(), locked->begin(), locked->end());
  }
  std::sort(all.begin(), all.end());
  return measurement;
}

inline void reportOvershoot(
    std::string_view label,
    const OvershootMeasurement& measurement) {
  const auto& v = measurement.overshootUs;
  DCHECK(std::is_sorted(v.begin(), v.end()))
      << "the percentiles below are read by index";
  const auto unfinished = measurement.unfinishedSlots;
  const std::string fixed = " delay_us=" + std::to_string(measurement.delayUs) +
      " vcpus=" + std::to_string(folly::available_concurrency());
  if (v.empty()) {
    LOG(INFO) << "TIMERACC " << label << fixed
              << " n=0 unfinished_slots=" << unfinished;
    return;
  }
  // Nearest-rank: the smallest value at or above the requested fraction.
  const auto at = [&](double p) {
    const auto rank = static_cast<size_t>(std::ceil(p * v.size()));
    return v[std::min(std::max<size_t>(rank, 1) - 1, v.size() - 1)];
  };
  LOG(INFO) << "TIMERACC " << label << fixed << " n=" << v.size()
            << " unfinished_slots=" << unfinished << " min_us=" << v.front()
            << " p50_us=" << at(0.50) << " p90_us=" << at(0.90)
            << " p99_us=" << at(0.99) << " max_us=" << v.back();
}

} // namespace libsoftmetaroce
