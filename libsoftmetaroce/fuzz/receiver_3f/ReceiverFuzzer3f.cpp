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

//
// Fuzzing harness for Receiver::onDataAvailable — the network receive entry
// point for the MetaRoCE transport protocol. Exercises all packet processing
// branches (SEND, WRITE, READ, ATOMIC, ACK, RNR_CANCEL, CONTROL/PING) with
// fuzz-controlled header fields to reach the MACA-confirmed use-after-free
// path at Receiver.cpp:416-417 (tag->flushAllPendingWqesInError).

// =============================================================
// Harness Intention:
//   Fuzz Receiver::onDataAvailable to exercise all packet processing
//   branches and reach the MACA-confirmed UAF at Receiver.cpp:416-417.
//
// Improvements:
//   - Bias routing fields (dstQp, flowId, tag) toward valid values
//     to reach deeper code paths (processDataPacket, processAckPacket,
//     flushAllPendingWqesInError).
//   - Post a receive WQE so data processing can proceed past tag lookup.
//   - Register a memory region so WRITE/READ/ATOMIC can sometimes pass
//     validation and reach deeper code (createVirtualRWqeIfNeeded,
//     createVirtualSendWqeIfNeeded, createVirtualAtomicExecWqeIfNeeded,
//     extendCdmsnTrackerIfNeeded, processDataPacket).
//   - Add all opcode types: SEND_IMM_P, WRITE_IMM, WRITE_IMM_P,
//     ATOMIC_CMP_SWP, ATOMIC_RESERVED_0/1.
//   - Post multiple receive WQEs for deeper data processing.
// =============================================================

#include <folly/Synchronized.h>
#include <folly/container/F14Map.h>
#include <folly/io/async/EventBase.h>
#include <gmock/gmock-actions.h>

#include <libsoftmetaroce/CompletionQueue.h>
#include <libsoftmetaroce/Constants.h>
#include <libsoftmetaroce/IngressPacket.h>
#include <libsoftmetaroce/PacketModifierSocket.h>
#include <libsoftmetaroce/PingController.h>
#include <libsoftmetaroce/Qp.h>
#include <libsoftmetaroce/Receiver.h>
#include <libsoftmetaroce/Wqe.h>
#include <libsoftmetaroce/codec/AckHeader.h>
#include <libsoftmetaroce/codec/AtomicHeader.h>
#include <libsoftmetaroce/codec/BaseHeader.h>
#include <libsoftmetaroce/codec/ControlHeader.h>
#include <libsoftmetaroce/codec/DataHeader.h>
#include <libsoftmetaroce/codec/Packet.h>
#include <libsoftmetaroce/codec/PingHeader.h>
#include <libsoftmetaroce/codec/ReadWriteHeader.h>
#include <libsoftmetaroce/codec/RnrCancelHeader.h>
#include <libsoftmetaroce/test/LimitedTestSender.h>
#include <libsoftmetaroce/test/MockUDPSocket.h>
#include <libsoftmetaroce/test/PostToTag.h>

#include "security/lionhead/utils/lib_ftest/ftest.h"

using namespace testing;
using namespace facebook::security::lionhead::fdp;

namespace {

constexpr uint16_t kFuzzBindPort = 0xace;
constexpr uint16_t kFuzzSendPort = 0xace;
constexpr uint32_t kQpNum = 456;
// A packet whose source QP is not the connected peer's is discarded, so an ack
// reaches the transmit state only while carrying this value.
constexpr uint32_t kRemoteQpNum = 789;
// Memory region buffer and registered rkey for WRITE/READ/ATOMIC validation.
constexpr uint32_t kMrKey = 1;
constexpr size_t kMrBufSize = 8192;

// Socket maker that returns a NiceMock<MockUDPSocket>.
std::pair<
    std::unique_ptr<folly::AsyncUDPSocket>,
    std::unique_ptr<libsoftmetaroce::Modifier>>
mockSocketMaker(folly::EventBase* evb) {
  return std::make_pair(
      std::make_unique<NiceMock<libsoftmetaroce::MockUDPSocket>>(evb), nullptr);
}

// Wrapper to access the private onDataAvailable via the ReadCallback base.
class FuzzReceiver {
 public:
  FuzzReceiver(
      folly::IPAddress localAddr,
      folly::Synchronized<
          folly::F14FastMap<uint32_t, std::unique_ptr<libsoftmetaroce::Qp>>>&
          qpMap,
      NiceMock<libsoftmetaroce::MockUDPSocket>* mockSock,
      libsoftmetaroce::PingController& pingCtrl) {
    auto socketMaker = [mockSock](folly::EventBase*) {
      return std::make_pair(
          std::unique_ptr<folly::AsyncUDPSocket>(mockSock), nullptr);
    };
    receiver_ = std::make_unique<libsoftmetaroce::Receiver>(
        localAddr, qpMap, socketMaker, kFuzzBindPort, kFuzzSendPort, pingCtrl);
  }

