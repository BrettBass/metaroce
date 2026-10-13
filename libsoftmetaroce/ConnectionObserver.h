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

#include <folly/synchronization/RelaxedAtomic.h>
#include <glog/logging.h>

#include <cstdint>
#include <exception>
#include <optional>
#include <utility>

namespace libsoftmetaroce {

// Configuration updates produced by one operation. Missing fields leave prior
// observer state unchanged; remoteQpNum is scoped to the QP.
struct AppliedTagConfig {
  uint32_t localQpNum{0};
  uint8_t tag{0};
  std::optional<uint32_t> remoteQpNum;
  std::optional<uint16_t> sendCsn;
  std::optional<uint16_t> receiveCsn;
  std::optional<uint16_t> sendMsn;
  std::optional<uint16_t> receiveMsn;

  [[nodiscard]] bool hasAppliedValue() const {
    return remoteQpNum || sendCsn || receiveCsn || sendMsn || receiveMsn;
  }
};

// Logs a dropped observation without allowing logging exceptions to escape.
inline void logDroppedObservation(const char* what) noexcept {
  // glog's FIRST_N counter isn't thread-safe.
  /* library-local */ static folly::relaxed_atomic<uint64_t>
      droppedObservations{0};
  try {
    if (what == nullptr) {
      LOG_IF(WARNING, droppedObservations.fetch_add(1) < 20)
          << "dropped a connection observation that threw a non-std::exception "
             "value";
    } else {
      LOG_IF(WARNING, droppedObservations.fetch_add(1) < 20)
          << "dropped a connection observation that threw: " << what;
    }
  } catch (...) {
  }
}

template <typename Fn>
bool containObservationFailure(Fn&& fn) noexcept {
  try {
    std::forward<Fn>(fn)();
  } catch (const std::exception& e) {
    logDroppedObservation(e.what());
    return false;
  } catch (...) {
    logDroppedObservation(nullptr);
    return false;
  }
  return true;
}

class ConnectionObserver {
 public:
  virtual ~ConnectionObserver() = default;
  // Runs outside the transport's QP lock: it may call back into verbs.
  virtual void onTagConfigured(const AppliedTagConfig& config) noexcept = 0;
  // Runs under the transport's QP lock: it must not call back into verbs.
  virtual void onQpRetiring(uint32_t localQpNum) noexcept = 0;
};

} // namespace libsoftmetaroce
