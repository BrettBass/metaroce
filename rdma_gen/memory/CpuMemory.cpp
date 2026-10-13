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

#include "rdma_gen/memory/CpuMemory.h"

namespace facebook::neteng::ai::rdma_gen {
void CpuMemory::pushBlock(int buffer_cap) {
  auto len = getLen(buffer_cap);
  auto b = calloc(sizeof(char), len);
  unsigned long base_offset = 0;
  const auto numEffectiveQps = params_.effectiveQpCap();
  for (uint32_t eqpIdx = 0; eqpIdx < numEffectiveQps; eqpIdx++) {
    const auto msg_size = params_.message_size[params_.baseQpIndex(eqpIdx)];
    for (int j = 0; j < buffer_cap; j++) {
      auto c = (char*)b;
      ctx()->buffers.push_back(c + base_offset + (j * msg_size));
    }
    base_offset += buffer_cap * msg_size;
  }
  ctx()->freeBuffers = [](RcQpContext* ctx) { free(ctx->buffers[0]); };
}

char* FOLLY_NULLABLE CpuMemory::allocateMemory(size_t length) {
  // length is a per-message buffer size and can exceed 2 GiB, so it must be
  // wider than 32-bit: an int argument truncates message_size >= 2^31 to a
  // negative value, which memalign then reinterprets as a huge size_t and
  // fails to allocate.
  char* d_A = (char*)memalign(kPageSize, length);
  if (!d_A) {
    LOG(ERROR) << "Failed to allocate memory";
    return nullptr;
  }

  if (kPageSize == kLargePageSize) {
    if (madvise(d_A, length, MADV_HUGEPAGE) == -1) {
      LOG(WARNING) << fmt::format("Failed to madvise hugepage for {}", d_A);
    }
  }

  return d_A;
}

void CpuMemory::pushBuffers(int buffer_cap) {
  const auto numEffectiveQps = params_.effectiveQpCap();
  for (uint32_t eqpIdx = 0; eqpIdx < numEffectiveQps; eqpIdx++) {
    for (int j = 0; j < buffer_cap; j++) {
      const auto len = params_.message_size[params_.baseQpIndex(eqpIdx)];
      auto b = allocateMemory(len);
      CHECK(b) << "Failed to allocate memory buffer of size " << len
               << " for qp " << eqpIdx << " buffer " << j;
      ctx()->buffers.push_back(b);
    }
  }
  ctx()->freeBuffers = [](RcQpContext* ctx) {
    for (auto d_A : ctx->buffers) {
      free(d_A);
    }
  };
}

void CpuMemory::regBlock(int buffer_cap) {
  auto len = getLen(buffer_cap);

  auto mr =
      ctx()->pd_adapter->registerMemory(ctx()->buffers[0], len, accessFlags_);
  ctx()->mr_list.emplace_back(std::move(mr));
}

void CpuMemory::regBuffers(int buffer_cap) {
  const auto numEffectiveQps = params_.effectiveQpCap();
  for (uint32_t eqpIdx = 0; eqpIdx < numEffectiveQps; eqpIdx++) {
    for (int j = 0; j < buffer_cap; j++) {
      auto idx = eqpIdx * buffer_cap + j;
      auto mr = ctx()->pd_adapter->registerMemory(
          ctx()->buffers[idx],
          params_.message_size[params_.baseQpIndex(eqpIdx)],
          accessFlags_);
      ctx()->mr_list.emplace_back(std::move(mr));
    }
  }
}
} // namespace facebook::neteng::ai::rdma_gen
