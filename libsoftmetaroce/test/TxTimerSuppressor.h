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
#include <cstdint>

DECLARE_uint32(libsoftmetaroce_min_rto_us);
DECLARE_int32(libsoftmetaroce_min_rnr_backoff_us);

namespace libsoftmetaroce {

// Pins the QP's retransmission (RTO) and HRNR/BRNR backoff timers far beyond
// any test's runtime so neither fires; restores the previous values on
// destruction.
class ScopedSuppressedTxTimers {
 public:
  ScopedSuppressedTxTimers()
      : savedRtoUs_(FLAGS_libsoftmetaroce_min_rto_us),
        savedRnrBackoffUs_(FLAGS_libsoftmetaroce_min_rnr_backoff_us) {
    FLAGS_libsoftmetaroce_min_rto_us = 3'600'000'000; // ~1 hour
    FLAGS_libsoftmetaroce_min_rnr_backoff_us = 1'000'000'000; // ~1000 s
  }
  ~ScopedSuppressedTxTimers() {
    FLAGS_libsoftmetaroce_min_rto_us = savedRtoUs_;
    FLAGS_libsoftmetaroce_min_rnr_backoff_us = savedRnrBackoffUs_;
  }

 private:
  uint32_t savedRtoUs_;
  int32_t savedRnrBackoffUs_;
};

} // namespace libsoftmetaroce
