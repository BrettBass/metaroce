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

#include "rdma_gen/memory/RdmaGenMemory.h"

namespace facebook::neteng::ai::rdma_gen {

unsigned long RdmaGenMemory::getLen(int buffer_cap) const {
  unsigned long len = 0;
  CHECK_EQ(params_.message_size.size(), params_.qp_cap);
  const auto numEffectiveQps = params_.effectiveQpCap();
  for (uint32_t eqpIdx = 0; eqpIdx < numEffectiveQps; eqpIdx++) {
    len += params_.message_size[params_.baseQpIndex(eqpIdx)] * buffer_cap;
  }

  return len;
}

void RdmaGenMemory::pushMem(int buffer_cap) {
  if (params_.use_block) {
    pushBlock(buffer_cap);
  } else {
    pushBuffers(buffer_cap);
  }
}

void RdmaGenMemory::regMem(int buffer_cap) {
  if (params_.use_block) {
    regBlock(buffer_cap);
  } else {
    regBuffers(buffer_cap);
  }
}

} // namespace facebook::neteng::ai::rdma_gen
