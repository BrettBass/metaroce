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

#include <condition_variable>
#include <shared_mutex>
#include <unordered_set>
#include <vector>

#include "rdma_gen/Common.h"

namespace facebook::neteng::ai::rdma_gen {

class RdmaGenPoolManager {
 public:
  explicit RdmaGenPoolManager(uint32_t poolSize);

  // only called once at requester side
  void requesterInit(
      uint32_t poolSize,
      uint32_t txDepth,
      const std::vector<uint32_t>& lenVec,
      const std::vector<void*>& addrs);

  // checks whether freePool_ for this qpIdx is empty
  bool freePoolEmpty(uint32_t qpIdx);

  // moves from freePool_ to externalPendingPool_
  RcBuffer moveFromFreeToPending(uint32_t qpIdx);

  // moves from externalPendingPool_ to waitingCompletionPool_
  void moveFromPendingToWaiting(uint32_t qpIdx, uint32_t bufferIdx);

  // moves from waitingCompletionPool_ to freePool_
  void moveFromWaitingToFree(
      const std::vector<void*>& addrs,
      uint32_t len,
      uint32_t qpIdx,
      uint32_t bufferIdx,
      std::optional<uint32_t> maybeImmediateData = std::nullopt);

  // Waits for a buffer in freePool to become available for any qp
  // in qpIds.  If cancelToken is provided, registers the pool CV
  // with it for the duration of the wait and exits immediately
  // when the token is cancelled.
  void waitForAvailable(
      const std::vector<uint32_t>& qpIds,
      ContinueFlag* cancelToken = nullptr);

  uint32_t getFreePoolSize() const;
  uint32_t getExternalPendingPoolSize() const;
  uint32_t getWaitingCompletionPoolSize() const;

  uint32_t getFreePoolSizeForQp(uint32_t qpIdx) const;
  uint32_t getExternalPendingPoolSizeForQp(uint32_t qpIdx) const;
  uint32_t getWaitingCompletionPoolSizeForQp(uint32_t qpIdx) const;

  uint32_t getBufTokenInFreePool(uint32_t qpIdx, uint32_t pos) const;
  bool checkInExternalPendingPool(uint32_t qpIdx, uint32_t bufferIdx) const;
  bool checkInWaitingCompletionPool(uint32_t qpIdx, uint32_t bufferIdx) const;

 private:
  // Mutex to guard RcBuffer pools.
  mutable std::shared_mutex poolMutex_;
  std::condition_variable_any poolCv_;
  std::vector<std::vector<RcBuffer>> freePool_;
  std::vector<std::unordered_set<uint64_t>> externalPendingPool_;
  std::vector<std::unordered_set<uint64_t>> waitingCompletionPool_;
};

} // namespace facebook::neteng::ai::rdma_gen
