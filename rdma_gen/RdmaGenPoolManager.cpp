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

#include "rdma_gen/RdmaGenPoolManager.h"

#include <folly/ScopeGuard.h>

using namespace std::chrono_literals;

namespace facebook::neteng::ai::rdma_gen {

RdmaGenPoolManager::RdmaGenPoolManager(uint32_t poolSize) {
  std::unique_lock lock(poolMutex_);
  freePool_.resize(poolSize);
  externalPendingPool_.resize(poolSize);
  waitingCompletionPool_.resize(poolSize);
}

void RdmaGenPoolManager::requesterInit(
    uint32_t poolSize,
    uint32_t txDepth,
    const std::vector<uint32_t>& lenVec,
    const std::vector<void*>& addrs) {
  // Making linter happy
  CHECK(!lenVec.empty());
  std::unique_lock lock(poolMutex_);
  for (uint32_t i = 0; i < poolSize; i++) {
    for (uint32_t j = 0; j < txDepth; j++) {
      RcBuffer rcBuf = {
          .buf = addrs[i * txDepth + j],
          .len = lenVec[i],
          .token = i * txDepth + j,
          .qp_idx = i};
      freePool_[i].push_back(rcBuf);
    }
  }
}

bool RdmaGenPoolManager::freePoolEmpty(uint32_t qpIdx) {
  std::shared_lock lock(poolMutex_);
  return freePool_.at(qpIdx).empty();
}

void RdmaGenPoolManager::waitForAvailable(
    const std::vector<uint32_t>& qpIds,
    ContinueFlag* cancelToken) {
  if (cancelToken) {
    cancelToken->registerCv(&poolCv_);
  }
  SCOPE_EXIT {
    if (cancelToken) {
      cancelToken->unregisterCv(&poolCv_);
    }
  };
  std::shared_lock lock(poolMutex_);
  poolCv_.wait_for(lock, 1s, [&] {
    if (cancelToken && !cancelToken->get()) {
      return true;
    }
    bool ret = false;
    for (const auto qpId : qpIds) {
      ret = ret || !freePool_.at(qpId).empty();
    }
    return ret;
  });
}

RcBuffer RdmaGenPoolManager::moveFromFreeToPending(uint32_t qpIdx) {
  std::unique_lock lock(poolMutex_);
  auto rcBuf = freePool_[qpIdx].back();
  freePool_[qpIdx].pop_back();
  externalPendingPool_[qpIdx].insert(rcBuf.token);
  return rcBuf;
}

void RdmaGenPoolManager::moveFromPendingToWaiting(
    uint32_t qpIdx,
    uint32_t bufferIdx) {
  std::unique_lock lock(poolMutex_);
  externalPendingPool_[qpIdx].erase(bufferIdx);
  waitingCompletionPool_[qpIdx].insert(bufferIdx);
}

void RdmaGenPoolManager::moveFromWaitingToFree(
    const std::vector<void*>& addrs,
    uint32_t len,
    uint32_t qpIdx,
    uint32_t bufferIdx,
    std::optional<uint32_t> maybeImmediateData) {
  std::unique_lock lock(poolMutex_);
  waitingCompletionPool_[qpIdx].erase(bufferIdx);
  RcBuffer rcBuf = {
      .buf = addrs[bufferIdx],
      .len = len,
      .token = bufferIdx,
      .qp_idx = qpIdx,
      .maybe_imm_data = maybeImmediateData,
  };
  freePool_[qpIdx].push_back(rcBuf);
  poolCv_.notify_all();
}

uint32_t RdmaGenPoolManager::getFreePoolSize() const {
  std::shared_lock lock(poolMutex_);
  return freePool_.size();
}

uint32_t RdmaGenPoolManager::getExternalPendingPoolSize() const {
  std::shared_lock lock(poolMutex_);
  return externalPendingPool_.size();
}

uint32_t RdmaGenPoolManager::getWaitingCompletionPoolSize() const {
  std::shared_lock lock(poolMutex_);
  return waitingCompletionPool_.size();
}

uint32_t RdmaGenPoolManager::getFreePoolSizeForQp(uint32_t qpIdx) const {
  std::shared_lock lock(poolMutex_);
  return freePool_.at(qpIdx).size();
}

uint32_t RdmaGenPoolManager::getExternalPendingPoolSizeForQp(
    uint32_t qpIdx) const {
  std::shared_lock lock(poolMutex_);
  return externalPendingPool_.at(qpIdx).size();
}

uint32_t RdmaGenPoolManager::getWaitingCompletionPoolSizeForQp(
    uint32_t qpIdx) const {
  std::shared_lock lock(poolMutex_);
  return waitingCompletionPool_.at(qpIdx).size();
}

uint32_t RdmaGenPoolManager::getBufTokenInFreePool(uint32_t qpIdx, uint32_t pos)
    const {
  std::shared_lock lock(poolMutex_);
  return freePool_.at(qpIdx).at(pos).token;
}

bool RdmaGenPoolManager::checkInExternalPendingPool(
    uint32_t qpIdx,
    uint32_t bufferIdx) const {
  std::shared_lock lock(poolMutex_);
  return externalPendingPool_.at(qpIdx).count(bufferIdx);
}

bool RdmaGenPoolManager::checkInWaitingCompletionPool(
    uint32_t qpIdx,
    uint32_t bufferIdx) const {
  std::shared_lock lock(poolMutex_);
  return waitingCompletionPool_.at(qpIdx).count(bufferIdx);
}

} // namespace facebook::neteng::ai::rdma_gen
