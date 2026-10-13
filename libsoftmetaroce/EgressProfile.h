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

#include <folly/SharedMutex.h>
#include <folly/Synchronized.h>
#include <folly/container/F14Map.h>
#include <infiniband/verbs.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace libsoftmetaroce {

class Codec;
class Encapsulation;

enum class SocketKind : uint8_t {
  UDP,
  RAW,
};

struct EgressProfile {
  using CodecFactory = std::function<std::unique_ptr<Codec>()>;
  using EncapsulationFactory =
      std::function<std::unique_ptr<Encapsulation>(const ibv_gid_entry&)>;
  using MtuValidator = std::function<bool(const ibv_gid_entry&, uint16_t)>;

  SocketKind socketKind{SocketKind::UDP};
  CodecFactory makeCodec;
  EncapsulationFactory makeEncapsulation;
  MtuValidator validateMtu;
  // Whether the profile permits a second receiver on an alternate port. A raw
  // AF_PACKET socket lacks port-based demux, so a second socket on the same
  // interface would receive duplicate copies of every frame.
  bool supportsAlternateReceiver{false};
  std::optional<uint16_t> rawEthertype;

  // Usable when all three factories are present and a raw profile supplies an
  // EtherType without claiming support for an alternate receiver.
  [[nodiscard]] bool isUsable() const noexcept;
};

EgressProfile makeMetaRoceEthernetProfile();

// Validates a QP MTU plus the largest payload-carrying header and framing
// counted by the interface MTU. Returns true when the MTU is unavailable.
[[nodiscard]] bool validateRawMtu(
    const ibv_gid_entry& entry,
    uint16_t qpMtuBytes,
    uint16_t framingBytesWithinMtu);

// Maps a gid_type to the profile serving it, one set per instance.
class EgressRegistry {
 public:
  // Seeded with the built-in metaroce UDP profile.
  EgressRegistry();

  // Add-only; reports false for a duplicate gid_type or an unusable profile.
  // A raw profile is required to return a RawEncapsulation, which registration
  // cannot check without a gid entry to invoke the factory with.
  [[nodiscard]] bool registerProfile(
      uint32_t gidType,
      EgressProfile profile); // NOLINT(performance-unnecessary-value-param)

  // Falls back to the UDP profile for an unregistered gid_type.
  [[nodiscard]] EgressProfile getProfile(uint32_t gidType) const;

  [[nodiscard]] bool validateMtu(const ibv_gid_entry& entry, uint16_t mtu)
      const;

 private:
  folly::Synchronized<
      folly::F14FastMap<uint32_t, EgressProfile>,
      folly::SharedMutex>
      profiles_;
};

// The registry the socket path uses; a transport extension registers into it.
EgressRegistry& defaultEgressRegistry();

} // namespace libsoftmetaroce
