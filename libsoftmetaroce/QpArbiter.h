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

#include <memory>
#include <set>

#include <folly/Function.h>
#include <folly/IPAddress.h>
#include <folly/MPMCQueue.h>
#include <folly/container/F14Map.h>
#include <folly/coro/Baton.h>
#include <folly/coro/Mutex.h>
#include <folly/coro/Task.h>

#include <libsoftmetaroce/EgressPacket.h>
#include <libsoftmetaroce/QpArbiterIface.h>
#include <atomic>

namespace libsoftmetaroce {

// This class manages a pool of Qps and selects which Qp gets to send a packet
// next. Qps are serviced in round-robin order, in the order that they are
// added, one packet per Qp at a time. A Qp is responsible for managing the
// order in which its flows are serviced. This class does not keep references to
// the Qps themselves, and instead only tracks qpNums and callbacks to get the
// next packet to send.
//
// The user should call addQp() on Qp creation, otherwise no packets will be
// sent from the Qp. When the Qp has a packet to send, signalReady() should be
// called. Packets are retrieved from ready Qps by
// waitForAndGetNextPacketToSend(). On destruction, the Qp should be removed
// from the arbiter by removeQp() to prevent unsafe memory accesses via the
// callback. On program exit, call shutdown() to cancel a pending wait for
// packets.
class QpArbiter : public QpArbiterIface {
 public:
  QpArbiter();
  ~QpArbiter() override;

  QpArbiter(const QpArbiter&) = delete;
  QpArbiter& operator=(const QpArbiter&) = delete;
  QpArbiter(QpArbiter&&) = delete;
  QpArbiter& operator=(QpArbiter&&) = delete;

  // Register a Qp and return its ReadySignal handle. Adding a Qp does not by
  // itself make it ready to send --- the QP must signal through the handle.
  ReadySignal* addQp(
      uint32_t qpNum,
      folly::Function<std::unique_ptr<struct EgressPacket>(void)>
          getNextPacketCallback,
      folly::Function<bool()> applyHandedOffAcksCallback) override;
  using QpArbiterIface::addQp;

  // Remove a Qp from the arbiter. Make sure this is done before the Qp is
  // deallocated elsewhere; invalidates the QP's ReadySignal handle.
  void removeQp(uint32_t qpNum) override;

  // Shutdown the arbiter. This will cancel a pending wait in
  // waitForAndGetNextPacketToSend() and prevent the arbiter from servicing any
  // Qps.
  folly::coro::Task<void> shutdown() override;

  // QP-keyed readiness signaling, reached through a QP's ReadySignal handle.
  // Unsupported after removeQp().
  void signalReady(uint32_t qpNum);
  folly::coro::Task<void> co_signalReady(uint32_t qpNum);

  // Non-blocking fast path: try to signal without scheduling a coroutine.
  // Returns true if the arbiter mutex was uncontended and the signal was
  // delivered synchronously; false if the caller should fall back to the
  // async co_signalReady path.
  bool trySignalReady(uint32_t qpNum) noexcept;

  // Mark a QP ready off the arbiter mutex: post to the intake ring for the
  // consumer to fold in. A full ring drops the signal (the retransmit timer
  // re-marks the QP) rather than blocking the producer.
  void postSignal(uint32_t qpNum) noexcept;

  // Count of postSignal() calls that found the intake ring full and dropped the
  // signal -- a measure of signaling pressure under an overloaded consumer.
  uint64_t getIntakeFullCount() const noexcept override {
    return intakeFullCount_.load(std::memory_order_relaxed);
  }

  uint64_t getStaleSignalDropCount() const noexcept override {
    return staleSignalDropCount_.load(std::memory_order_relaxed);
  }

  // Wait for a packet to send and return it. This will block until a packet is
  // available (i.e., until signalReady() is called) or shutdown() is called.
  // Packets are returned one at a time in round-robin order across the Qps that
  // have packets to send, in the order that the Qps were added. When
  // claimedQpNum is non-null it receives the serviced qpNum captured under the
  // arbiter mutex, so it stays consistent with the returned packet.
  folly::coro::Task<std::unique_ptr<struct EgressPacket>>
  waitForAndGetNextPacketToSend(uint32_t* claimedQpNum = nullptr);

  // Consume API: the single consumer claims via
  // waitForAndGetNextPacketToSend(); reenqueueQp() always succeeds (the QP
  // stays in the ready set for the next claim).
  folly::coro::Task<ClaimedPacket> claimNextPacketToSend() override;
  [[nodiscard]] bool reenqueueQp(uint32_t qpNum) noexcept override;

  // If there was a problem sending the frame, a Sender will return the
  // packet here.  Until we handle this by selectively delaying further
  // traffic to the likely unreachable destination, terminate.
  folly::coro::Task<void> handleFaultOnEgress(
      std::unique_ptr<struct EgressPacket> failed,
      int errnoOnWrite) override;

  // for debugging, query whether a specific qp is in the ready set.
  bool isQpnReady(uint32_t qpNum);

 private:
  // Per-QP handle returned by addQp(); forwards to the QP-keyed signaling
  // above.
  class SingleReadySignal : public ReadySignal {
   public:
    SingleReadySignal(QpArbiter* arbiter, uint32_t qpNum)
        : arbiter_(arbiter), qpNum_(qpNum) {}
    folly::coro::Task<void> co_signalReady() override {
      co_await arbiter_->co_signalReady(qpNum_);
    }
    void postSignal() noexcept override {
      arbiter_->postSignal(qpNum_);
    }
    bool trySignalReady() noexcept override {
      return arbiter_->trySignalReady(qpNum_);
    }

   private:
    QpArbiter* arbiter_;
    uint32_t qpNum_;
  };

  // Fold any qpNums posted via postSignal() into readyQpNums_. Caller must hold
  // arbiterMutex_.
  void drainSignalIntake();

  static constexpr size_t kSignalIntakeCapacity = 8192;

  folly::coro::Baton arbiterBaton_;
  // Lock-free intake of qpNums to mark ready, drained under arbiterMutex_ by
  // the consumer. Producers (postSignal) never take arbiterMutex_ on the fast
  // path.
  folly::MPMCQueue<uint32_t> toSignal_{kSignalIntakeCapacity};
  std::atomic<uint64_t> intakeFullCount_{0};
  std::atomic<uint64_t> staleSignalDropCount_{0};
  // Cumulative time the single sender was parked on the baton with no ready QP;
  // logged process-wide at teardown.
  std::atomic<uint64_t> idleNs_{0};
  // Protects access to all following variables.
  folly::coro::Mutex arbiterMutex_;
  folly::F14FastMap<
      uint32_t,
      folly::Function<std::unique_ptr<struct EgressPacket>(void)>>
      qpNumToCallback_;
  // No entry for a QP registered without one, which is only tests.
  folly::F14FastMap<uint32_t, folly::Function<bool()>> qpNumToAckApplier_;
  // QP handles returned by addQp(), kept reference-stable for the QP's
  // lifetime.
  folly::F14NodeMap<uint32_t, SingleReadySignal> readySignals_;
  std::set<uint32_t> readyQpNums_;
  uint32_t nextQpNumToSendOn_;
  bool shutdown_{false};
};

} // namespace libsoftmetaroce
