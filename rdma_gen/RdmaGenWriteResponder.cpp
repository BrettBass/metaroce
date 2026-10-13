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

#include "rdma_gen/RdmaGenWriteResponder.h"

namespace facebook::neteng::ai::rdma_gen {

RdmaGenWriteResponder::RdmaGenWriteResponder(
    const Params& params,
    int terminateThreshold,
    const std::string& jsonPath)
    : RdmaGenResponder(params, terminateThreshold, jsonPath) {
  // As a testing tool, write responder only supports all requesters
  // writing to the same memory region for now, so rx_depth is 1.
  CHECK_EQ(params.rx_depth, 1)
      << "rx_depth larger than 1 is not supported for now";
  CHECK(params.role == EndpointRole::WriteResponder)
      << "write responder role is not set correctly";
}

RcBuffer RdmaGenWriteResponder::fetchDataReadyBuffer(int idx) {
  waitForStop();
  return {
      endpoint_->getBuffer(idx),
      params().message_size[params().baseQpIndex(idx)],
      static_cast<uint32_t>(idx),
      0};
}

} // namespace facebook::neteng::ai::rdma_gen
