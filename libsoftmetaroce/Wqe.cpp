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

#include <arpa/inet.h>
#include <gflags/gflags.h>
#include <algorithm>

#include "libsoftmetaroce/Wqe.h"

DEFINE_bool(
    libsoftmetaroce_audit_posn_state,
    false,
    "Hold each posn's state in full beside a work request's counts and check "
    "the two against each other. Costs a byte per packet of every message.");

namespace libsoftmetaroce {

bool posnAuditingEnabled() {
  return FLAGS_libsoftmetaroce_audit_posn_state;
}

std::ostream& operator<<(std::ostream& os, const Wqe::WorkType& workType) {
  os << fmt::format("{}", workType);
  return os;
}

std::ostream& operator<<(std::ostream& os, const Wqe& wqe) {
  os << fmt::format("type: {}", wqe.workType_);
  if (wqe.getImmediateData()) {
    os << ", immediateData: " << ntohl(wqe.getImmediateData().value());
  }
  if (wqe.getMsn()) {
    os << ", msn: " << wqe.getMsn().value();
  }
  if (wqe.getCsn()) {
    os << ", csn: " << wqe.getCsn().value();
  }
  if (wqe.hasFenceFlag()) {
    os << ", fence: true";
  }
  if (const auto statusOverride = wqe.getCompletionStatusOverride()) {
    os << ", status: " << (int)*statusOverride;
  }
  os << ", size: " << wqe.getMessageSizeBytes() << "B";
  os << ", outstanding: " << wqe.outstandingPosnCount_;
  os << " of numPosn: " << wqe.numPosn_;
  return os;
}

} // namespace libsoftmetaroce
