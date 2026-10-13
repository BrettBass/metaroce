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

#include <gtest/gtest.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <libsoftmetaroce/Constants.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <cstring>
#include <string>

namespace libsoftmetaroce {
namespace {

// Loopback is the one interface name if_nametoindex resolves everywhere the
// tests run, so the cases that need a resolvable name use it.
constexpr const char* kResolvableInterface = "lo";

constexpr uint32_t kLinkLocalScopeId = 7;
constexpr uint32_t kUnresolvedInterfaceIndex = 0;

DiscoveredAddress candidate(
    const std::string& address,
    uint32_t scopeId = 0,
    bool interfaceUp = true,
    uint32_t interfaceIndex = 7) {
  return DiscoveredAddress{
      .scopelessAddress = folly::IPAddress(address),
      .interfaceName = "eth0",
      .interfaceIndex = interfaceIndex,
      .scopeId = scopeId,
      .interfaceUp = interfaceUp,
  };
}

// getifaddrs reported this address with a scope, but if_nametoindex no longer
// resolves the interface name: the interface went away mid-scan.
DiscoveredAddress withdrawnInterfaceCandidate() {
  auto input = candidate("fe80::1");
  input.scopeId = kLinkLocalScopeId;
  input.interfaceIndex = kUnresolvedInterfaceIndex;
  return input;
}

// Member storage keeps the pointers in entry_ valid for the object's lifetime.
class IfAddrsEntry {
 public:
  IfAddrsEntry(std::string interfaceName, unsigned int flags)
      : name_{std::move(interfaceName)} {
    entry_.ifa_name = name_.data();
    entry_.ifa_flags = flags;
  }

  IfAddrsEntry(const IfAddrsEntry&) = delete;
  IfAddrsEntry& operator=(const IfAddrsEntry&) = delete;

  void setScopedV6(const std::string& address, uint32_t scopeId) {
    storage_.sin6_family = AF_INET6;
    storage_.sin6_scope_id = scopeId;
    const auto bytes = folly::IPAddressV6(address).toByteArray();
    std::memcpy(&storage_.sin6_addr, bytes.data(), bytes.size());
    entry_.ifa_addr = reinterpret_cast<sockaddr*>(&storage_);
  }

  void setUnsupportedFamily() {
    storage_.sin6_family = AF_PACKET;
    entry_.ifa_addr = reinterpret_cast<sockaddr*>(&storage_);
  }

  [[nodiscard]] const struct ifaddrs& get() const {
    return entry_;
  }

