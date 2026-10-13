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

#include "rdma_gen/RdmaGenReadResponder.h"

namespace facebook::neteng::ai::rdma_gen {

RdmaGenReadResponder::RdmaGenReadResponder(
    const Params& params,
    int terminateThreshold)
    : RdmaGenResponder(params, terminateThreshold) {
  // As a testing tool, read responder only supports all requesters
  // reading from the same memory region for now, so rx_depth is 1.
  CHECK_EQ(params.rx_depth, 1)
      << "rx_depth larger than 1 is not supported for now";
  CHECK(
      params.role == EndpointRole::ReadResponder ||
      params.role == EndpointRole::AtomicResponder)
      << "read/atomic responder role is not set correctly";
}

RcBuffer RdmaGenReadResponder::fetchDataBuffer(int idx) {
  return {
      .buf = endpoint_->getBuffer(idx),
      .len = params().message_size[idx],
      .token = static_cast<uint32_t>(idx),
      .qp_idx = 0};
}

} // namespace facebook::neteng::ai::rdma_gen
