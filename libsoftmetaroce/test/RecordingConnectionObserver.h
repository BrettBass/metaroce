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

#include <libsoftmetaroce/ConnectionObserver.h>

#include <cstdint>
#include <vector>

namespace libsoftmetaroce {

// Records each kind of notification in callback order.
class RecordingConnectionObserver final : public ConnectionObserver {
 public:
  void onTagConfigured(const AppliedTagConfig& config) noexcept override {
    // Growing the observation history can allocate.
    containObservationFailure([&] { tagConfigs.push_back(config); });
  }

  void onQpRetiring(uint32_t localQpNum) noexcept override {
    // Growing the observation history can allocate.
    containObservationFailure([&] { retiringQps.push_back(localQpNum); });
  }

  std::vector<AppliedTagConfig> tagConfigs;
  std::vector<uint32_t> retiringQps;
};

} // namespace libsoftmetaroce
