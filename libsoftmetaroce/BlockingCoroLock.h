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

#include <folly/coro/BlockingWait.h>
#include <folly/coro/Mutex.h>
#include <folly/coro/Task.h>

#include <mutex>

namespace libsoftmetaroce {

// Runs fn with mutex held, from a thread that is not running a coroutine. It
// builds a coroutine only to wait on a contended mutex.
template <typename Fn>
auto withCoroMutexHeld(folly::coro::Mutex& mutex, Fn&& fn) {
  if (mutex.try_lock()) {
    std::scoped_lock<folly::coro::Mutex> lock{std::adopt_lock, mutex};
    return fn();
  }
  return folly::coro::blockingWait([&]() -> folly::coro::Task<decltype(fn())> {
    auto lock = co_await mutex.co_scoped_lock();
    co_return fn();
  }());
}

// Runs fn with mutex held if the mutex is free, without waiting for it, and
// returns whether fn ran.
template <typename Fn>
bool tryWithCoroMutexHeld(folly::coro::Mutex& mutex, Fn&& fn) {
  if (!mutex.try_lock()) {
    return false;
  }
  std::scoped_lock<folly::coro::Mutex> lock{std::adopt_lock, mutex};
  fn();
  return true;
}

} // namespace libsoftmetaroce
