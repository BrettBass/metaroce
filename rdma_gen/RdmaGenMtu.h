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
#include <map>
#include <vector>

#include <fmt/core.h>
#include <fmt/format.h>
#include <string_view>
#include "rdma_gen/if/gen-cpp2/RdmaInfo_types.h"

// IBV_MTU_8192 is not defined in standard rdma-core headers (only up to 4096).
#ifndef IBV_MTU_8192
constexpr ibv_mtu IBV_MTU_8192 = static_cast<ibv_mtu>(6);
#endif

namespace facebook::neteng::ai::rdma_gen {

uint32_t ibvMtuToBytes(ibv_mtu mtu);

std::vector<ibv_mtu> parseMtuFlagList(
    const std::string& mtuFlag,
    size_t expectedMtus);
ibv_mtu parseMtuFlagSingle(const std::string& mtuFlag);
const std::string ibvMtuToString(ibv_mtu mtu);
thrift::Mtu toThriftMtu(enum ibv_mtu ibvMtu);
enum ibv_mtu toIbvMtu(thrift::Mtu mtu);

} // namespace facebook::neteng::ai::rdma_gen

template <>
struct fmt::formatter<ibv_mtu> : fmt::formatter<std::string_view> {
  template <typename FormatContext>
  auto format(ibv_mtu m, FormatContext& ctx) const {
    return fmt::formatter<string_view>::format(
        facebook::neteng::ai::rdma_gen::ibvMtuToString(m), ctx);
  }
};
