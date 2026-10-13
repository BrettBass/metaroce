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

#include <folly/String.h>
#include <glog/logging.h>

#include "rdma_gen/RdmaGenMtu.h"

namespace facebook::neteng::ai::rdma_gen {

uint32_t ibvMtuToBytes(ibv_mtu mtu) {
  switch (mtu) {
    case IBV_MTU_256:
      return 256;
    case IBV_MTU_512:
      return 512;
    case IBV_MTU_1024:
      return 1024;
    case IBV_MTU_2048:
      return 2048;
    case IBV_MTU_4096:
      return 4096;
    case IBV_MTU_8192:
      return 8192;
    default:
      LOG(FATAL) << "Unknown ibv_mtu value: " << static_cast<int>(mtu);
  }
}

// static because only expected use of converting string to ibv_mtu is
// here in parsing the command line argument.
static ibv_mtu ibvMtuFromString(const std::string_view& mtu) {
  // convert both correctly formatted (IBV_MTU_256) and undecorated
  // mtu sizes (256) if we can tell what was desired.
  static const std::unordered_map<std::string_view, ibv_mtu> kIbvMtuMap = {
      {"IBV_MTU_256", IBV_MTU_256},
      {"IBV_MTU_512", IBV_MTU_512},
      {"IBV_MTU_1024", IBV_MTU_1024},
      {"IBV_MTU_2048", IBV_MTU_2048},
      {"IBV_MTU_4096", IBV_MTU_4096},
      {"IBV_MTU_8192", IBV_MTU_8192},
      {"256", IBV_MTU_256},
      {"512", IBV_MTU_512},
      {"1024", IBV_MTU_1024},
      {"2048", IBV_MTU_2048},
      {"4096", IBV_MTU_4096},
      {"8192", IBV_MTU_8192},
  };

  if (!kIbvMtuMap.contains(mtu)) {
    LOG(FATAL) << fmt::format("Unknown MTU: {}", mtu);
  }
  return kIbvMtuMap.at(mtu);
}

/*
  The --mtu flag can be a single value or a comma separated list of per-server
  MTUs. If multiple MTUs are given, the list must have a as many MTUs as there
  are servers. If a single MTU is given, the list will be extended to have the
  expected number of entries. The argument cannot be made empty; it has a
  default defined above, and can be overridden.
*/
std::vector<ibv_mtu> parseMtuFlagList(
    const std::string& mtuFlag,
    size_t expectedMtus) {
  std::vector<ibv_mtu> ret;
  std::vector<std::string_view> mtu_per_server;

  if (mtuFlag.empty()) {
    LOG(ERROR) << "--mtu flag may not be empty.";
    exit(1);
  }

  folly::split(',', mtuFlag, mtu_per_server);

  ret.reserve(mtu_per_server.size());
  for (auto& str : mtu_per_server) {
    ret.push_back(ibvMtuFromString(str));
  }

  if (ret.size() > 1 && (expectedMtus != ret.size())) {
    LOG(ERROR)
        << "Expected " << expectedMtus
        << " comma-separated MTUs to be specified by the --mtu argument, but found "
        << ret.size();
    exit(1);
  } else if (ret.size() == 1 && expectedMtus > 1) {
    ret.resize(expectedMtus, ret[0]);
  }

  return ret;
}

ibv_mtu parseMtuFlagSingle(const std::string& mtuFlag) {
  auto mtus = parseMtuFlagList(mtuFlag, 1);
  CHECK(!mtus.empty()) << "mtu flag is not permitted to be empty";
  return mtus[0];
}

const std::string ibvMtuToString(const ibv_mtu mtu) {
  const std::map<ibv_mtu, std::string> mtuToStr = {
      {IBV_MTU_256, "IBV_MTU_256"},
      {IBV_MTU_512, "IBV_MTU_512"},
      {IBV_MTU_1024, "IBV_MTU_1024"},
      {IBV_MTU_2048, "IBV_MTU_2048"},
      {IBV_MTU_4096, "IBV_MTU_4096"},
      {IBV_MTU_8192, "IBV_MTU_8192"},
  };
  if (mtuToStr.contains(mtu)) {
    return mtuToStr.at(mtu);
  } else {
    return "UNKNOWN";
  }
}

thrift::Mtu toThriftMtu(enum ibv_mtu ibvMtu) {
  switch (ibvMtu) {
    case IBV_MTU_256:
      return thrift::Mtu::MTU_256;
    case IBV_MTU_512:
      return thrift::Mtu::MTU_512;
    case IBV_MTU_1024:
      return thrift::Mtu::MTU_1024;
    case IBV_MTU_2048:
      return thrift::Mtu::MTU_2048;
    case IBV_MTU_4096:
      return thrift::Mtu::MTU_4096;
    case IBV_MTU_8192:
      return thrift::Mtu::MTU_8192;
    default:
      LOG(ERROR) << "Unknown IBV MTU value " << ibvMtu;
      return thrift::Mtu::UNKNOWN;
  }
}

enum ibv_mtu toIbvMtu(thrift::Mtu mtu) {
  switch (mtu) {
    case thrift::Mtu::MTU_256:
      return IBV_MTU_256;
    case thrift::Mtu::MTU_512:
      return IBV_MTU_512;
    case thrift::Mtu::MTU_1024:
      return IBV_MTU_1024;
    case thrift::Mtu::MTU_2048:
      return IBV_MTU_2048;
    case thrift::Mtu::MTU_4096:
      return IBV_MTU_4096;
    case thrift::Mtu::MTU_8192:
      return IBV_MTU_8192;
    case thrift::Mtu::UNKNOWN:
      [[fallthrough]];
    default:
      LOG(ERROR) << "Unknown MTU value " << static_cast<int>(mtu);
      return (enum ibv_mtu)0;
  }
}

} // namespace facebook::neteng::ai::rdma_gen
