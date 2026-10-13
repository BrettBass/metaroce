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

#include <libsoftmetaroce/Cqe.h>
#include <libsoftmetaroce/Ecn.h>
#include <libsoftmetaroce/Flow.h>
#include <libsoftmetaroce/codec/AckHeader.h>
#include <libsoftmetaroce/codec/BaseHeader.h>
#include <libsoftmetaroce/codec/Packet.h>
#include <atomic>
#include <deque>
#include <memory>
#include <optional>

namespace libsoftmetaroce {

class Qp;
class Tag;
enum class RequestValidationError : uint8_t;

/* lightweight receive flow stores RxState */
class RxFlow : public Flow {
 public:
  struct RxState {
    explicit RxState() = default;
    uint16_t rcvNxt{0};
    uint16_t maxRcvdFsn{UINT16_MAX};
    uint8_t receivedCeCount{0};
    std::deque<FsnState> rcvBitmap;

    bool hasRnrBitsSet() const {
      return std::any_of(
          rcvBitmap.begin(), rcvBitmap.end(), [](const auto& state) {
            return state == FsnState::BRNR;
          });
    }

    uint8_t getAndClearReceivedCeCount() {
      // Saturating the four-bit field beats wrapping a larger count into a
      // smaller one and reporting less congestion than arrived.
      uint8_t ce = std::min(receivedCeCount, kMaxCnpCeCount);
      receivedCeCount = 0;
      return ce;
    }
  };

  explicit RxFlow(
      Qp& qp,
      uint16_t flowId,
      uint16_t socketId,
      uint16_t portPairIdx)
      : Flow(qp, flowId, socketId, portPairIdx) {}
  RxFlow(RxFlow&& other) noexcept
      // Copy base intentionally: moving it would trigger use-after-move on
      // the subclass members accessed below.
      // NOLINTNEXTLINE(performance-move-constructor-init)
      : Flow(other), rxState_(std::move(other.rxState_)) {
    uint32_t seq = other.ackSeq_.exchange(0, std::memory_order_acq_rel);
    if (seq > 0 && (seq & 1) == 0) {
      pendingAck_ = std::move(other.pendingAck_);
      ackSeq_.store(2, std::memory_order_release);
    }
  }

  RxFlow(const RxFlow&) = delete;
  RxFlow& operator=(const RxFlow&) = delete;
  RxFlow& operator=(RxFlow&& other) = delete;

  ~RxFlow() {
    if (ackSeq_.load(std::memory_order_acquire) != 0) {
      LOG(WARNING) << "destroying flow with a pending ack to send";
    }
  }

  /* processes the data packet and if an ack needs to be sent, it returns an
   * ack header */
  std::optional<AckHeader> processDataPacket(
      const DataPacket* const dataPacket,
      const uint8_t ipTos);

  RxState& getRxState() {
    return rxState_;
  }

  void setPendingAck(AckHeader ackHeader);

  std::unique_ptr<struct EgressPacket> getNextAckToSend();

 private:
  // One of this side's outstanding messages, named rather than pointed to: the
  // receive path decides a failure under its Tag's guard but applies it after
  // releasing, and a `Wqe*` is good only under that guard. `opType` is the
  // arriving opcode, which selects the map the msn resolves in.
  struct FailedMessage {
    BaseHeader::Opcode::OperationType opType;
    uint16_t msn;
    WorkCompletionStatus status;
  };

  static constexpr bool isEcnCapable(uint8_t ipTos) {
    return (ipTos & kEcnMask) != 0;
  }
  static constexpr bool isEcnMarked(uint8_t ipTos) {
    return (ipTos & kEcnMask) == kEcnCE;
  }
  bool shouldAckForEcn(uint8_t ipTos);

  // Creates the servicing WQE on a message's first packet (annotates the posted
  // one for SEND) and validates the request. Tag reused from the caller's
  // lookup.
  RequestValidationError ensureAndValidateWqe(
      Tag& tag,
      const DataPacket& packet);
  std::optional<AckHeader> processNewDataPacket(const DataPacket& packet);
  // Put the tag in error and complete what this thread owns: its receives, and
  // `failedMessage` if any, marked so it reports its own status not the flush.
  // The sends are the sender's, marked failing at its next visit. Caller holds
  // neither the Tag semaphore nor its retirement guard; this acquires both.
  void enterTagErrorAndFlushReceives(
      Tag& tag,
      const std::optional<FailedMessage>& failedMessage);

  RxState rxState_;

  // Seqlock-based single-slot ACK buffer. Single atomic:
  //   0         — empty, no pending ACK
  //   odd       — writer is mid-write, reader spins until even
  //   even > 0  — stable ACK ready to read
  // Writer never blocks. Reader spins on odd (bounded: one struct
  // assignment + one atomic store).
  AckHeader pendingAck_;
  std::atomic<uint32_t> ackSeq_{0};
};

} // namespace libsoftmetaroce
