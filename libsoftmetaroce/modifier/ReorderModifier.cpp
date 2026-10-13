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

#include <libsoftmetaroce/modifier/ReorderModifier.h>

#include <glog/logging.h>

namespace libsoftmetaroce {

ReorderModifier::ReorderModifier(std::unique_ptr<ModifierConfig> config)
    : Modifier(std::move(config)) {
  auto* reorderConfig =
      dynamic_cast<ReorderModifierConfig*>(&getModifierConfig());
  if (!reorderConfig) {
    throw std::runtime_error(
        "ReorderModifier: config is not a ReorderModifierConfig");
  }
  distance_ = reorderConfig->distance;
}

ssize_t ReorderModifier::modifyTxPacket(
    SendPacketFn& sendPacketFn,
    std::unique_ptr<TxPacketWithMetadata> txPacketData) {
  // Store pointer to sendPacketFn for use in timer callback.
  // The caller must ensure sendPacketFn outlives this modifier.
  sendPacketFn_ = &sendPacketFn;

  const auto ret = txPacketData->packet->getSizeBytes();

  // held for a delayed send; take ownership of the payload so it survives the
  // original buffer being freed (no-op for packets without a payload)
  txPacketData->packet->ownPayload();

  if (distance_.unit == ReorderModifierConfig::Distance::Unit::Packet) {
    VLOG(2)
        << "ReorderModifier: Intercepted TX packet for packet-based reorder, "
        << "distance=" << distance_.value << " packets";
    onPacketSent();
    queue.emplace(
        (*totalPacketsSent_.rlock()) + distance_.value,
        std::make_unique<DelayedSend>(
            [this,
             address = txPacketData->remoteSocketAddr,
             packet = std::move(txPacketData->packet)]() mutable {
              VLOG(2) << "ReorderModifier: Sending reordered TX packet";
              if (sendPacketFn_) {
                (*sendPacketFn_)(address, std::move(packet), std::nullopt);
              }
            }));
  } else if (distance_.unit == ReorderModifierConfig::Distance::Unit::TimeUs) {
    // The clock the deadline is expressed against, so the log below reports
    // delivery error rather than drift between two clocks.
    auto interceptTimeNs = getNowNs64();
    VLOG(2) << "ReorderModifier: Intercepted TX packet for time-based reorder, "
            << "configured_delay_us=" << distance_.value << " ("
            << (distance_.value / 1000.0) << "ms)";
    deadlineQueue_.scheduleAfter(
        std::chrono::microseconds(distance_.value),
        [this,
         address = txPacketData->remoteSocketAddr,
         packet = std::move(txPacketData->packet),
         interceptTimeNs,
         configuredDelayUs = distance_.value]() mutable {
          const int64_t actualDelayUs =
              (static_cast<int64_t>(getNowNs64()) -
               static_cast<int64_t>(interceptTimeNs)) /
              1000;
          VLOG(2) << "ReorderModifier: Sending delayed TX packet, "
                  << "configured_delay_us=" << configuredDelayUs << " ("
                  << (configuredDelayUs / 1000.0) << "ms), "
                  << "actual_delay_us=" << actualDelayUs << " ("
                  << (actualDelayUs / 1000.0) << "ms)";
          if (sendPacketFn_) {
            (*sendPacketFn_)(address, std::move(packet), std::nullopt);
          }
          onPacketSent();
        });
  }
  return (ret);
}

void ReorderModifier::modifyRxPacket(
    ReceivePacketFn& receivePacketFn,
    std::unique_ptr<IngressPacket> ingressPacket) {
  // Store pointer to receivePacketFn for use in timer callback.
  // The caller must ensure receivePacketFn outlives this modifier.
  receivePacketFn_ = &receivePacketFn;

  if (distance_.unit == ReorderModifierConfig::Distance::Unit::Packet) {
    VLOG(2)
        << "ReorderModifier: Intercepted RX packet for packet-based reorder, "
        << "distance=" << distance_.value << " packets";
    onPacketReceived();
    rxQueue_.emplace(
        (*totalPacketsReceived_.rlock()) + distance_.value,
        std::make_unique<DelayedSend>(
            [this, packet = std::move(ingressPacket)]() mutable {
              VLOG(2) << "ReorderModifier: Receiving reordered RX packet";
              if (receivePacketFn_) {
                (*receivePacketFn_)(std::move(packet));
              }
            }));

  } else if (distance_.unit == ReorderModifierConfig::Distance::Unit::TimeUs) {
    auto interceptTimeNs = getNowNs64();
    VLOG(2) << "ReorderModifier: Intercepted RX packet for time-based reorder, "
            << "configured_delay_us=" << distance_.value << " ("
            << (distance_.value / 1000.0) << "ms)";
    deadlineQueue_.scheduleAfter(
        std::chrono::microseconds(distance_.value),
        [this,
         packet = std::move(ingressPacket),
         interceptTimeNs,
         configuredDelayUs = distance_.value]() mutable {
          const int64_t actualDelayUs =
              (static_cast<int64_t>(getNowNs64()) -
               static_cast<int64_t>(interceptTimeNs)) /
              1000;
          VLOG(2) << "ReorderModifier: Delivering delayed RX packet, "
                  << "configured_delay_us=" << configuredDelayUs << " ("
                  << (configuredDelayUs / 1000.0) << "ms), "
                  << "actual_delay_us=" << actualDelayUs << " ("
                  << (actualDelayUs / 1000.0) << "ms)";
          if (receivePacketFn_) {
            (*receivePacketFn_)(std::move(packet));
          }
        });
  }
}

void ReorderModifier::onPacketSent() {
  auto totalPktSentLock = totalPacketsSent_.wlock();
  (*totalPktSentLock)++;
  // Check if the next packets in the queue are ready to be sent.
  while (!queue.empty() && queue.top().first <= *totalPktSentLock) {
    (*queue.top().second.get())();
    queue.pop();
  }
}

void ReorderModifier::onPacketReceived() {
  auto totalPktReceivedLock = totalPacketsReceived_.wlock();
  (*totalPktReceivedLock)++;
  // Check if the next packets in the queue are ready to be received.
  while (!rxQueue_.empty() && rxQueue_.top().first <= *totalPktReceivedLock) {
    (*rxQueue_.top().second.get())();
    rxQueue_.pop();
  }
}

} // namespace libsoftmetaroce
