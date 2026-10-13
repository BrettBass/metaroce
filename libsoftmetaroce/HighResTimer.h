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

#include <folly/io/async/STTimerFDTimeoutManager.h>
#include <libsoftmetaroce/ClockSource.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <string_view>

namespace libsoftmetaroce {

class HighResTimer {
 public:
  class Callback : public folly::HHWheelTimerHighRes::Callback {
   public:
    explicit Callback(folly::Function<void()> callback)
        : callback_(std::move(callback)) {}

    Callback(Callback&& other) noexcept
        : callback_(std::move(other.callback_)),
          everScheduled_(other.everScheduled_.load(std::memory_order_relaxed)) {
    }

    Callback& operator=(Callback&& other) noexcept {
      if (this != &other) {
        callback_ = std::move(other.callback_);
        everScheduled_.store(
            other.everScheduled_.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
      }
      return *this;
    }

    void timeoutExpired() noexcept override {
      callback_();
    }

    // The default callbackCanceled() calls timeoutExpired(), which would
    // execute the callback during cancelAll() in the destructor. The
    // callback may capture references to objects that are being destroyed,
    // so we override to make cancellation a no-op.
    void callbackCanceled() noexcept override {}

    void operator()() noexcept {
      callback_();
    }

   private:
    // For everScheduled_, which the timer sets on schedule and reads on cancel.
    friend class HighResTimer;

    folly::Function<void()> callback_;
    // allows skipping expensive cancel for never scheduled callbacks
    std::atomic<bool> everScheduled_{false};
  };

  explicit HighResTimer(std::string_view namePrefix = "lsmrTimer");

  // Cancel pending callbacks and join the timer thread, leaving this object
  // intact. Idempotent, so the destructor has no callback to cancel and no
  // thread to join.
  void stop() {
    if (stopped_) {
      return;
    }
    stopped_ = true;
    evb_.runInEventBaseThreadAndWait([this] {
      wheelTimer_->cancelAll();
      evb_.terminateLoopSoon();
    });
    if (thread_ && thread_->joinable()) {
      thread_->join();
    }
  }

  ~HighResTimer() {
    stop();
  }

  void cancelCallback(Callback* callback) {
    CHECK(thread_)
        << "cannot cancel a timeout after the thread has been destroyed.";
    if (!callback->everScheduled_.load(std::memory_order_acquire)) {
      return;
    }
    VLOG(2) << "Canceling timeout";
    evb_.runInEventBaseThreadAndWait([callback] { callback->cancelTimeout(); });
  }

  void scheduleTimeout(
      const std::chrono::microseconds& newTimeoutUs,
      Callback* callback) {
    VLOG(2) << "Scheduling timeout for " << newTimeoutUs.count() << "us";
    callback->everScheduled_.store(true, std::memory_order_release);
    evb_.runInEventBaseThread([this, callback, newTimeoutUs]() {
      wheelTimer_->scheduleTimeout(callback, newTimeoutUs);
    });
  }

  // Returns once the timer thread has passed this point. A timeout armed
  // before it still expires.
  void fenceCallbacks() {
    evb_.runInEventBaseThreadAndWait([] {});
  }

  // The armed state is written on the timer thread, so it is read there.
  bool isScheduled(Callback* callback) {
    bool scheduled = false;
    evb_.runInEventBaseThreadAndWait(
        [callback, &scheduled] { scheduled = callback->isScheduled(); });
    return scheduled;
  }

  void scheduleTimeout(
      const std::chrono::microseconds& newTimeoutUs,
      Callback callback) {
    VLOG(2) << "Scheduling timeout for " << newTimeoutUs.count() << "us";
    // Heap-allocate as an OwnedCallback that self-deletes when fired or
    // canceled. scheduleTimeoutFn() wraps callbacks in a Wrapper whose
    // default callbackCanceled() calls timeoutExpired(), executing the
    // callback during cancelAll() — a use-after-free when captured objects
    // are destroyed. OwnedCallback preserves the no-op execution semantics
    // on cancel while cleaning up its own memory.
    evb_.runInEventBaseThread(
        [this, cb = new OwnedCallback(std::move(callback)), newTimeoutUs] {
          wheelTimer_->scheduleTimeout(cb, newTimeoutUs);
        });
  }

 private:
  // Self-deleting callback for the value-taking scheduleTimeout overload.
  // Deletes itself after firing or when canceled during cancelAll().
  class OwnedCallback : public Callback {
   public:
    using Callback::Callback;

    void timeoutExpired() noexcept override {
      Callback::timeoutExpired();
      delete this;
    }

    void callbackCanceled() noexcept override {
      delete this;
    }
  };

  // Written only by the owner tearing this down, never on the timer thread.
  bool stopped_{false};
  folly::EventBase evb_;
  std::unique_ptr<std::thread> thread_;
  folly::HHWheelTimerHighRes::UniquePtr wheelTimer_;
  std::unique_ptr<TimeoutManager> timeoutMgr_;
};

} // namespace libsoftmetaroce
