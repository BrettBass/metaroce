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

#include <folly/json/dynamic.h>
#include <cstdint>
#include <optional>
#include <vector>

#include "rdma_gen/transport/ITransportProvider.h"

namespace facebook::neteng::ai::rdma_gen {

enum class OperationType;

struct CqeLogEntry {
  WorkCompletionAdapter cqe{};
  int64_t timestamp{0};
  std::optional<uint32_t> maybeImmData;
  std::optional<uint32_t> maybeSendFlags;
};

// Convert a vector of CqeLogEntry to a folly::dynamic array for JSON output
folly::dynamic cqeLogToDynamic(
    const std::vector<CqeLogEntry>& cqeLog,
    OperationType opType);

} // namespace facebook::neteng::ai::rdma_gen
