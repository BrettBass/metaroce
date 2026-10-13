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
#include <libsoftmetaroce/BlockingCoroLock.h>

namespace libsoftmetaroce {
namespace {

TEST(BlockingCoroLockTest, ATryOnAHeldMutexReturnsWithoutRunning) {
  folly::coro::Mutex mutex;
  ASSERT_TRUE(mutex.try_lock()) << "precondition: the mutex starts free";
  bool ran = false;
  const bool tookMutex = tryWithCoroMutexHeld(mutex, [&] { ran = true; });
  EXPECT_FALSE(tookMutex)
      << "a caller on a held mutex falls back to its slow path, so the try "
         "must report that it did not take the mutex";
  EXPECT_FALSE(ran) << "fn runs only under the mutex, which another holder has";
  mutex.unlock();
}

TEST(BlockingCoroLockTest, ATryOnAFreeMutexRunsUnderItAndReleasesIt) {
  folly::coro::Mutex mutex;
  bool heldWhileRunning = false;
  const bool tookMutex = tryWithCoroMutexHeld(
      mutex, [&] { heldWhileRunning = !mutex.try_lock(); });
  EXPECT_TRUE(tookMutex)
      << "a free mutex is taken, so the caller skips its slow path";
  EXPECT_TRUE(heldWhileRunning)
      << "fn reads state the mutex guards, so it must run with the mutex held";
  EXPECT_TRUE(mutex.try_lock())
      << "the mutex must be free again once fn returns, or every later caller "
         "would wait on it";
  mutex.unlock();
}

} // namespace
} // namespace libsoftmetaroce
