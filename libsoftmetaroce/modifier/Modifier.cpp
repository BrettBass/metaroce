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

#include <libsoftmetaroce/modifier/Modifier.h>

#include <fmt/core.h>
#include <glog/logging.h>
#include <libsoftmetaroce/codec/Packet.h>
#include <algorithm>
#include <stdexcept>

namespace libsoftmetaroce {

Modifier::~Modifier() {
  // CompositeModifier uses Unknown type; child modifiers log their own stats
  if (config_->type == ModifierConfig::ModifierType::Unknown) {
    return;
  }

  const char* dirStr =
      config_->filter.direction == ModifierConfig::ModifierFilter::Direction::Tx
      ? "tx"
      : "rx";
  const char* pktTypeStr = config_->filter.packetType ==
          ModifierConfig::ModifierFilter::PacketType::Data
      ? "data"
      : "ack";
  auto limitStr = config_->modificationTimes.has_value()
      ? std::to_string(config_->modificationTimes.value())
      : "unlimited";

  const uint32_t applied = config_->appliedCount->load();
  if (applied == 0) {
    LOG(WARNING) << fmt::format(
        "Modifier '{}' (direction={}, packet_type={}) applied 0 times "
        "(configured limit: {}). Check direction, filter, random_percent, or "
        "modification_times.",
        config_->name,
        dirStr,
        pktTypeStr,
        limitStr);
  } else {
    LOG(INFO) << fmt::format(
        "Modifier '{}' (direction={}, packet_type={}) stats: "
        "applied {} times (configured limit: {})",
        config_->name,
        dirStr,
        pktTypeStr,
        applied,
        limitStr);
  }
}

bool Modifier::tryReserveModificationSlot() {
  if (!config_->modificationTimes.has_value()) {
    config_->appliedCount->fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  const uint32_t limit = config_->modificationTimes.value();
  uint32_t cur = config_->appliedCount->load(std::memory_order_relaxed);
  while (cur < limit) {
    if (config_->appliedCount->compare_exchange_weak(
            cur, cur + 1, std::memory_order_relaxed)) {
      return true;
    }
  }
  return false;
}

ssize_t Modifier::modifyTxPacketIfMatches(
    SendPacketFn& sendPacketFn,
    std::unique_ptr<TxPacketWithMetadata> txPacketData) {
  bool matches = config_->matchesFilter(
      *(txPacketData->packet.get()),
      ModifierConfig::ModifierFilter::Direction::Tx,
      txPacketData->remoteSocketAddr);

  if (matches) {
    logPacketDetails(
        txPacketData->packet, txPacketData->remoteSocketAddr, "Tx");
  }

  if (!matches || !tryReserveModificationSlot()) {
    // Don't modify. Send the packet right away
    ssize_t ret = sendPacketFn(
        txPacketData->remoteSocketAddr,
        std::move(txPacketData->packet),
        std::nullopt);
    onPacketSent();
    return ret;
  }

  VLOG(1) << fmt::format(
      "Applying {} modifier to TX packet: count={}/{}",
      config_->name,
      config_->appliedCount->load(std::memory_order_relaxed),
      config_->modificationTimes.has_value()
          ? std::to_string(config_->modificationTimes.value())
          : "unlimited");

  return modifyTxPacket(sendPacketFn, std::move(txPacketData));
}

void Modifier::modifyRxPacketIfMatches(
    ReceivePacketFn& receivePacketFn,
    std::unique_ptr<IngressPacket> ingressPacket) {
  bool matches = config_->matchesFilter(
      *(ingressPacket->packet.get()),
      ModifierConfig::ModifierFilter::Direction::Rx,
      ingressPacket->sourceSocketAddress);

  if (matches) {
    logPacketDetails(
        ingressPacket->packet, ingressPacket->sourceSocketAddress, "RX");
  }

  if (!matches || !tryReserveModificationSlot()) {
    // Don't modify. Receive the packet right away
    receivePacketFn(std::move(ingressPacket));
    onPacketReceived();
    return;
  }

  VLOG(1) << fmt::format(
      "Applying {} modifier to RX packet: count={}/{}",
      config_->name,
      config_->appliedCount->load(std::memory_order_relaxed),
      config_->modificationTimes.has_value()
          ? std::to_string(config_->modificationTimes.value())
          : "unlimited");

  modifyRxPacket(receivePacketFn, std::move(ingressPacket));
}

void Modifier::logPacketDetails(
    const std::unique_ptr<Packet>& packet,
    const folly::SocketAddress& remoteSocketAddr,
    std::string_view direction) {
  const auto& baseHeader = packet->getBaseHeader();
  VLOG(1) << fmt::format(
      "Packet matches {} modifier filter: direction={}, address={}, src_qp={}, dst_qp={}, path_id={}",
      config_->name,
      direction,
      remoteSocketAddr.describe(),
      baseHeader.getSrcQp(),
      baseHeader.getDstQp(),
      baseHeader.getFlowId());

  if (baseHeader.getOpcode().isData()) {
    if (const auto* dataPacket =
            dynamic_cast<const DataPacket*>(packet.get())) {
      const auto& dataHeader = dataPacket->getDataHeader();
      VLOG(1) << fmt::format(
          "Modifier: Data packet details: fsn={}, una={}, msn={}, csn={}, posn={}",
          dataHeader.getFsn(),
          dataHeader.getUna(),
          dataHeader.getMsn(),
          dataHeader.getCsn(),
          dataHeader.getPosn());
    }
  } else if (baseHeader.getOpcode().isAnAck()) {
    if (const auto* ackPacket = dynamic_cast<const AckPacket*>(packet.get())) {
      const auto& ackHeader = ackPacket->getAckHeader();
      VLOG(1) << fmt::format(
          "Modifier: Ack packet details: cumulative_fsn={}, max_rcvd_fsn={}, cdmsn={}, ack_status={}",
          ackHeader.getCumulativeFsn(),
          ackHeader.getMaxRcvdFsn(),
          ackHeader.getCumulRespDeliveredMsn(),
          static_cast<uint8_t>(ackHeader.getAckStatus()));
    }
  }
}

// CompositeModifier

namespace {
Modifier& modifierRef(const std::unique_ptr<Modifier>& modifier) {
  return *CHECK_NOTNULL(modifier.get());
}
} // namespace

CompositeModifier::CompositeModifier(
    std::vector<std::unique_ptr<Modifier>> modifiers)
    : Modifier(
          std::make_unique<ModifierConfig>(
              ModifierConfig::ModifierType::Unknown,
              ModifierConfig::ModifierFilter(
                  ModifierConfig::ModifierFilter::Direction::Tx,
                  ModifierConfig::ModifierFilter::PacketType::Data,
                  folly::F14FastMap<std::string, FieldConditions>{}),
              std::numeric_limits<uint32_t>::max(),
              "composite")),
      modifiers_(std::move(modifiers)) {
  txChainFns_.resize(modifiers_.size());
  rxChainFns_.resize(modifiers_.size());
}

ssize_t CompositeModifier::modifyTxPacketIfMatches(
    SendPacketFn& sendPacketFn,
    std::unique_ptr<TxPacketWithMetadata> txPacketData) {
  if (modifiers_.empty()) {
    return sendPacketFn(
        txPacketData->remoteSocketAddr,
        std::move(txPacketData->packet),
        std::nullopt);
  }
  terminalSendFn_ = &sendPacketFn;
  if (!txChainsBuilt_) {
    buildTxChains();
    txChainsBuilt_ = true;
  }
  return modifierRef(modifiers_[0])
      .modifyTxPacketIfMatches(txChainFns_[0], std::move(txPacketData));
}

void CompositeModifier::modifyRxPacketIfMatches(
    ReceivePacketFn& receivePacketFn,
    std::unique_ptr<IngressPacket> ingressPacket) {
  if (modifiers_.empty()) {
    receivePacketFn(std::move(ingressPacket));
    return;
  }
  terminalReceiveFn_ = &receivePacketFn;
  if (!rxChainsBuilt_) {
    buildRxChains();
    rxChainsBuilt_ = true;
  }
  size_t last = modifiers_.size() - 1;
  modifierRef(modifiers_[last])
      .modifyRxPacketIfMatches(rxChainFns_[last], std::move(ingressPacket));
}

ssize_t CompositeModifier::modifyTxPacket(
    SendPacketFn&,
    std::unique_ptr<TxPacketWithMetadata>) {
  // This should never be called since we override modifyTxPacketIfMatches
  throw std::runtime_error(
      "CompositeModifier::modifyTxPacket should not be called");
}

void CompositeModifier::modifyRxPacket(
    ReceivePacketFn&,
    std::unique_ptr<IngressPacket>) {
  // This should never be called since we override modifyRxPacketIfMatches
  throw std::runtime_error(
      "CompositeModifier::modifyRxPacket should not be called");
}

void CompositeModifier::onPacketSent() {
  for (auto& modifier : modifiers_) {
    modifierRef(modifier).onPacketSent();
  }
}

CompositeModifier::~CompositeModifier() {
  // tx chains forward to modifiers_[i + 1] and rx back to modifiers_[i - 1], so
  // no element destruction order keeps a running callback off a freed sibling.
  stopTimers();
}

void CompositeModifier::stopTimers() {
  for (auto& modifier : modifiers_) {
    CHECK_NOTNULL(modifier.get())->stopTimers();
  }
}

bool CompositeModifier::mayDeferReceive() const noexcept {
  return std::any_of(
      modifiers_.begin(), modifiers_.end(), [](const auto& modifier) {
        return modifierRef(modifier).mayDeferReceive();
      });
}

void CompositeModifier::buildTxChains() {
  for (size_t i = 0; i < modifiers_.size(); ++i) {
    txChainFns_[i] = [this, i](
                         const folly::SocketAddress& addr,
                         std::unique_ptr<Packet> pkt,
                         std::optional<uint32_t> /* icrc */) -> ssize_t {
      auto nextTxPacketData = std::make_unique<TxPacketWithMetadata>();
      nextTxPacketData->remoteSocketAddr = addr;
      nextTxPacketData->packet = std::move(pkt);
      if (i + 1 < modifiers_.size()) {
        return modifierRef(modifiers_[i + 1])
            .modifyTxPacketIfMatches(
                txChainFns_[i + 1], std::move(nextTxPacketData));
      }
      return (*terminalSendFn_)(
          nextTxPacketData->remoteSocketAddr,
          std::move(nextTxPacketData->packet),
          std::nullopt);
    };
  }
}

void CompositeModifier::buildRxChains() {
  for (size_t i = 0; i < modifiers_.size(); ++i) {
    rxChainFns_[i] = [this, i](std::unique_ptr<IngressPacket> pkt) {
      if (i == 0) {
        (*terminalReceiveFn_)(std::move(pkt));
      } else {
        modifierRef(modifiers_[i - 1])
            .modifyRxPacketIfMatches(rxChainFns_[i - 1], std::move(pkt));
      }
    };
  }
}

} // namespace libsoftmetaroce
