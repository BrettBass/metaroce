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

#include <libsoftmetaroce/GidDiscovery.h>

#include <glog/logging.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <libsoftmetaroce/Constants.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <algorithm>
#include <cstring>
#include <utility>

namespace libsoftmetaroce {

namespace {

// Remove the embedded IPv6 scope; DiscoveredAddress stores it separately.
folly::IPAddress scopelessAddress(const folly::IPAddress& address) {
  if (!address.isV6()) {
    return address;
  }
  auto scopeless = address.asV6();
  scopeless.setScopeId(0);
  return folly::IPAddress(scopeless);
}

bool isMetaRoceEthernetGidCandidate(
    const DiscoveredAddress& candidate,
    const ScanConditions& conditions) {
  return candidate.scopelessAddress.isV6() &&
      candidate.scopelessAddress.asV6().isLinkLocal() &&
      candidate.scopeId != 0 && conditions.explicitGidsSelected &&
      conditions.hasRawCapability;
}

std::optional<uint32_t> classifyMetaRoceEthernet(
    const DiscoveredAddress& candidate,
    const ScanConditions& conditions) {
  if (isMetaRoceEthernetGidCandidate(candidate, conditions) &&
      candidate.interfaceIndex != 0) {
    return kGidTypeMetaRoceEthernet;
  }
  return std::nullopt;
}

std::optional<uint32_t> classifyMetaRoceUdp(
    const DiscoveredAddress& candidate) {
  if (candidate.scopelessAddress.isV4() ||
      (candidate.scopelessAddress.isV6() && candidate.scopeId == 0)) {
    return kGidTypeMetaRoceUdp;
  }
  return std::nullopt;
}

} // namespace

std::optional<DiscoveredAddress> makeDiscoveredAddress(
    const struct ifaddrs& entry) {
  if (entry.ifa_addr == nullptr) {
    return std::nullopt;
  }
  const auto family = entry.ifa_addr->sa_family;
  if (family != AF_INET && family != AF_INET6) {
    return std::nullopt;
  }
  const auto interfaceIndex = if_nametoindex(entry.ifa_name);
  if (interfaceIndex == 0) {
    LOG(WARNING) << "if_nametoindex failed for " << entry.ifa_name
                 << "; the interface went away during discovery";
    return std::nullopt;
  }
  // folly::IPAddress(const sockaddr*) throws on a null pointer or an
  // unsupported family, both excluded above.
  return DiscoveredAddress{
      .scopelessAddress = scopelessAddress(folly::IPAddress(entry.ifa_addr)),
      .interfaceName = entry.ifa_name,
      .interfaceIndex = interfaceIndex,
      .scopeId = family == AF_INET6
          ? reinterpret_cast<const sockaddr_in6*>(entry.ifa_addr)->sin6_scope_id
          : 0,
      .interfaceUp = (entry.ifa_flags & IFF_UP) != 0,
  };
}

void GidRuleRegistry::registerRule(GidRule rule) {
  CHECK(rule) << "a registered GID rule must not be empty";
  rules_.wlock()->push_back(std::move(rule));
}

GidRuleChain GidRuleRegistry::makeChain(
    const ScanConditions& conditions) const {
  std::vector<GidRule> rules;
  rules.emplace_back([conditions](const DiscoveredAddress& candidate) {
    return classifyMetaRoceEthernet(candidate, conditions);
  });
  rules.emplace_back(classifyMetaRoceUdp);
  const size_t builtInCount = rules.size();
  {
    const auto registered = rules_.rlock();
    rules.insert(rules.end(), registered->begin(), registered->end());
  }
  return GidRuleChain(std::move(rules), builtInCount);
}

GidRuleRegistry& defaultGidRuleRegistry() {
  static GidRuleRegistry registry;
  return registry;
}

GidRuleChain::GidRuleChain(std::vector<GidRule> rules, size_t builtInCount)
    : rules_(std::move(rules)), builtInCount_(builtInCount) {
  CHECK_LE(builtInCount_, rules_.size());
}

GidClaims GidRuleChain::classify(const DiscoveredAddress& candidate) const {
  GidClaims claims;
  for (size_t i = 0; i < rules_.size(); ++i) {
    if (auto gidType = rules_[i](candidate)) {
      auto& group = i < builtInCount_ ? claims.builtIn : claims.registered;
      group.push_back(*gidType);
    }
  }
  return claims;
}

void recordGid(
    const DiscoveredAddress& candidate,
    uint32_t gidType,
    bool requestedByUser,
    bool rawFramedGid,
    bool fromBuiltInRule,
    DiscoveryTables& tables) {
  const auto entry = buildGidEntry(candidate, gidType);
  // Key discovery by the bare address; requested entries must use the same
  // form.
  const auto addressString = candidate.scopelessAddress.str();
  const auto alreadyRecorded = [&] {
    const auto it = tables.discovered.find(addressString);
    return it != tables.discovered.end() &&
        std::any_of(
               it->second.begin(),
               it->second.end(),
               [gidType](const auto& gid) {
                 return gid.entry.gid_type == gidType;
               });
  };
  if (rawFramedGid) {
    LOG(INFO) << "raw-framed GID " << addressString << " (gid_type " << gidType
              << ") on " << candidate.interfaceName;
    if (!alreadyRecorded()) {
      auto& gids = tables.discovered[addressString];
      const bool firstForAddress = gids.empty();
      gids.push_back({entry, fromBuiltInRule});
      if (firstForAddress && !fromBuiltInRule) {
        tables.discoveredOrder.push_back(addressString);
      }
    }
    return;
  }

  tables.all.try_emplace(addressString, entry);
  if (candidate.interfaceUp && requestedByUser && !alreadyRecorded()) {
    auto& gids = tables.discovered[addressString];
    const bool firstForAddress = gids.empty();
    gids.push_back({entry, fromBuiltInRule});
    if (firstForAddress) {
      tables.discoveredOrder.push_back(addressString);
    }
  }
  tables.byInterfaceName.try_emplace(candidate.interfaceName, entry);
  tables.interfaceNameByAddress.try_emplace(
      addressString, candidate.interfaceName);
}

ibv_gid_entry buildGidEntry(
    const DiscoveredAddress& candidate,
    uint32_t gidType) {
  ibv_gid_entry entry{};
  const auto address = candidate.scopelessAddress.isV4()
      ? folly::IPAddress(candidate.scopelessAddress.asV4().createIPv6())
      : candidate.scopelessAddress;
  const auto bytes = address.asV6().toByteArray();
  std::memcpy(entry.gid.raw, bytes.data(), bytes.size());
  entry.port_num = 1;
  entry.gid_type = gidType;
  entry.ndev_ifindex = candidate.interfaceIndex;
  return entry;
}

std::vector<ibv_gid_entry> orderGidTable(
    const std::vector<std::string>& addressOrder,
    const std::unordered_map<std::string, std::vector<DiscoveredGid>>&
        discovered) {
  std::vector<ibv_gid_entry> table;
  // Extension types are buffered rather than seated as they are seen: taking an
  // index the moment one appears would push every later address down by one.
  std::vector<ibv_gid_entry> extensionGids;
  const auto seat = [&table](ibv_gid_entry entry) {
    entry.gid_index = static_cast<uint32_t>(table.size());
    table.push_back(entry);
  };
  for (const auto& address : addressOrder) {
    const auto it = discovered.find(address);
    if (it == discovered.end()) {
      LOG(WARNING) << "Requested libsoftmetaroce_gid '" << address
                   << "' was not found among local IP addresses; skipping";
      continue;
    }
    for (const auto& gid : it->second) {
      if (gid.fromBuiltInRule) {
        seat(gid.entry);
      } else {
        extensionGids.push_back(gid.entry);
      }
    }
  }
  for (const auto& entry : extensionGids) {
    seat(entry);
  }
  return table;
}

} // namespace libsoftmetaroce
