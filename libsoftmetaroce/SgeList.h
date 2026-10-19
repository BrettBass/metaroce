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

#include <folly/CppAttributes.h>
#include <folly/small_vector.h>
#include <infiniband/verbs.h>
#include <libsoftmetaroce/Constants.h>
#include <cstdint>
#include <optional>
#include <span>

namespace libsoftmetaroce {

// The verbs caller's sges, borrowed, with their addresses as posted.
// validatedSgeList() checks their shape and total length; MemoryRegions checks
// their keys.
struct ValidatedSgeList {
  std::span<const struct ibv_sge> sges;
  uint32_t totalLength{0};
};

// An sge copied out of the work request, its address resolved to local virtual
// memory and its lkey retained for validation.
struct LocalSge {
  void* FOLLY_NULLABLE address{nullptr};
  uint32_t length{0};
  uint32_t lkey{0};
};

using LocalSges = folly::small_vector<LocalSge, kMaxSgesPerWorkRequest>;

// nullopt for a malformed list: a count past the device limit, a missing list,
// or a total length a message cannot hold.
[[nodiscard]] std::optional<ValidatedSgeList> validatedSgeList(
    const struct ibv_sge* FOLLY_NULLABLE sgList,
    int numSge);

} // namespace libsoftmetaroce
