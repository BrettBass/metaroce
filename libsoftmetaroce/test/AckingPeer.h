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

#include <folly/io/Cursor.h>
#include <folly/io/async/AsyncUDPSocket.h>
#include <folly/io/async/EventBase.h>
#include <folly/portability/GMock.h>
#include <libsoftmetaroce/Constants.h>
#include <libsoftmetaroce/Ecn.h>
#include <libsoftmetaroce/IngressPacket.h>
#include <libsoftmetaroce/PacketModifierSocket.h>
#include <libsoftmetaroce/Receiver.h>
#include <libsoftmetaroce/codec/AckHeader.h>
#include <libsoftmetaroce/codec/BaseHeader.h>
#include <libsoftmetaroce/codec/DataHeader.h>
#include <libsoftmetaroce/codec/Packet.h>
#include <libsoftmetaroce/modifier/Modifier.h>
#include <libsoftmetaroce/test/MockUDPSocket.h>
#include <atomic>
#include <functional>
#include <memory>
#include <utility>

namespace libsoftmetaroce {

// A peer for tests that drive the verbs send path: it answers every data packet
// written to one of its sockets with an ack, delivered to a receiver on the
// reader thread that a peer's ack would arrive on. A send whose packets are
// only discarded stays outstanding until the retry count runs out and completes
// in error seconds later, so without a peer that acks, a test cannot observe an
// ordinary completion or anything that follows one.
//
// The ack is what the data packet asks for: it gives the packet's own tag, fsn
// and msn, and echoes its timestamp, so a stream of them advances the sender
// the way an in-order receiver does. Every packet is answered that way, so a
// test that needs any other response builds it directly.
class AckingPeer {
 public:
  // The receiver the acks go to. A verbs object creates its receiver while
  // bringing up the first queue pair, so a test names it after that rather than
  // at construction, and packets written before then are discarded.
  void ackTo(Receiver& receiver) {
    receiver_.store(&receiver, std::memory_order_release);
  }

  // Stop answering writes, so later work cannot complete. An ack already queued
  // still runs, since a receiver drains its event base before stopping it.
  void stopAcking() {
    receiver_.store(nullptr, std::memory_order_release);
  }

  // A socket maker for MetaRoceVerbs, whose sockets ack what they are asked to
  // send. A verbs object builds its receiver's and its sender's sockets from
  // one maker; only data packets are acked, so the receiver's own replies pass
  // through as the plain mock leaves them.
  //
  // Only the per-packet write is answered. A sender coalescing a run into one
  // writeGSO or writem would go unacked here, which the egress modes that
  // batch would report as a send that no peer answered.
  [[nodiscard]] std::function<std::pair<
      std::unique_ptr<folly::AsyncUDPSocket>,
      std::unique_ptr<Modifier>>(folly::EventBase*)>
  socketMaker() {
    return [this](folly::EventBase* evb) {
      auto socket = std::make_unique<::testing::NiceMock<MockUDPSocket>>(evb);
      ON_CALL(*socket, write(::testing::_, ::testing::_))
          .WillByDefault([this](
                             const folly::SocketAddress& dest,
                             const std::unique_ptr<folly::IOBuf>& buf) {
            ackDatagram(dest, *buf);
            return static_cast<ssize_t>(buf->computeChainDataLength());
          });
      return std::make_pair(
          std::unique_ptr<folly::AsyncUDPSocket>(std::move(socket)),
          std::unique_ptr<Modifier>(nullptr));
    };
  }

  [[nodiscard]] int getAckCount() const {
    return acks_.load(std::memory_order_relaxed);
  }

 private:
  // Ack the datagram in `buf`, whose headers sit at the front of it as they
  // would at the front of a receiver's own read.
  void ackDatagram(
      const folly::SocketAddress& source,
      const folly::IOBuf& buf) {
    Receiver* receiver = receiver_.load(std::memory_order_acquire);
    if (receiver == nullptr) {
      return;
    }
    folly::io::Cursor cursor(&buf);
    const BaseHeader dataBaseHeader(cursor);
    if (!dataBaseHeader.getOpcode().isData()) {
      return;
    }
    const DataHeader dataHeader(cursor);

    auto ackPacket = std::make_unique<AckPacket>(
        BaseHeader::BaseHeaderBuilder()
            .setOpcode(
                BaseHeader::Opcode(BaseHeader::Opcode::OperationType::SEL_ACK))
            .setVersion(kMetaRoceProtocolVersion)
            .setFlowId(dataBaseHeader.getFlowId())
            .setDstQp(dataBaseHeader.getSrcQp())
            .setSrcQp(dataBaseHeader.getDstQp())
            .build(),
        AckHeader::AckHeaderBuilder()
            .setTag(dataHeader.getTag())
            .setCumulativeFsn(dataHeader.getFsn())
            .setMaxRcvdFsn(dataHeader.getFsn())
            .setAckStatus(AckStatus::ACK)
            .setCumulRespDeliveredMsn(dataHeader.getMsn())
            .setTimestamp(dataHeader.getTimestamp())
            .build());

    auto ingressPacket = std::make_unique<IngressPacket>(
        IngressPacket{std::move(ackPacket), source, kEcnECT0});
    acks_.fetch_add(1, std::memory_order_relaxed);
    // Onto the reader thread rather than inline: the ack reaches transmit state
    // the calling sender thread is in the middle of writing.
    receiver->readerEventBaseForTest().runInEventBaseThread(
        [receiver, packet = std::move(ingressPacket)]() mutable {
          static_cast<PacketModifierSocket::ReadCallback*>(receiver)
              ->onDataAvailable(std::move(packet));
        });
  }

  std::atomic<Receiver*> receiver_{nullptr};
  std::atomic<int> acks_{0};
};

} // namespace libsoftmetaroce
