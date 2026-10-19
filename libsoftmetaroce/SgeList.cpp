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

#include <glog/logging.h>
#include <libsoftmetaroce/Constants.h>
#include <libsoftmetaroce/SgeList.h>
#include <limits>

namespace libsoftmetaroce {

std::optional<ValidatedSgeList> validatedSgeList(
    const struct ibv_sge* FOLLY_NULLABLE sgList,
    int numSge) {
  if (numSge < 0 || static_cast<uint32_t>(numSge) > kMaxSgesPerWorkRequest ||
      (numSge != 0 && sgList == nullptr)) {
    return std::nullopt;
  }
  const std::span<const struct ibv_sge> sges(
      sgList, static_cast<size_t>(numSge));
  uint64_t totalLength = 0;
  for (const struct ibv_sge& sge : sges) {
    totalLength += sge.length;
  }
  if (totalLength > std::numeric_limits<uint32_t>::max()) {
    return std::nullopt;
  }
  return ValidatedSgeList{
      .sges = sges, .totalLength = static_cast<uint32_t>(totalLength)};
}

uint32_t totalSgeLength(std::span<const LocalSge> sges) {
  uint64_t totalBytes = 0;
  for (const LocalSge& sge : sges) {
    totalBytes += sge.length;
  }
  CHECK_LE(totalBytes, std::numeric_limits<uint32_t>::max())
      << "the total SGE length must fit in uint32_t";
  return static_cast<uint32_t>(totalBytes);
}

} // namespace libsoftmetaroce