  void injectPacket(std::unique_ptr<libsoftmetaroce::Packet> pkt, uint8_t tos) {
    folly::SocketAddress srcAddr("2001:db8::1", 12345);
    auto ingress = std::make_unique<libsoftmetaroce::IngressPacket>(
        libsoftmetaroce::IngressPacket{std::move(pkt), srcAddr, tos});
    auto* cb =
        static_cast<libsoftmetaroce::PacketModifierSocket::ReadCallback*>(
            receiver_.get());
    // Drive onDataAvailable on the reader's EventBase thread, as production
    // does, so cold error replies (NAK/HRNR/ping) write on the owning thread.
    receiver_->readerEventBaseForTest().runInEventBaseThreadAndWait(
        [&] { cb->onDataAvailable(std::move(ingress)); });
  }

 private:
  std::unique_ptr<libsoftmetaroce::Receiver> receiver_;
};

// Build a BaseHeader with the MetaRoCE BTH prefix and fuzz-controlled fields.
libsoftmetaroce::BaseHeader buildBaseHeader(
    libsoftmetaroce::BaseHeader::Opcode::OperationType opType,
    uint32_t srcQp,
    uint32_t dstQp,
    uint16_t flowId) {
  libsoftmetaroce::BaseHeader::Opcode opCode(opType);
  return libsoftmetaroce::BaseHeader::BaseHeaderBuilder()
      .setSrcQp(srcQp)
      .setDstQp(dstQp)
      .setFlowId(flowId)
      .setOpcode(std::move(opCode))
      .setVersion(0)
      .build();
}

// Build a DataHeader with fuzz-controlled fields.
libsoftmetaroce::DataHeader
buildDataHeader(uint16_t fsn, uint16_t posn, uint16_t msn, uint8_t tag) {
  return libsoftmetaroce::DataHeader::DataHeaderBuilder()
      .setFsn(fsn)
      .setUna(fsn)
      .setPosn(posn)
      .setMsn(msn)
      .setTag(tag)
      .setTimestamp(0)
      .setAckRequest(true)
      .build();
}

} // namespace

// Per-iteration fixture: sets up a Receiver with a QP in RTS state.
// NOLINTNEXTLINE(facebook-hte-ShadowingClass)
class ReceiverFuzzer3f : public ::testing::Test {
 protected:
  void SetUp() override {
    evb_ = std::make_unique<folly::EventBase>();
    mockSocket_ = new NiceMock<libsoftmetaroce::MockUDPSocket>(evb_.get());

    retxTimer_ = std::make_shared<libsoftmetaroce::HighResTimer>();
    arbiter_ = std::make_shared<libsoftmetaroce::QpArbiter>();
    cq_ = std::make_shared<libsoftmetaroce::CompletionQueue>();

    // Register a memory region so WRITE/READ/ATOMIC can pass validation
    // when the fuzzer supplies the correct rkey and an in-range address.
    mrPtr_ = mrs_.reg_mr(
        nullptr,
        mrBuf_,
        kMrBufSize,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
            IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_ATOMIC);

    fuzzReceiver_ = std::make_unique<FuzzReceiver>(
        folly::IPAddress("fc00::1"), qpMap_, mockSocket_, *pingCtrl_);
  }

