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

#include <folly/Synchronized.h>
#include <folly/Utility.h>
#include <folly/container/F14Map.h>
#include <folly/coro/Baton.h>
#include <folly/coro/Task.h>
#include <folly/io/async/AsyncUDPSocket.h>
#include <folly/io/async/EventBase.h>
#include <infiniband/verbs.h>
#include <libsoftmetaroce/PacketModifierSocket.h>
#include <libsoftmetaroce/SourcePortCounter.h>
#include <libsoftmetaroce/codec/PingHeader.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

namespace libsoftmetaroce {

enum class PingOutcome {
  COMPLETED,
  TIMED_OUT,
  CANCELLED,
};

inline std::ostream& operator<<(std::ostream& os, PingOutcome outcome) {
  switch (outcome) {
    case PingOutcome::COMPLETED:
      return os << "COMPLETED";
    case PingOutcome::TIMED_OUT:
      return os << "TIMED_OUT";
    case PingOutcome::CANCELLED:
      return os << "CANCELLED";
  }
  return os << "UNKNOWN(" << static_cast<int>(outcome) << ")";
}

/**
 * Result of a ping operation.
 */
struct PingResult {
  // Outcome of the ping operation
  PingOutcome outcome;
  // Error state of the ping response
  PingErrorState errorState;
  // Ping round trip time; 0 if the ping timed out or was cancelled
  std::chrono::nanoseconds rtt;
};

/**
 * PingController manages ping packet transmission and response handling.
 *
 * This component operates independently of QPs and provides:
 * - A send-only socket per local IP for transmitting pings
 * - Coroutine-based async ping with timeout
 * - Ping responses are dispatched from the Receiver via handlePingResponse()
 *
 */
class PingController : public folly::NonCopyableNonMovable {
 public:
  using SocketMaker = std::function<std::pair<
      std::unique_ptr<folly::AsyncUDPSocket>,
      std::unique_ptr<Modifier>>(folly::EventBase*)>;

  /**
   * Construct a PingController.
   *
   * @param socketMaker Factory function to create sockets (allows modifier
   *                    injection for testing)
   * @param sendToPort  Destination port for outbound pings (default 0xace)
   */
  explicit PingController(
      SocketMaker socketMaker,
      uint16_t sendToPort,
      const std::atomic<uint16_t>* activePortSet = nullptr);
  PingController(PingController&&) = delete;
  PingController& operator=(PingController&&) = delete;

  ~PingController();

  /**
   * Send a ping and return the result as a coroutine.
   */
  folly::coro::Task<PingResult> co_sendPing(
      folly::IPAddress localAddr,
      folly::IPAddress remoteAddr,
      ibv_gid_entry entry,
      uint32_t sourceQpNum,
      uint32_t remoteQpNum,
      uint8_t tagNum,
      uint16_t flowId,
      uint16_t supportedFlowCount,
      std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});

  /**
   * Get number of pending pings (for testing/debugging).
   */
  size_t getNumPendingPings() const;

  /**
   * Cancel all pending pings (useful during shutdown).
   */
  void cancelAllPendingPings();

  /**
   * Handle incoming ping response — called by Receiver on PING_RESPONSE.
   * Thread-safe via folly::Synchronized.
   */
  void handlePingResponse(
      uint16_t pingId,
      PingErrorState errorState,
      PingOutcome outcome = PingOutcome::COMPLETED);

 private:
  struct PendingPing {
    std::chrono::steady_clock::time_point sentTime;
    folly::coro::Baton baton;
    PingResult result{
        .outcome = PingOutcome::TIMED_OUT,
        .errorState = PingErrorState::NO_ERROR,
        .rtt = std::chrono::nanoseconds::zero()};
  };

  // Send-only sockets, one per local address, and the source ports they bind.
  struct SendSockets {
    folly::F14FastMap<folly::IPAddress, std::unique_ptr<PacketModifierSocket>>
        sockets;
    SourcePortCounter sourcePorts;
  };
  using LockedSendSockets =
      folly::Synchronized<SendSockets, std::mutex>::LockedPtr;

  // Get or create send-only socket for local address
  PacketModifierSocket* getSocketForAddress(
      LockedSendSockets& sendSockets,
      folly::IPAddress localAddr,
      const ibv_gid_entry& entry);

  // Members
  SocketMaker socketMaker_;
  uint16_t sendToPort_;

  // Event base for socket construction (not actively looped;
  // PacketModifierSocket::write() is synchronous)
  folly::EventBase evb_;

  folly::Synchronized<SendSockets, std::mutex> sendSockets_;

  // Pending ping tracking (keyed by pingId). shared_ptr is required because
  // the coroutine (via its CancellationCallback) and handlePingResponse both
  // need the PendingPing to stay alive: ViaIfAsync can reschedule the
  // coroutine, so it may outlive handlePingResponse's return.
  folly::Synchronized<folly::F14FastMap<uint32_t, std::shared_ptr<PendingPing>>>
      pendingPings_;

  // In-flight co_sendPing coroutines; the destructor waits for all of them to
  // complete before tearing down so a running ping never touches a freed one.
  std::mutex outstandingMutex_;
  std::condition_variable outstandingCv_;
  size_t outstandingPings_{0}; // guarded by outstandingMutex_

  // Monotonic counter for ping id generation
  std::atomic<uint16_t> pingIdCounter_{0};

  const std::atomic<uint16_t>* activePortSet_{nullptr};

  [[nodiscard]] uint16_t loadActivePortSet() const {
    return activePortSet_ ? activePortSet_->load(std::memory_order_relaxed) : 0;
  }
};

} // namespace libsoftmetaroce
