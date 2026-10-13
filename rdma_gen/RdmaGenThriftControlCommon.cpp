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

#include "rdma_gen/RdmaGenThriftControlCommon.h"

namespace facebook::neteng::ai::rdma_gen {
namespace RdmaGenThriftConversions {

thrift::RcQpInfo convertToThriftQpInfo(const RcQpInfo& qpInfo) {
  thrift::RcQpInfo thriftQpInfo;
  thriftQpInfo.lid() = qpInfo.lid;
  thriftQpInfo.qpn() = qpInfo.qpn;
  thriftQpInfo.psn() = qpInfo.psn;

  // Convert vector of ibv_gid to list of GlobalId
  std::vector<thrift::GlobalId> thriftGids;
  thriftGids.reserve(qpInfo.gids.size());
  for (const auto& gid : qpInfo.gids) {
    thrift::GlobalId thriftGid;
    thriftGid.subnetPrefix() = gid.global.subnet_prefix;
    thriftGid.interfaceId() = gid.global.interface_id;
    thriftGids.push_back(std::move(thriftGid));
  }
  thriftQpInfo.gids() = std::move(thriftGids);

  if (qpInfo.eroceQpCfg.has_value()) {
    thriftQpInfo.eroceQpCfg().ensure().eroce_hw_cfg() =
        qpInfo.eroceQpCfg->hw_cfg;
  }

  return thriftQpInfo;
}

RcQpInfo convertFromThriftQpInfo(const thrift::RcQpInfo& qpInfo) {
  RcQpInfo nativeQpInfo;
  nativeQpInfo.lid = *qpInfo.lid();
  nativeQpInfo.qpn = *qpInfo.qpn();
  nativeQpInfo.psn = *qpInfo.psn();

  // Convert list of GlobalId to vector of ibv_gid
  nativeQpInfo.gids.reserve(qpInfo.gids()->size());
  for (const auto& thriftGid : *qpInfo.gids()) {
    union ibv_gid gid{};
    gid.global.subnet_prefix = *thriftGid.subnetPrefix();
    gid.global.interface_id = *thriftGid.interfaceId();
    nativeQpInfo.gids.push_back(gid);
  }

  if (auto eroceQpCfg = qpInfo.eroceQpCfg(); eroceQpCfg.has_value()) {
    nativeQpInfo.eroceQpCfg = EroceQpCfg{.hw_cfg = *eroceQpCfg->eroce_hw_cfg()};
  }

  return nativeQpInfo;
}

} // namespace RdmaGenThriftConversions
} // namespace facebook::neteng::ai::rdma_gen
