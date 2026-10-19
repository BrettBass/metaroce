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

#include <string_view>

namespace libsoftmetaroce {

class Qp;

/* base class for Tx and Rx Flows */
class Flow {
 public:
  enum class FsnState : uint8_t { UNACKED, ACKED, BRNR, ABANDONED };
  static constexpr std::string_view fsnStateToString(FsnState state) noexcept {
    switch (state) {
      case FsnState::UNACKED:
        return "UNACKED";
      case FsnState::ACKED:
        return "ACKED";
      case FsnState::BRNR:
        return "BRNR";
      case FsnState::ABANDONED:
        return "ABANDONED";
    }
    return "UNKNOWN";
  }
  friend std::ostream& operator<<(std::ostream& os, const FsnState& state) {
    return os << fsnStateToString(state);
  }
  Flow(
      Qp& qp,
      uint16_t flowId,
      uint16_t socketId,
      uint16_t portPairIdx) noexcept
      : qp_{qp},
        flowId_{flowId},
        socketId_{socketId},
        originalSocketId_{socketId},
        portPairIdx_{portPairIdx},
        originalPortPairIdx_{portPairIdx} {}
  Qp& getQp() const noexcept {
    return qp_;
  }
  uint16_t getFlowId() const noexcept {
    return flowId_;
  }
  uint16_t getSocketId() const noexcept {
    return socketId_;
  }
  inline folly::IPAddress getRemoteAddr() const;

  inline uint16_t getLocalPortIdx() const;
  inline uint16_t getRemotePortIdx() const;
  inline uint16_t getOriginalRemotePortIdx() const;

 protected:
  // Injected dependency.
  Qp& qp_;

  // Configuration.
  const uint16_t flowId_;

  // Runtime routing state; socketId_/portPairIdx_ change on repath/failover,
  // and socketId_ also on an ingress UDP source-port change (sender thread),
  // each with its const construction-time original preserved for reset.
  uint16_t socketId_;
  const uint16_t originalSocketId_;
  // Selected configured PortPair. Local and remote indexes are derived from
  // this so failover cannot create an unconfigured local/remote combination.
  uint16_t portPairIdx_; // modified only by Sender thread
  const uint16_t originalPortPairIdx_;
};

} // namespace libsoftmetaroce
