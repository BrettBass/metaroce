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

#include "rdma_gen/memory/RocmMemoryRegistration.h"

#include "rdma_gen/memory/RdmaGenMemoryFactory.h"

#ifndef NO_ROCM

#include "rdma_gen/memory/RocmMemory.h"

namespace facebook::neteng::ai::rdma_gen {

void registerRocmMemoryCreator(RdmaGenMemoryFactory& factory) {
  factory.registerMemoryCreator(
      RdmaMode::RocmRdma,
      [](Params& p,
         RcQpContext* ctx,
         int accessFlags,
         std::unique_ptr<CudaWrapper>) {
        return std::make_unique<RocmMemory>(p, ctx, accessFlags);
      });
}

} // namespace facebook::neteng::ai::rdma_gen

#else // NO_ROCM

namespace facebook::neteng::ai::rdma_gen {

void registerRocmMemoryCreator(RdmaGenMemoryFactory& /* factory */) {
  // No-op: ROCm memory support not compiled
}

} // namespace facebook::neteng::ai::rdma_gen

#endif // NO_ROCM
