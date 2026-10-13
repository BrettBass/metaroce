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
#include "rdma_gen/memory/RdmaGenMemory.h"
#include "rdma_gen/memory/RdmaGenRdmaMode.h"

namespace facebook::neteng::ai::rdma_gen {

class RdmaGenMemoryFactory {
  using MemoryCreator = std::function<std::unique_ptr<RdmaGenMemory>(
      Params& p,
      RcQpContext* ctx,
      int accessFlags,
      std::unique_ptr<CudaWrapper>)>;

 public:
  RdmaGenMemoryFactory() = default;
  ~RdmaGenMemoryFactory() {}

  static RdmaGenMemoryFactory& getInstance();

  void registerMemoryCreator(
      const RdmaMode::Value rdma_mode,
      const MemoryCreator& creator) {
    creators_[rdma_mode] = creator;
  }

  std::unique_ptr<RdmaGenMemory> createMemory(
      const RdmaMode::Value rdma_mode,
      Params& p,
      RcQpContext* ctx,
      int accessFlags,
      std::unique_ptr<CudaWrapper> cudaWrapper) {
    if (creators_.contains(rdma_mode)) {
      return creators_[rdma_mode](p, ctx, accessFlags, std::move(cudaWrapper));
    }
    LOG(FATAL) << "unsupported rdma_mode";
  }

 private:
  RdmaGenMemoryFactory(const RdmaGenMemoryFactory&) = delete;
  RdmaGenMemoryFactory(RdmaGenMemoryFactory&&) = delete;
  RdmaGenMemoryFactory& operator=(const RdmaGenMemoryFactory&) = delete;
  RdmaGenMemoryFactory& operator=(RdmaGenMemoryFactory&&) = delete;

  std::map<RdmaMode::Value, MemoryCreator> creators_;
};
} // namespace facebook::neteng::ai::rdma_gen