 private:
  std::string name_;
  sockaddr_in6 storage_{};
  ifaddrs entry_{};
};

// Conditions that enable built-in Ethernet classification.
constexpr ScanConditions kEthernetAllowed{
    .explicitGidsSelected = true,
    .hasRawCapability = true,
};

TEST(GidDiscovery, CollectsEveryClaimAndPreservesOpaqueTypes) {
  constexpr uint32_t kCustomGidType = 0xfeed;
  const GidRuleChain chain(
      {
          [=](const DiscoveredAddress&) {
            return std::optional{kCustomGidType};
          },
          [](const DiscoveredAddress&) { return std::optional{uint32_t{9}}; },
      },
      /*builtInCount=*/1);

  const auto claims = chain.classify(candidate("192.0.2.1"));

  EXPECT_EQ(claims.builtIn, std::vector<uint32_t>{kCustomGidType})
      << "classify must pass an opaque gid_type through unchanged";
  EXPECT_EQ(claims.registered, std::vector<uint32_t>{9u})
      << "a later claim for the same address must remain in the registered group";
}

TEST(GidDiscovery, EthernetSurfacesForAnyRequestedGid) {
  const auto chain = GidRuleRegistry().makeChain(kEthernetAllowed);
  EXPECT_EQ(
      chain
          .classify(
              candidate("fe80::1", kLinkLocalScopeId, /*interfaceUp=*/false))
          .builtIn,
      std::vector<uint32_t>{kGidTypeMetaRoceEthernet})
      << "an eligible link-local GID must classify as Ethernet whenever the "
         "user requested any GID, even if this address was not the one listed";
}

TEST(GidDiscovery, DefaultRulesUseUdpForOrdinaryAddresses) {
  const auto chain = GidRuleRegistry().makeChain(ScanConditions{});

  EXPECT_EQ(
      chain.classify(candidate("192.0.2.1")).builtIn,
      std::vector<uint32_t>{kGidTypeMetaRoceUdp})
      << "an ordinary IPv4 address must classify as UDP";
  EXPECT_EQ(
      chain.classify(candidate("2001:db8::1")).builtIn,
      std::vector<uint32_t>{kGidTypeMetaRoceUdp})
      << "a global IPv6 address must classify as UDP";
}

TEST(GidDiscovery, DefaultRulesRejectIneligibleLinkLocal) {
  constexpr ScanConditions kNoRawCapability{
      .explicitGidsSelected = true,
      .hasRawCapability = false,
  };
  constexpr ScanConditions kNoRequestedGids{
      .explicitGidsSelected = false,
      .hasRawCapability = true,
  };
  const auto scopedLinkLocal = candidate("fe80::1", kLinkLocalScopeId);

  EXPECT_TRUE(
      GidRuleRegistry()
          .makeChain(kNoRawCapability)
          .classify(scopedLinkLocal)
          .empty())
      << "a link-local GID without raw capability must not classify as "
         "Ethernet, and scoped link-local is not UDP either";
  EXPECT_TRUE(
      GidRuleRegistry()
          .makeChain(kNoRequestedGids)
          .classify(scopedLinkLocal)
          .empty())
      << "with no requested GIDs the Ethernet rule must not fire";
}

TEST(GidDiscovery, EthernetRuleRequiresResolvedInterfaceIndex) {
  const auto chain = GidRuleRegistry().makeChain(kEthernetAllowed);

  EXPECT_TRUE(chain.classify(withdrawnInterfaceCandidate()).empty())
      << "an unresolved interface index (0) must not classify as Ethernet";
}

TEST(GidDiscovery, UnscopedLinkLocalRetainsUdpClassification) {
  const auto chain = GidRuleRegistry().makeChain(ScanConditions{});

  EXPECT_EQ(
      chain.classify(candidate("fe80::1", /*scopeId=*/0)).builtIn,
      std::vector<uint32_t>{kGidTypeMetaRoceUdp})
      << "the UDP rule must claim an unscoped link-local address";
}

TEST(GidDiscovery, RegisteredRuleAppendsToDefaultRules) {
  constexpr uint32_t kCustomGidType = 0xbeef;
  GidRuleRegistry registry;
  registry.registerRule([=](const DiscoveredAddress& input) {
    return input.scopelessAddress == folly::IPAddress("192.0.2.99")
        ? std::optional<uint32_t>{kCustomGidType}
        : std::nullopt;
  });
  const auto chain = registry.makeChain(ScanConditions{});

  const auto claimed = chain.classify(candidate("192.0.2.99"));
  EXPECT_EQ(claimed.builtIn, std::vector<uint32_t>{kGidTypeMetaRoceUdp})
      << "the built-in address/type claim must remain available when an "
         "extension also claims the address";
  EXPECT_EQ(claimed.registered, std::vector<uint32_t>{kCustomGidType})
      << "the registered rule must add a claim without replacing the built-in claim";

  const auto declined = chain.classify(candidate("192.0.2.1"));
  EXPECT_EQ(declined.builtIn, std::vector<uint32_t>{kGidTypeMetaRoceUdp})
      << "addresses the registered rule declines still hit the UDP default";
  EXPECT_TRUE(declined.registered.empty())
      << "the declined registered rule must add no extension claim";
}

TEST(GidDiscovery, DecodesAScopedEntryIntoABareAddressAndASeparateScope) {
  IfAddrsEntry entry(kResolvableInterface, IFF_UP);
  entry.setScopedV6("fe80::1", kLinkLocalScopeId);

  const auto decoded = makeDiscoveredAddress(entry.get());

  ASSERT_TRUE(decoded) << "makeDiscoveredAddress must accept a scoped "
                          "link-local on a resolvable interface";
  EXPECT_EQ(decoded->scopelessAddress.str(), "fe80::1")
      << "the decoded address must omit the kernel-reported scope so it matches "
         "a bare requested-GID address";
  EXPECT_EQ(decoded->scopeId, kLinkLocalScopeId)
      << "the scope the kernel reported must survive in its own field";
  EXPECT_TRUE(decoded->interfaceUp)
      << "IFF_UP on the entry must reach the decoded address";
}

TEST(GidDiscovery, RejectsAnEntryDiscoveryCannotUse) {
  const IfAddrsEntry noAddress(kResolvableInterface, IFF_UP);
  IfAddrsEntry badFamily(kResolvableInterface, IFF_UP);
  badFamily.setUnsupportedFamily();
  IfAddrsEntry withdrawn("lsmr-no-such-interface", IFF_UP);
  withdrawn.setScopedV6("fe80::1", kLinkLocalScopeId);

  EXPECT_FALSE(makeDiscoveredAddress(noAddress.get()).has_value())
      << "an entry carrying no address has nothing to classify";
  EXPECT_FALSE(makeDiscoveredAddress(badFamily.get()).has_value())
      << "folly::IPAddress would throw on a family that is neither IPv4 nor "
         "IPv6, so the decoder must reject it first";
  EXPECT_FALSE(makeDiscoveredAddress(withdrawn.get()).has_value())
      << "an interface that no longer resolves to an index cannot carry a GID";
}

TEST(GidDiscovery, RecordsAScopelessCandidateUnderItsBareAddress) {
  auto input = candidate("fe80::1", kLinkLocalScopeId);
  DiscoveryTables tables;

  recordGid(
      input,
      kGidTypeMetaRoceEthernet,
      /*requestedByUser=*/false,
      /*rawFramedGid=*/true,
      /*fromBuiltInRule=*/true,
      tables);

  EXPECT_EQ(tables.discovered.count("fe80::1"), 1u)
      << "recordGid must key a scoped candidate by its bare address";
}

TEST(GidDiscovery, RecordsOrdinaryGidInEveryTable) {
  DiscoveryTables tables;
  recordGid(
      candidate("192.0.2.1"),
      kGidTypeMetaRoceUdp,
      /*requestedByUser=*/true,
      /*rawFramedGid=*/false,
      /*fromBuiltInRule=*/true,
      tables);

  EXPECT_EQ(tables.discovered.count("192.0.2.1"), 1u)
      << "an admitted ordinary GID is discoverable";
  EXPECT_EQ(tables.discoveredOrder, std::vector<std::string>{"192.0.2.1"})
      << "the admitted address must appear once in discovery order";
  EXPECT_EQ(tables.all.count("192.0.2.1"), 1u)
      << "every ordinary GID is recorded, admitted or not";
  EXPECT_EQ(tables.byInterfaceName.count("eth0"), 1u)
      << "the interface name must map to its entry";
  EXPECT_EQ(tables.interfaceNameByAddress["192.0.2.1"], "eth0")
      << "the reverse index resolves an address back to its interface";
}

TEST(GidDiscovery, EthernetGidSkipsTheOrdinaryTables) {
  DiscoveryTables tables;
  recordGid(
      candidate("fe80::1", /*scopeId=*/3),
      kGidTypeMetaRoceEthernet,
      /*requestedByUser=*/false,
      /*rawFramedGid=*/true,
      /*fromBuiltInRule=*/true,
      tables);

  EXPECT_EQ(tables.discovered.count("fe80::1"), 1u)
      << "recordGid must retain a raw-framed claim without consulting "
         "requestedByUser";
  EXPECT_TRUE(tables.discoveredOrder.empty())
      << "the built-in Ethernet GID must not enter fallback address order";
  EXPECT_TRUE(tables.all.empty())
      << "raw-framed claims must not populate the unfiltered address lookup";
  EXPECT_TRUE(tables.byInterfaceName.empty())
      << "a raw GID must stay out of the interface-name map used by macvlan "
         "discovery";
  EXPECT_TRUE(tables.interfaceNameByAddress.empty())
      << "a raw GID must stay out of the address-to-interface map used by "
         "macvlan discovery";
}

TEST(GidDiscovery, RegisteredRawGidProvidesFallbackAddressOrder) {
  constexpr uint32_t kRegisteredRawGidType = 0xbeef;
  DiscoveryTables tables;
  recordGid(
      candidate("fe80::1", /*scopeId=*/3),
      kRegisteredRawGidType,
      /*requestedByUser=*/false,
      /*rawFramedGid=*/true,
      /*fromBuiltInRule=*/false,
      tables);

  const auto table = orderGidTable(tables.discoveredOrder, tables.discovered);

  ASSERT_EQ(table.size(), 1u)
      << "a registered raw GID must be seated without a requested-GID list";
  EXPECT_EQ(table[0].gid_type, kRegisteredRawGidType)
      << "the registered raw GID must preserve its type";
  EXPECT_EQ(table[0].gid_index, 0u)
      << "the sole registered raw GID must take gid_index 0";
  EXPECT_TRUE(tables.all.empty())
      << "a registered raw GID must stay out of the ordinary address lookup";
  EXPECT_TRUE(tables.byInterfaceName.empty())
      << "a registered raw GID must stay out of the interface-name lookup";
  EXPECT_TRUE(tables.interfaceNameByAddress.empty())
      << "a registered raw GID must stay out of the address-to-interface lookup";
}

TEST(GidDiscovery, UnrequestedOrdinaryGidIsKnownButNotDiscovered) {
  DiscoveryTables tables;
  recordGid(
      candidate("192.0.2.1"),
      kGidTypeMetaRoceUdp,
      /*requestedByUser=*/false,
      /*rawFramedGid=*/false,
      /*fromBuiltInRule=*/true,
      tables);

  EXPECT_TRUE(tables.discovered.empty())
      << "an address outside the requested list must not be offered";
  EXPECT_EQ(tables.all.count("192.0.2.1"), 1u)
      << "it stays resolvable for callers that ask by name";
  EXPECT_TRUE(tables.discoveredOrder.empty())
      << "an unrequested address must not take a gid index slot";
}

TEST(GidDiscovery, DownInterfaceKeepsOrdinaryGidUndiscovered) {
  DiscoveryTables tables;
  recordGid(
      candidate("192.0.2.1", /*scopeId=*/0, /*interfaceUp=*/false),
      kGidTypeMetaRoceUdp,
      /*requestedByUser=*/true,
      /*rawFramedGid=*/false,
      /*fromBuiltInRule=*/true,
      tables);

  EXPECT_TRUE(tables.discovered.empty())
      << "an ordinary GID on a down interface must not enter discovery";
  EXPECT_EQ(tables.all.count("192.0.2.1"), 1u)
      << "the address stays resolvable by name even when not discoverable";
}

TEST(GidDiscovery, RepeatedAddressDoesNotDuplicateOrder) {
  DiscoveryTables tables;
  for (int i = 0; i < 3; ++i) {
    recordGid(
        candidate("192.0.2.1"),
        kGidTypeMetaRoceUdp,
        /*requestedByUser=*/true,
        /*rawFramedGid=*/false,
        /*fromBuiltInRule=*/true,
        tables);
  }

  EXPECT_EQ(tables.discoveredOrder.size(), 1u)
      << "repeated observations must not duplicate the address in discovery "
         "order";
  EXPECT_EQ(tables.discovered.at("192.0.2.1").size(), 1u)
      << "the repeated address/type pair must be recorded once";
}

TEST(GidDiscovery, AddressCarriesEveryGidTypeItWasClaimedFor) {
  constexpr uint32_t kExtensionGidType = 0xbeef;
  DiscoveryTables tables;
  recordGid(
      candidate("192.0.2.1"),
      kGidTypeMetaRoceUdp,
      /*requestedByUser=*/true,
      /*rawFramedGid=*/false,
      /*fromBuiltInRule=*/true,
      tables);
  recordGid(
      candidate("192.0.2.1"),
      kExtensionGidType,
      /*requestedByUser=*/true,
      /*rawFramedGid=*/false,
      /*fromBuiltInRule=*/false,
      tables);

  const auto& gids = tables.discovered.at("192.0.2.1");
  ASSERT_EQ(gids.size(), 2u)
      << "a second gid_type must not displace the first, or the address stops "
         "being reachable over the transport it already served";
  EXPECT_EQ(gids[0].entry.gid_type, kGidTypeMetaRoceUdp)
      << "the built-in claim was recorded before the extension claim";
  EXPECT_TRUE(gids[0].fromBuiltInRule)
      << "the first claim must retain its built-in origin";
  EXPECT_EQ(gids[1].entry.gid_type, kExtensionGidType)
      << "the second claim must preserve the extension gid_type";
  EXPECT_FALSE(gids[1].fromBuiltInRule)
      << "the second claim must retain its registered-rule origin";
  EXPECT_EQ(tables.discoveredOrder, std::vector<std::string>{"192.0.2.1"})
      << "the address still takes one place in discovery order, however many "
         "gid_types it carries";
}

TEST(GidDiscovery, EntryBuilderNormalizesIpv4AndCopiesMetadata) {
  constexpr uint32_t kCustomGidType = 0xfeed;
  const auto input = candidate("192.0.2.1");

  const auto entry = buildGidEntry(input, kCustomGidType);
  const auto address = folly::IPAddressV6::fromBinary(
      folly::ByteRange(entry.gid.raw, sizeof(entry.gid.raw)));

  EXPECT_EQ(address, folly::IPAddress("::ffff:192.0.2.1"))
      << "an IPv4 GID must be stored as its IPv4-mapped IPv6 form";
  EXPECT_EQ(entry.gid_type, kCustomGidType)
      << "the resolved gid_type must be copied verbatim";
  EXPECT_EQ(entry.ndev_ifindex, input.interfaceIndex)
      << "the candidate's interface index must be copied to ndev_ifindex";
  EXPECT_EQ(entry.port_num, 1) << "the emulated device exposes a single port";
}

TEST(GidDiscovery, EntryBuilderCopiesIpv6Verbatim) {
  const auto input = candidate("2001:db8::1");

  const auto entry = buildGidEntry(input, kGidTypeMetaRoceUdp);
  const auto address = folly::IPAddressV6::fromBinary(
      folly::ByteRange(entry.gid.raw, sizeof(entry.gid.raw)));

  EXPECT_EQ(address, folly::IPAddress("2001:db8::1"))
      << "a native IPv6 GID must be copied into gid.raw unchanged";
  EXPECT_EQ(entry.gid_type, kGidTypeMetaRoceUdp)
      << "the resolved gid_type must be copied verbatim";
  EXPECT_EQ(entry.ndev_ifindex, input.interfaceIndex)
      << "the candidate's interface index must be copied to ndev_ifindex";
}

TEST(GidDiscovery, ExtensionTypesAppendWithoutMovingBuiltInIndexes) {
  constexpr uint32_t kExtensionGidType = 0xbeef;
  const std::vector<std::string> order{"192.0.2.1", "192.0.2.2"};
  const auto builtInGid = [](const std::string& address, uint32_t gidType) {
    return DiscoveredGid{
        buildGidEntry(candidate(address), gidType), /*fromBuiltInRule=*/true};
  };

  // Every address carries only its built-in type, as before anything
  // registered.
  const std::unordered_map<std::string, std::vector<DiscoveredGid>>
      beforeExtension{
          {"192.0.2.1", {builtInGid("192.0.2.1", kGidTypeMetaRoceUdp)}},
          {"192.0.2.2", {builtInGid("192.0.2.2", kGidTypeMetaRoceUdp)}},
      };
  // The first address now also carries an extension type.
  auto afterExtension = beforeExtension;
  afterExtension["192.0.2.1"].push_back(
      DiscoveredGid{
          buildGidEntry(candidate("192.0.2.1"), kExtensionGidType),
          /*fromBuiltInRule=*/false});

  const auto before = orderGidTable(order, beforeExtension);
  const auto after = orderGidTable(order, afterExtension);

  ASSERT_EQ(before.size(), 2u)
      << "the baseline table must contain both built-in GIDs";
  ASSERT_EQ(after.size(), 3u) << "the extension type must add a table entry";
  for (size_t i = 0; i < before.size(); ++i) {
    EXPECT_EQ(after[i].gid_type, before[i].gid_type)
        << "entry " << i << " changed gid_type, so a gid index an operator "
        << "already set now resolves to a different transport";
    EXPECT_EQ(
        folly::ByteRange(after[i].gid.raw, sizeof(after[i].gid.raw)),
        folly::ByteRange(before[i].gid.raw, sizeof(before[i].gid.raw)))
        << "entry " << i << " changed address, so --sgid_index=" << i
        << " now points to a different host address";
    EXPECT_EQ(after[i].gid_index, i) << "gid_index must match table position";
  }
  EXPECT_EQ(after[2].gid_type, kExtensionGidType)
      << "the extension type must land after every built-in one, not beside the "
         "address it belongs to";
}

TEST(GidDiscovery, OrderGidTableSkipsAnAddressDiscoveryNeverFound) {
  const std::unordered_map<std::string, std::vector<DiscoveredGid>> discovered{
      {"192.0.2.1",
       {DiscoveredGid{
           buildGidEntry(candidate("192.0.2.1"), kGidTypeMetaRoceUdp),
           /*fromBuiltInRule=*/true}}},
  };

  const auto table = orderGidTable({"192.0.2.9", "192.0.2.1"}, discovered);

  ASSERT_EQ(table.size(), 1u)
      << "an address absent from discovery must not reach the table";
  EXPECT_EQ(table[0].gid_index, 0u)
      << "the sole discovered address must take gid_index 0";
}

} // namespace
} // namespace libsoftmetaroce
