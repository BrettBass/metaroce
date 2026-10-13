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

#include <gflags/gflags.h>

#include "libsoftmetaroce/ClockSource.h"

#include <atomic>

namespace libsoftmetaroce {
DEFINE_string(
    libsoftmetaroce_ptp_dev,
    "",
    "PTP device to use for clock source.");

std::unique_ptr<ClockSource> getClockSourceInstance() {
  if (FLAGS_libsoftmetaroce_ptp_dev != "") {
    return std::make_unique<PTPClockSource>(FLAGS_libsoftmetaroce_ptp_dev);
  } else {
    return std::make_unique<ClockSource>();
  }
};

uint64_t getNowNs64() {
  static auto clk_src = getClockSourceInstance();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             clk_src->now().time_since_epoch())
      .count();
}

uint32_t nextPtpTimerId() {
  static std::atomic<uint32_t> next{0};
  return next.fetch_add(1, std::memory_order_relaxed);
}

uint32_t getNowNs() {
  return static_cast<uint32_t>(getNowNs64());
}
} // namespace libsoftmetaroce
