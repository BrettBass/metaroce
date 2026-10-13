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

#include <folly/SocketAddress.h>
#include <glog/logging.h>
#include <libsoftmetaroce/IngressPacket.h>
#include <libsoftmetaroce/modifier/ModifierConfig.h>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace libsoftmetaroce {

using SendPacketFn = folly::Function<ssize_t(
    const folly::SocketAddress&,
    std::unique_ptr<Packet>,
    std::optional<uint32_t> originalIcrc)>;
using ReceivePacketFn = folly::Function<void(std::unique_ptr<IngressPacket>)>;

class Modifier {
 public:
  struct TxPacketWithMetadata {
    folly::SocketAddress remoteSocketAddr;
    std::unique_ptr<Packet> packet;
  };

  explicit Modifier(std::unique_ptr<ModifierConfig> config)
      : config_(std::move(config)) {}
  virtual ~Modifier();
  // Derived classes hold a reference to the config this owns.
  Modifier(Modifier&&) = delete;
  Modifier& operator=(Modifier&&) = delete;
  Modifier(const Modifier&) = delete;
  Modifier& operator=(const Modifier&) = delete;
  /*
   * Returns true if the packet matches the filter. If it matches, it applies
   * the modifier.
   */

  virtual ssize_t modifyTxPacketIfMatches(
      SendPacketFn& sendPacketFn,
      std::unique_ptr<TxPacketWithMetadata> txPacketData);
  virtual void modifyRxPacketIfMatches(
      ReceivePacketFn& receivePacketFn,
      std::unique_ptr<IngressPacket> ingressPacket);
  virtual ssize_t modifyTxPacket(
      SendPacketFn& sendPacketFn,
      std::unique_ptr<TxPacketWithMetadata> txPacketData) = 0;
  virtual void modifyRxPacket(
      ReceivePacketFn& receivePacketFn,
      std::unique_ptr<IngressPacket> ingressPacket) = 0;

  virtual void onPacketSent() = 0;
  virtual void onPacketReceived() {}

  // Whether a received packet may be delivered from a timer thread rather than
  // the reader thread that took it off the socket.
  [[nodiscard]] virtual bool mayDeferReceive() const noexcept {
    return false;
  }

  // Cancel this modifier's timers and join their threads, leaving it intact.
  virtual void stopTimers() {}

  // Log packet details for debugging
  void logPacketDetails(
      const std::unique_ptr<Packet>& packet,
      const folly::SocketAddress& remoteSocketAddr,
      std::string_view direction);

  // Every constructor supplies a config, including CompositeModifier's, which
  // builds one of type Unknown.
  [[nodiscard]] ModifierConfig& getModifierConfig() const noexcept {
    return *CHECK_NOTNULL(config_.get());
  }

 protected:
  // Which direction this modifier was configured to act on.
  [[nodiscard]] bool actsOnReceive() const noexcept {
    return getModifierConfig().filter.direction ==
        ModifierConfig::ModifierFilter::Direction::Rx;
  }

 private:
  // Atomically reserves one modification slot. Returns true if the packet
  // should be modified, false if the global cap has been reached.
  bool tryReserveModificationSlot();

  std::unique_ptr<ModifierConfig> config_;
};

class DropModifier : public Modifier {
 public:
  explicit DropModifier(std::unique_ptr<ModifierConfig> config)
      : Modifier(std::move(config)) {}
  ssize_t modifyTxPacket(
      SendPacketFn&,
      std::unique_ptr<TxPacketWithMetadata> txPacketData) override {
    // Since it is a Drop Modifier, we only pretend to have sent the frame.
    return txPacketData->packet->getSizeBytes();
  }
  void modifyRxPacket(ReceivePacketFn&, std::unique_ptr<IngressPacket>)
      override {
    // A drop modifier never forwards the packet.
  }
  void onPacketSent() override {}
};

class CompositeModifier : public Modifier {
 public:
  explicit CompositeModifier(std::vector<std::unique_ptr<Modifier>> modifiers);

  ~CompositeModifier() override;

  // The chain functions capture this, so a copy or a move would leave them
  // driving the original composite.
  CompositeModifier(const CompositeModifier&) = delete;
  CompositeModifier& operator=(const CompositeModifier&) = delete;
  CompositeModifier(CompositeModifier&&) = delete;
  CompositeModifier& operator=(CompositeModifier&&) = delete;

  ssize_t modifyTxPacketIfMatches(
      SendPacketFn& sendPacketFn,
      std::unique_ptr<TxPacketWithMetadata> txPacketData) override;

  void modifyRxPacketIfMatches(
      ReceivePacketFn& receivePacketFn,
      std::unique_ptr<IngressPacket> ingressPacket) override;

  ssize_t modifyTxPacket(SendPacketFn&, std::unique_ptr<TxPacketWithMetadata>)
      override;

  void modifyRxPacket(ReceivePacketFn&, std::unique_ptr<IngressPacket>)
      override;

  void onPacketSent() override;

  [[nodiscard]] bool mayDeferReceive() const noexcept override;

  void stopTimers() override;

 private:
  // Build chain functions stored as members so timer-based modifiers
  // (PacingModifier, ReorderModifier) can safely capture &sendPacketFn.
  // With stack-local chain functions, the reference dangles after the
  // chain function returns and causes use-after-free when the timer fires.
  void buildTxChains();
  void buildRxChains();

  // Runtime state.
  // Chain functions stored as members for stable lifetime. txChainFns_[i]
  // is the downstream function passed to modifiers_[i]; it chains to
  // modifiers_[i+1] (or the terminal function for the last modifier).
  // rxChainFns_[i] chains to modifiers_[i-1] (or the terminal for i==0).
  std::vector<SendPacketFn> txChainFns_;
  std::vector<ReceivePacketFn> rxChainFns_;
  SendPacketFn* terminalSendFn_{nullptr};
  ReceivePacketFn* terminalReceiveFn_{nullptr};
  bool txChainsBuilt_{false};
  bool rxChainsBuilt_{false};

  // Injected dependency (owned modifier chain).
  // modifiers_ must be declared last so it is destroyed first (reverse
  // declaration order). Timer-based modifiers (PacingModifier,
  // ReorderModifier) cancel their timer callbacks during destruction;
  // those callbacks hold references to txChainFns_/rxChainFns_ entries,
  // which must still be alive at that point.
  std::vector<std::unique_ptr<Modifier>> modifiers_;
};

} // namespace libsoftmetaroce
