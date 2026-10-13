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

#include <sys/mman.h>
#include "rdma_gen/memory/RdmaGenMemory.h"

namespace facebook::neteng::ai::rdma_gen {

constexpr int kLargePageSize = 2 * 1024 * 1024; // 2MB Page Size

// rdma_mode=cpu (default) memory allocation
class CpuMemory : public RdmaGenMemory {
 public:
  CpuMemory(const Params& params, RcQpContext* ctx, unsigned int accessFlags)
      : RdmaGenMemory(params, ctx, accessFlags),
        kPageSize(getPageSize(params)) {}
  std::vector<void*>& getBuffers() override {
    return ctx()->buffers;
  }

 protected:
  void pushBlock(int bufferCap) override;
  void pushBuffers(int bufferCap) override;
  void regBlock(int bufferCap) override;
  void regBuffers(int bufferCap) override;
  const int kPageSize;

 private:
  char* allocateMemory(size_t length);
  int getPageSize(const Params& params) const {
    return (params.use_huge_pages) ? kLargePageSize
                                   : (int)sysconf(_SC_PAGESIZE);
  }
};
} // namespace facebook::neteng::ai::rdma_gen
