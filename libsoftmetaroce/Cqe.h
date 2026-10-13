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

#include <infiniband/verbs.h>
#include <cstdint>
#include <optional>

namespace libsoftmetaroce {

enum WorkCompletionStatus {
  kSuccess = 0,
  kLocalLenErr = 1,
  kLocalOpsErr = 2,
  kLocalEecOpsErr = 3,
  kLocalProtErr = 4,
  kWrFlushErr = 5,
  kWrMwBindErr = 6,
  kWrBadRespErr = 7,
  kWrLocalAccErr = 8,
  kWrRemInvReqErr = 9,
  kWrRemAccErr = 10,
  kWrRemOpsErr = 11,
  kWcRetryExcErr = 12,
  kWcRnrRetryExcErr = 13,
  kWcLocRddVioErr = 14,
  kWcRemInvRdReqErr = 15,
  kWcRemAbortErr = 16,
  kWcInvEecnErr = 17,
  kWcInvEecStateErr = 18,
  kWcFatalErr = 19,
  kWcRespTimeoutErr = 20,
  kWcGenErr = 21,
  kWcTmErr = 22,
  kWcTmRndvIncompete = 23,
};

struct Cqe {
  uint64_t workReqId;
  WorkCompletionStatus status;
  std::optional<uint32_t> maybeImmData;
  uint32_t sizeBytes;
  uint32_t qpn;
  uint8_t tag;
  enum ibv_wc_opcode opcode;
};

} // namespace libsoftmetaroce
