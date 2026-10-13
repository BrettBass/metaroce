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

namespace facebook::neteng::ai::rdma_gen {

// rdma_mode=gpu (requires cuda) memory allocation
class GpuMemory : public RdmaGenMemory {
 public:
  GpuMemory(
      const Params& params,
      RcQpContext* ctx,
      unsigned int accessFlags,
      std::unique_ptr<CudaWrapper> cudaWrapper)
      : RdmaGenMemory(params, ctx, accessFlags),
        cudaWrapper_(std::move(cudaWrapper)) {
    initGpu();
  }
  std::vector<void*>& getBuffers() override {
    return ctx()->gpu_buffers;
  }
  CudaWrapper* cuda() {
    return cudaWrapper_.get();
  }

 protected:
  void pushBlock(int bufferCap) override;
  void pushBuffers(int bufferCap) override;
  void regBlock(int bufferCap) override;
  void regBuffers(int bufferCap) override;

 private:
  void initGpu();
  std::unique_ptr<CudaWrapper> cudaWrapper_;
#ifndef NO_CUDA
  CUdevice cuDevice_{};
#endif
};
} // namespace facebook::neteng::ai::rdma_gen
