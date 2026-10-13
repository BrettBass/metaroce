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

#include <vector>

#include "rdma_gen/memory/RdmaGenMemory.h"

#ifndef NO_ROCM
#include <hip/hip_runtime.h>
#include <hip/hip_runtime_api.h>
#endif

namespace facebook::neteng::ai::rdma_gen {

// rdma_mode=rocm (requires ROCm/HIP) memory allocation for AMD GPUs
// Uses HIP Virtual Memory APIs for dmabuf-exportable allocations
// for GPU-Direct RDMA with NICs that support ibv_reg_dmabuf_mr
class RocmMemory : public RdmaGenMemory {
 public:
  RocmMemory(const Params& params, RcQpContext* ctx, unsigned int accessFlags)
      : RdmaGenMemory(params, ctx, accessFlags) {
    initRocm();
  }

  ~RocmMemory() override;

  std::vector<void*>& getBuffers() override {
    return ctx()->gpu_buffers;
  }

 protected:
  void pushBlock(int bufferCap) override;
  void pushBuffers(int bufferCap) override;
  void regBlock(int bufferCap) override;
  void regBuffers(int bufferCap) override;

 private:
  void initRocm();

#ifndef NO_ROCM
  // Allocate GPU memory using HIP Virtual Memory APIs (dmabuf-exportable)
  // Returns the allocated pointer and stores the handle for later cleanup
  void* allocateVMMemory(size_t size, hipMemGenericAllocationHandle_t& handle);

  // Export GPU memory allocation handle as dmabuf file descriptor
  // Returns the dmabuf fd, or -1 on failure
  int exportHandleAsDmabuf(hipMemGenericAllocationHandle_t handle);

  // Clean up a single VM allocation
  void cleanupVMAllocation(
      void* ptr,
      size_t size,
      hipMemGenericAllocationHandle_t handle);

  int hipDeviceId_{0};
  size_t granularity_{0};
  // Store dmabuf file descriptors for cleanup
  std::vector<int> dmabufFds_;
  // Store VM allocation handles for cleanup
  std::vector<hipMemGenericAllocationHandle_t> vmHandles_;
  // Store VM allocation pointers for cleanup
  std::vector<void*> vmPtrs_;
  // Store VM allocation sizes for cleanup
  std::vector<size_t> vmSizes_;
#endif
};

} // namespace facebook::neteng::ai::rdma_gen
