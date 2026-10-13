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

#include "rdma_gen/Common.h"

namespace facebook::neteng::ai::rdma_gen {

std::string formatGids(const std::vector<union ibv_gid>& gids) {
  if (gids.empty()) {
    return "(no gids)";
  }
  // IPv4-mapped IPv6 prefix: ::ffff:0:0/96 (first 12 bytes)
  static const uint8_t v4MappedPrefix[12] = {
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};

  std::string result;
  for (size_t i = 0; i < gids.size(); ++i) {
    if (i > 0) {
      result += ", ";
    }
    // Check if this is an IPv4-mapped IPv6 address
    if (memcmp(gids[i].raw, v4MappedPrefix, sizeof(v4MappedPrefix)) == 0) {
      // Extract the IPv4 address from the last 4 bytes
      char ipv4Str[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &gids[i].raw[12], ipv4Str, sizeof(ipv4Str));
      result += ipv4Str;
    } else {
      // Format as IPv6 address
      char ipv6Str[INET6_ADDRSTRLEN];
      inet_ntop(AF_INET6, &gids[i], ipv6Str, sizeof(ipv6Str));
      result += ipv6Str;
    }
  }
  return result;
}

RcQpContext::~RcQpContext() {
  // Peer dtor frees the undelying QP resources
  peer_list.clear();
  cq_adapter = nullptr;

  mr_list.clear();

  pd_adapter = nullptr;
  if (channel) {
    transport_provider.getVerbs()->ibv_destroy_comp_channel(channel);
  }
  ctx_adapter = nullptr;
  dev_adapter = nullptr;

  if (freeBuffers) {
    freeBuffers(this);
  }
}
} // namespace  facebook::neteng::ai::rdma_gen
