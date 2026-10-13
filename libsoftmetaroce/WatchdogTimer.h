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

#include <gflags/gflags.h>

#include <atomic>
#include <chrono>
#include <cstdint>

DECLARE_uint32(libsoftmetaroce_qp_watchdog_interval_ms);

namespace libsoftmetaroce {

// Below this a watchdog fires on ordinary scheduling delay.
inline constexpr std::chrono::milliseconds kMinWatchdogIntervalMs{20};

struct QpAttributeDeadlines {
  // How long the QP tolerates no progress.
  std::chrono::milliseconds watchdogIntervalMs;
};

// The exponent and retry count that derive `intervalMs`, saturating where the
// ceiling and IBTA's three-bit retry count cannot reach it.
struct QpAttributesForInterval {
  uint8_t localAckTimeoutExponent;
  uint8_t retryCount;
};
[[nodiscard]] QpAttributesForInterval qpAttributesForInterval(
    std::chrono::milliseconds intervalMs);

// The deadline a QP tolerates without progress. The zero exponent asks for no
// timeout and gets kDisabledTimeoutWatchdogMs rather than none at all.
[[nodiscard]] QpAttributeDeadlines deadlinesFromQpAttributes(
    uint8_t localAckTimeoutExponent,
    uint8_t retryCount);

// Thread-safe watchdog timer that tracks progress and detects stalls.
// Uses atomic operations to safely handle concurrent reads and writes
// from different threads (e.g., ACK processing thread and timer callbacks).
class WatchdogTimer {
 public:
  WatchdogTimer();

  // Record that progress was made (e.g., when a packet is ACKed).
  // Thread-safe.
  void recordProgress();

  // Records progress as of a moment the caller specifies, so a caller keeping
  // its own clock reaches expiry without waiting out the interval. Thread-safe.
  void recordProgressAt(std::chrono::steady_clock::time_point at);

  // Returns true if the time since last progress exceeds the watchdog interval.
  // Thread-safe.
  [[nodiscard]] bool hasExpired() const;

  // Configures the interval hasExpired() measures against, instead of the
  // libsoftmetaroce_qp_watchdog_interval_ms flag. Thread-safe.
  void configureInterval(std::chrono::milliseconds intervalMs);

  // The interval hasExpired() measures against, floored and never below
  // kMinWatchdogIntervalMs. Thread-safe.
  [[nodiscard]] std::chrono::milliseconds getIntervalMs() const;

  // Try to claim the right to handle expiry. Returns true if this caller
  // should handle the expiry, false if another caller already claimed it.
  // This ensures only one expiry handler runs even if multiple timer callbacks
  // detect the expiry simultaneously.
  // Thread-safe.
  [[nodiscard]] bool tryClaimExpiry();

  // Get the last progress time for debugging/logging.
  // Thread-safe.
  [[nodiscard]] std::chrono::steady_clock::time_point getLastProgressTime()
      const;

 private:
  // Convert time_point to int64_t nanoseconds for atomic storage
  static constexpr int64_t toNanos(std::chrono::steady_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               tp.time_since_epoch())
        .count();
  }

  // Convert int64_t nanoseconds back to time_point
  static constexpr std::chrono::steady_clock::time_point fromNanos(
      int64_t nanos) {
    return std::chrono::steady_clock::time_point(
        std::chrono::nanoseconds(nanos));
  }

  // Configuration.
  std::atomic<uint64_t> intervalMs_;

  // Runtime state.
  // Last time progress was made, stored as nanoseconds since epoch.
  // Using int64_t for guaranteed lock-free atomic operations.
  std::atomic<int64_t> lastProgressTimeNs_;

  // Guards against redundant expiry handling. Since the watchdog check runs
  // in multiple timer callbacks (retx, HRNR, BRNR), we use this to ensure
  // only one expiry is processed.
  std::atomic<bool> expiryScheduled_{false};
};

} // namespace libsoftmetaroce
