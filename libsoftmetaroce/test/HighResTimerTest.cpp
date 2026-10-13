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
#include <libsoftmetaroce/HighResTimer.h>

#include <atomic>

namespace libsoftmetaroce {

TEST(HighResTimerTest, CancelAllDoesNotExecuteValueCallbacks) {
  std::atomic<bool> executed{false};
  {
    HighResTimer timer;
    timer.scheduleTimeout(
        std::chrono::microseconds(10'000'000),
        HighResTimer::Callback([&] { executed = true; }));
  }
  EXPECT_FALSE(executed)
      << "Value callback should not execute during cancelAll";
}

TEST(HighResTimerTest, CancelAllDoesNotExecutePointerCallbacks) {
  std::atomic<bool> executed{false};
  HighResTimer::Callback cb([&] { executed = true; });
  {
    HighResTimer timer;
    timer.scheduleTimeout(std::chrono::microseconds(10'000'000), &cb);
  }
  EXPECT_FALSE(executed)
      << "Pointer callback should not execute during cancelAll";
}

TEST(HighResTimerTest, AQueuedArmingReadsAsScheduled) {
  HighResTimer timer;
  HighResTimer::Callback cb([] {});
  timer.scheduleTimeout(std::chrono::hours(1), &cb);
  EXPECT_TRUE(timer.isScheduled(&cb))
      << "a caller must be able to read the timeout it just asked for as "
         "armed, without waiting on the timer thread itself";
  timer.cancelCallback(&cb);
  EXPECT_FALSE(timer.isScheduled(&cb))
      << "and must read it as disarmed once cancelled, or the read above "
         "proves nothing beyond returning true";
}

} // namespace libsoftmetaroce