  // Builds the QP the fuzzed packet is addressed to, in RTS state with receive
  // WQEs posted. A QP allocates its handoff ring at construction, so which of
  // the two ack paths it takes is decided here and not afterwards: with the
  // ring the receiver enqueues for the sender to apply at its arbiter visit,
  // and without it the receiver applies the ack inline.
  void createQp(bool singleOwnerTx) {
    FLAGS_libsoftmetaroce_single_owner_tx = singleOwnerTx;
    auto qp = std::make_unique<libsoftmetaroce::Qp>(
        kQpNum, *retxTimer_, *arbiter_, mrs_, cq_.get(), &ibvQpStorage_);
    qp->setIbvQpState(IBV_QPS_INIT);
    qp->setMtu(4096);
    qp->setRemoteQpNum(kRemoteQpNum);
    qp->addPortPair(folly::IPAddress("fc00::1"), folly::IPAddress("fc00::101"));

    libsoftmetaroce::LimitedTestSender sender(*arbiter_, mockSocketMaker);
    sender.setLimitNumSockets(2);
    qp->setupFlows(1, 1, &sender);
    qp->setIbvQpState(IBV_QPS_RTR);
    qp->setIbvQpState(IBV_QPS_RTS);

    qpMap_.wlock()->emplace(kQpNum, std::move(qp));

    // Post multiple receive WQEs so data processing can proceed past tag
    // lookup for several iterations.
    auto lockedMap = qpMap_.rlock();
    auto* qpPtr = lockedMap->at(kQpNum).get();
    auto tag = qpPtr->getTag(libsoftmetaroce::kMetaRoceInitialTag);
    for (int i = 0; i < 4; ++i) {
      auto rwqe = std::make_unique<libsoftmetaroce::ReceiveWqe>(
          /*workReqId=*/42 + i, recvBuf_ + i * 2048, 2048, qpPtr->getMtu());
      libsoftmetaroce::postToTag(*tag, std::move(rwqe), qpPtr->getMtu());
    }
  }

  void TearDown() override {
    qpMap_.wlock()->clear();
    fuzzReceiver_.reset();
    retxTimer_.reset();
    arbiter_.reset();
    cq_.reset();
    evb_.reset();
  }

  folly::Synchronized<
      folly::F14FastMap<uint32_t, std::unique_ptr<libsoftmetaroce::Qp>>>
      qpMap_;
  NiceMock<libsoftmetaroce::MockUDPSocket>* mockSocket_ = nullptr;
  std::unique_ptr<folly::EventBase> evb_;
  std::unique_ptr<libsoftmetaroce::PingController> pingCtrl_{
      std::make_unique<libsoftmetaroce::PingController>(
          mockSocketMaker,
          kFuzzSendPort)};
  std::unique_ptr<FuzzReceiver> fuzzReceiver_;

  std::shared_ptr<libsoftmetaroce::HighResTimer> retxTimer_;
  std::shared_ptr<libsoftmetaroce::QpArbiter> arbiter_;
  std::shared_ptr<libsoftmetaroce::CompletionQueue> cq_;
  libsoftmetaroce::MemoryRegions mrs_;
  struct ibv_qp ibvQpStorage_{};
  struct ibv_mr* mrPtr_ = nullptr;
  char mrBuf_[kMrBufSize] = {};
  char recvBuf_[8192] = {};
};

