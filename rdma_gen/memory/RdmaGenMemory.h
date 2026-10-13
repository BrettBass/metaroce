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

#include "rdma_gen/Common.h"

namespace facebook::neteng::ai::rdma_gen {

// Base class to be inherited by specific memory types (cpu, gpu, device)
// to populate the memory buffer vector in the RcQpContext
class RdmaGenMemory {
 public:
  RdmaGenMemory(
      const Params& params,
      RcQpContext* ctx,
      unsigned int accessFlags)
      : params_(params), ctx_(ctx), accessFlags_(accessFlags) {}
  RdmaGenMemory(const RdmaGenMemory&) = delete;
  RdmaGenMemory(RdmaGenMemory&&) = delete;
  RdmaGenMemory& operator=(RdmaGenMemory&& other) = delete;
  RdmaGenMemory operator=(const RdmaGenMemory& other) = delete;
  virtual ~RdmaGenMemory() {}
  virtual void setUpBuffers(int bufferCap) {
    LOG(INFO) << fmt::format(
        "{}: max num of registered buffers: {}", __func__, bufferCap);
    pushMem(bufferCap);
    regMem(bufferCap);
  }
  virtual std::vector<void*>& getBuffers() = 0;
  virtual void* getBuffer(int bufIdx) {
    return getBuffers().at(bufIdx);
  }

 protected:
  // invokes pushBlock or pushBuffers based on params
  void pushMem(int bufferCap);
  // invokes regBlock or regBuffers based on params
  void regMem(int bufferCap);
  // the total allocation necessary for all qp_cap qps and message sizes.
  unsigned long getLen(int buffer_cap) const;
  // allocates a getLen sized block, subdividing it into buffer addresses in
  // the buffers vector.
  virtual void pushBlock(int bufferCap) = 0;
  // uses memalign to allocate qp * buffer_cap individual message_size buffers
  virtual void pushBuffers(int bufferCap) = 0;
  // registers the pushBlock allocation as a single-entry mr_list
  virtual void regBlock(int bufferCap) = 0;
  // registers each of the buffers allocated by pushBuffers
  virtual void regBuffers(int bufferCap) = 0;
  RcQpContext* ctx() {
    return ctx_;
  }
  IRdmaVerbs* verbs() {
    CHECK_NOTNULL(ctx());
    return ctx()->transport_provider.getVerbs();
  }

  Params params_;
  RcQpContext* ctx_;
  unsigned int accessFlags_;
};

} // namespace facebook::neteng::ai::rdma_gen
