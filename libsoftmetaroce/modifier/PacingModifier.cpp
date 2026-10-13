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

#include <libsoftmetaroce/modifier/PacingModifier.h>

#include <glog/logging.h>

namespace libsoftmetaroce {

PacingModifier::PacingModifier(std::unique_ptr<ModifierConfig> config)
    : Modifier(std::move(config)) {
  auto* pacingConfig =
      dynamic_cast<PacingModifierConfig*>(&getModifierConfig());
  if (!pacingConfig) {
    throw std::runtime_error(
        "PacingModifier: config is not a PacingModifierConfig");
  }
  intervalUs_ = pacingConfig->intervalUs;
}

std::optional<uint64_t> PacingModifier::reserveReleaseTime(uint64_t nowNs) {
  const uint64_t intervalNs = static_cast<uint64_t>(intervalUs_) * 1000;
  auto nextSendTimeLock = nextSendTimeNs_.wlock();
  if (*nextSendTimeLock <= nowNs) {
    *nextSendTimeLock = nowNs + intervalNs;
    return std::nullopt;
  }
  const uint64_t releaseTimeNs = *nextSendTimeLock;
  *nextSendTimeLock += intervalNs;
  return releaseTimeNs;
}

ssize_t PacingModifier::modifyTxPacket(
    SendPacketFn& sendPacketFn,
    std::unique_ptr<TxPacketWithMetadata> txPacketData) {
  const auto ret = txPacketData->packet->getSizeBytes();
  const auto releaseTimeNs = reserveReleaseTime(getNowNs64());
  if (!releaseTimeNs) {
    VLOG(2) << "PacingModifier: Sending TX packet immediately";
    sendPacketFn(
        txPacketData->remoteSocketAddr,
        std::move(txPacketData->packet),
        std::nullopt);
    return ret;
  }

  VLOG(2) << "PacingModifier: Scheduling TX packet for release_time_ns="
          << *releaseTimeNs;
  // held for a delayed send; take ownership of the payload so it survives the
  // original buffer being freed (no-op for packets without a payload)
  txPacketData->packet->ownPayload();
  deadlineQueue_.scheduleAt(
      *releaseTimeNs,
      // Capture this invocation's callback; the caller keeps it alive through
      // the deferred send.
      [send = &sendPacketFn,
       address = txPacketData->remoteSocketAddr,
       packet = std::move(txPacketData->packet)]() mutable {
        VLOG(2) << "PacingModifier: Sending paced TX packet";
        (*send)(address, std::move(packet), std::nullopt);
      });
  return ret;
}

void PacingModifier::modifyRxPacket(
    ReceivePacketFn& receivePacketFn,
    std::unique_ptr<IngressPacket> ingressPacket) {
  const auto releaseTimeNs = reserveReleaseTime(getNowNs64());
  if (!releaseTimeNs) {
    VLOG(2) << "PacingModifier: Delivering RX packet immediately";
    receivePacketFn(std::move(ingressPacket));
    return;
  }

  VLOG(2) << "PacingModifier: Scheduling RX packet for release_time_ns="
          << *releaseTimeNs;
  deadlineQueue_.scheduleAt(
      *releaseTimeNs,
      // Capture this invocation's callback; the caller keeps it alive through
      // the deferred delivery.
      [receive = &receivePacketFn,
       packet = std::move(ingressPacket)]() mutable {
        VLOG(2) << "PacingModifier: Delivering paced RX packet";
        (*receive)(std::move(packet));
      });
}

} // namespace libsoftmetaroce