// Fuzz all packet types through onDataAvailable.
FUZZ_F(ReceiverFuzzer3f, FuzzOnDataAvailable) {
  using OpType = libsoftmetaroce::BaseHeader::Opcode::OperationType;

  createQp(f.coin("singleOwnerTx"));

  // Bias routing fields toward valid values to reach deeper code paths.
  auto srcQp = f.coin("useSrcQp") ? kRemoteQpNum : f.u32("srcQp");
  auto dstQp = f.coin("useDstQp") ? kQpNum : f.u32("dstQp");
  auto flowId =
      f.coin("useFlowId") ? static_cast<uint16_t>(0) : f.u16("flowId");
  auto fsn = f.u16("fsn");
  auto posn = f.u16("posn");
  auto msn = f.u16("msn");
  auto tag = f.coin("useTag")
      ? static_cast<uint8_t>(libsoftmetaroce::kMetaRoceInitialTag)
      : f.u8("tag");
  auto tos = f.u8("tos");

  // Bias remoteAddr and rkey toward valid memory region values so that
  // WRITE/READ/ATOMIC can sometimes pass validation and reach deeper
  // code paths (createVirtualRWqeIfNeeded, createVirtualSendWqeIfNeeded,
  // createVirtualAtomicExecWqeIfNeeded, extendCdmsnTrackerIfNeeded).
  uint64_t remoteAddr;
  uint32_t rkey;
  if (f.coin("useValidMr")) {
    // Point into the registered memory region.
    remoteAddr = reinterpret_cast<uint64_t>(mrBuf_) +
        f.u32_range("mrOffset", 0, kMrBufSize - 8);
    rkey = kMrKey;
  } else {
    remoteAddr = f.u64("remoteAddr");
    rkey = f.u32("rkey");
  }

  // Choose which packet type to construct (expanded to cover all opcodes).
  auto packetType = f.u8_range("packetType", 0, 11);

  std::unique_ptr<libsoftmetaroce::Packet> packet;
  // A packet borrows its payload pointer, so the buffer has to outlive the
  // injection below rather than the case arm that fills it.
  std::vector<uint8_t> payload;

  switch (packetType) {
    case 0: {
      // SEND data packet
      auto baseHdr = buildBaseHeader(OpType::SEND, srcQp, dstQp, flowId);
      auto dataHdr = buildDataHeader(fsn, posn, msn, tag);
      payload = f.bytes("payload");
      packet = std::make_unique<libsoftmetaroce::DataPacket>(
          baseHdr,
          dataHdr,
          payload.data(),
          static_cast<uint32_t>(payload.size()));
      break;
    }
    case 1: {
      // SEND_IMM_P — exercises the SEND_IMM_P branch in Receiver.cpp:311-314
      auto baseHdr = buildBaseHeader(OpType::SEND_IMM_P, srcQp, dstQp, flowId);
      auto dataHdr = buildDataHeader(fsn, posn, msn, tag);
      payload = f.bytes("sendImmPayload");
      auto immData = f.u32("sendImmData");
      packet = std::make_unique<libsoftmetaroce::DataPacket>(
          baseHdr,
          dataHdr,
          payload.data(),
          static_cast<uint32_t>(payload.size()),
          immData);
      break;
    }
    case 2: {
      // WRITE packet — remoteAddress and rkey are fuzz-controlled
      // to trigger the address-error validation path.
      auto length = f.u32("writeLength");
      auto baseHdr = buildBaseHeader(OpType::WRITE, srcQp, dstQp, flowId);
      auto dataHdr = buildDataHeader(fsn, posn, msn, tag);
      libsoftmetaroce::ReadWriteHeader rwHdr(remoteAddr, rkey, length);
      payload = f.bytes("writePayload");
      packet = std::make_unique<libsoftmetaroce::ReadWritePacket>(
          baseHdr,
          dataHdr,
          rwHdr,
          payload.data(),
          static_cast<uint32_t>(payload.size()));
      break;
    }
    case 3: {
      // WRITE_IMM — exercises Receiver.cpp:317-331
      auto length = f.u32("writeImmLength");
      auto baseHdr = buildBaseHeader(OpType::WRITE_IMM, srcQp, dstQp, flowId);
      auto dataHdr = buildDataHeader(fsn, posn, msn, tag);
      libsoftmetaroce::ReadWriteHeader rwHdr(remoteAddr, rkey, length);
      payload = f.bytes("writeImmPayload");
      packet = std::make_unique<libsoftmetaroce::ReadWritePacket>(
          baseHdr,
          dataHdr,
          rwHdr,
          payload.data(),
          static_cast<uint32_t>(payload.size()));
      break;
    }
    case 4: {
      // WRITE_IMM_P — exercises Receiver.cpp:317-331
      auto length = f.u32("writeImmPLength");
      auto baseHdr = buildBaseHeader(OpType::WRITE_IMM_P, srcQp, dstQp, flowId);
      auto dataHdr = buildDataHeader(fsn, posn, msn, tag);
      libsoftmetaroce::ReadWriteHeader rwHdr(remoteAddr, rkey, length);
      payload = f.bytes("writeImmPPayload");
      auto immData = f.u32("writeImmPData");
      packet = std::make_unique<libsoftmetaroce::ReadWritePacket>(
          baseHdr,
          dataHdr,
          rwHdr,
          payload.data(),
          static_cast<uint32_t>(payload.size()),
          immData);
      break;
    }
    case 5: {
      // READ packet — exercises the read validation path.
      auto length = f.u32("readLength");
      auto baseHdr = buildBaseHeader(OpType::READ, srcQp, dstQp, flowId);
      auto dataHdr = buildDataHeader(fsn, posn, msn, tag);
      libsoftmetaroce::ReadWriteHeader rwHdr(remoteAddr, rkey, length);
      packet = std::make_unique<libsoftmetaroce::ReadWritePacket>(
          baseHdr, dataHdr, rwHdr, nullptr, 0);
      break;
    }
    case 6: {
      // ATOMIC_FETCH_ADD packet
      auto swapOrAdd = f.u64("swapOrAdd");
      auto compareData = f.u64("compareData");
      auto baseHdr =
          buildBaseHeader(OpType::ATOMIC_FETCH_ADD, srcQp, dstQp, flowId);
      auto dataHdr = buildDataHeader(fsn, posn, msn, tag);
      libsoftmetaroce::AtomicHeader atomicHdr(
          remoteAddr, rkey, swapOrAdd, compareData);
      packet = std::make_unique<libsoftmetaroce::AtomicPacket>(
          baseHdr, dataHdr, atomicHdr);
      break;
    }
    case 7: {
      // ATOMIC_CMP_SWP — exercises Receiver.cpp:349-350
      auto swapOrAdd = f.u64("cmpSwapData");
      auto compareData = f.u64("cmpCompareData");
      auto baseHdr =
          buildBaseHeader(OpType::ATOMIC_CMP_SWP, srcQp, dstQp, flowId);
      auto dataHdr = buildDataHeader(fsn, posn, msn, tag);
      libsoftmetaroce::AtomicHeader atomicHdr(
          remoteAddr, rkey, swapOrAdd, compareData);
      packet = std::make_unique<libsoftmetaroce::AtomicPacket>(
          baseHdr, dataHdr, atomicHdr);
      break;
    }
    case 8: {
      // ACK packet — vary AckStatus to exercise different TxFlow paths.
      auto baseHdr = buildBaseHeader(OpType::SEL_ACK, srcQp, dstQp, flowId);
      // Select from the main AckStatus values to exercise different
      // processing branches in TxFlow::processAckPacket.
      libsoftmetaroce::AckStatus ackStatus;
      switch (f.u8_range("ackStatusType", 0, 2)) {
        case 0:
          ackStatus = libsoftmetaroce::AckStatus::ACK;
          break;
        case 1:
          ackStatus = libsoftmetaroce::AckStatus::ACK_OUT_OF_ORDER;
          break;
        default:
          ackStatus = libsoftmetaroce::AckStatus::NAK_INVALID_REQUEST;
          break;
      }
      auto ackHdr = libsoftmetaroce::AckHeader::AckHeaderBuilder()
                        .setTag(tag)
                        .setCumulativeFsn(fsn)
                        .setMaxRcvdFsn(f.u16("maxRcvdFsn"))
                        .setTimestamp(f.u32("ackTimestamp"))
                        .setAckStatus(ackStatus)
                        .setCumulRespDeliveredMsn(msn)
                        .build();
      packet = std::make_unique<libsoftmetaroce::AckPacket>(baseHdr, ackHdr);
      break;
    }
    case 9: {
      // RNR_CANCEL packet
      auto baseHdr = buildBaseHeader(OpType::RNR_CANCEL, srcQp, dstQp, flowId);
      libsoftmetaroce::RnrCancelHeader rnrHdr(
          f.u8("flowsAvailable"), tag, f.u16("postedCsn"), f.u32("rateHints"));
      packet =
          std::make_unique<libsoftmetaroce::RnrCancelPacket>(baseHdr, rnrHdr);
      break;
    }
    case 10: {
      // CONTROL/PING — exercises the handlePing path in Receiver.cpp:167-197
      auto baseHdr =
          buildBaseHeader(OpType::UNRELIABLE_CONTROL, srcQp, dstQp, flowId);
      auto ctrlHdr = libsoftmetaroce::ControlHeader(
          static_cast<uint16_t>(libsoftmetaroce::ControlPacketType::PING),
          0,
          f.u32("pingTimestamp"));
      libsoftmetaroce::PingHeader pingHdr(
          tag,
          static_cast<uint8_t>(libsoftmetaroce::PingErrorState::NO_ERROR),
          f.u16("supportedFlowCount"));
      packet = std::make_unique<libsoftmetaroce::PingPacket>(
          baseHdr, ctrlHdr, pingHdr, nullptr, 0);
      break;
    }
    case 11:
    default: {
      // ATOMIC_RESERVED_0 — exercises the unsupported opcode NAK path
      // at Receiver.cpp:255-261.
      auto baseHdr =
          buildBaseHeader(OpType::ATOMIC_RESERVED_0, srcQp, dstQp, flowId);
      auto dataHdr = buildDataHeader(fsn, posn, msn, tag);
      payload = f.bytes("reservedPayload");
      packet = std::make_unique<libsoftmetaroce::DataPacket>(
          baseHdr,
          dataHdr,
          payload.data(),
          static_cast<uint32_t>(payload.size()));
      break;
    }
  }

  fuzzReceiver_->injectPacket(std::move(packet), tos);

  // With the handoff ring the receiver only enqueues an ack, so draining here
  // applies it as the sender's arbiter visit would. A no-op without the ring.
  qpMap_.rlock()->at(kQpNum)->applyHandedOffAcks();
}
