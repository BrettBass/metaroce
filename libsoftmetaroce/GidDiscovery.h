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

#include <folly/IPAddress.h>
#include <folly/SharedMutex.h>
#include <folly/Synchronized.h>
#include <infiniband/verbs.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct ifaddrs;

namespace libsoftmetaroce {

// What discovery found about one address.
struct DiscoveredAddress {
  folly::IPAddress scopelessAddress;
  std::string interfaceName;
  uint32_t interfaceIndex{};
  uint32_t scopeId{};
  bool interfaceUp{};
};

// Decodes one getifaddrs entry. Returns nullopt for an entry with no address, a
// family that is neither IPv4 nor IPv6, or an interface name that no longer
// resolves to an index.
[[nodiscard]] std::optional<DiscoveredAddress> makeDiscoveredAddress(
    const struct ifaddrs& entry);

// What the process and configuration permit, fixed for one discovery scan.
struct ScanConditions {
  // Link-locals exist on every interface, so the built-in rule only fires when
  // the operator selected addresses explicitly. It does not mean this
  // candidate is one of them.
  bool explicitGidsSelected{};
  // AF_PACKET needs CAP_NET_RAW.
  bool hasRawCapability{};
};

using GidRule =
    std::function<std::optional<uint32_t>(const DiscoveredAddress&)>;

// Claims are separated so registered types can be appended without changing
// built-in GID indexes.
struct GidClaims {
  std::vector<uint32_t> builtIn;
  std::vector<uint32_t> registered;

  [[nodiscard]] bool empty() const {
    return builtIn.empty() && registered.empty();
  }
};

class GidRuleChain {
 public:
  // builtInCount rules from the front are the built-in defaults; the rest came
  // from the registry.
  GidRuleChain(std::vector<GidRule> rules, size_t builtInCount);

  // Runs every rule. A rule declining a candidate says nothing about the
  // others, so a candidate several rules claim yields several gid_types.
  [[nodiscard]] GidClaims classify(const DiscoveredAddress& candidate) const;

 private:
  std::vector<GidRule> rules_;
  size_t builtInCount_{};
};

class GidRuleRegistry {
 public:
  void registerRule(GidRule rule);

  // Returns a snapshot with built-in rules followed by registered rules.
  [[nodiscard]] GidRuleChain makeChain(const ScanConditions& conditions) const;

 private:
  folly::Synchronized<std::vector<GidRule>, folly::SharedMutex> rules_;
};

// Returns the process-wide GID rule registry.
GidRuleRegistry& defaultGidRuleRegistry();

// A discovered GID and whether it came from a built-in rule.
struct DiscoveredGid {
  ibv_gid_entry entry{};
  bool fromBuiltInRule{};
};

struct DiscoveryTables {
  // GIDs admitted for table construction, grouped by bare address with at most
  // one entry per gid_type.
  std::unordered_map<std::string, std::vector<DiscoveredGid>> discovered;
  // Addresses to seat in discovery order, each listed once regardless of how
  // many gid_types they have.
  std::vector<std::string> discoveredOrder;
  std::unordered_map<std::string, ibv_gid_entry> all;
  std::unordered_map<std::string, ibv_gid_entry> byInterfaceName;
  std::unordered_map<std::string, std::string> interfaceNameByAddress;
};

// Raw-framed claims bypass request and interface-state filtering and do not
// populate the ordinary-address lookup tables.
void recordGid(
    const DiscoveredAddress& candidate,
    uint32_t gidType,
    bool requestedByUser,
    bool rawFramedGid,
    bool fromBuiltInRule,
    DiscoveryTables& tables);

[[nodiscard]] ibv_gid_entry buildGidEntry(
    const DiscoveredAddress& candidate,
    uint32_t gidType);

// Builds the non-multiport table with built-in entries in address order,
// followed by registered entries. Adding registered claims does not move
// built-in entries.
//
// addressOrder is the requested order, or first-seen order when none was
// supplied. Missing addresses are logged and skipped.
[[nodiscard]] std::vector<ibv_gid_entry> orderGidTable(
    const std::vector<std::string>& addressOrder,
    const std::unordered_map<std::string, std::vector<DiscoveredGid>>&
        discovered);

} // namespace libsoftmetaroce
