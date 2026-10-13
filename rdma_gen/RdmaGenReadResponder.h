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

#include "rdma_gen/RdmaGenResponder.h"

namespace facebook::neteng::ai::rdma_gen {

// Represents a RC read responder.
// Usage:
//     rc::Params params = ...;
//     RcReadResponder responder(params)
//     responder.init(...);
//     for (int i = 0; i < params.qp_cap; i++) {
//       auto rcBuf = responder.fetchDataBuffer(i);
//       ...modify rcBuf as you like...
//     }
//     responder.waitForStop();
//     responder.stop();
class RdmaGenReadResponder : public RdmaGenResponder {
 public:
  explicit RdmaGenReadResponder(
      const Params& params,
      int terminateThreshold = 1);

  virtual ~RdmaGenReadResponder() = default;

  // Fetches a RcBuffer that is to be read by given client idx.
  RcBuffer fetchDataBuffer(int idx);
};

} // namespace facebook::neteng::ai::rdma_gen
