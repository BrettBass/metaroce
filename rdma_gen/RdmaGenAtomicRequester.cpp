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

#include "rdma_gen/RdmaGenAtomicRequester.h"

#include <cerrno>

#include <folly/String.h>

namespace facebook::neteng::ai::rdma_gen {

RdmaGenAtomicRequester::RdmaGenAtomicRequester(
    const Params& params,
    const std::vector<int64_t>& numMessagesPerServer,
    bool equalMessagesPerServer,
    const std::vector<std::string>& rcReceivers,
    const std::vector<int>& rcCmPorts,
    bool logMessageLatencies,
    const std::vector<int>& rcCos,
    const std::vector<enum ibv_mtu>& mtus,
    const enum ibv_qp_state targetQPState,
    const std::string& jsonPath)
    : RdmaGenRequester(
          params,
          numMessagesPerServer,
          equalMessagesPerServer,
          rcReceivers,
          rcCmPorts,
          logMessageLatencies,
          rcCos,
          mtus,
          targetQPState,
          jsonPath),
      isFetchAdd_(params.role == EndpointRole::AtomicFetchAddRequester) {
  CHECK(
      params.role == EndpointRole::AtomicFetchAddRequester ||
      params.role == EndpointRole::AtomicCmpSwapRequester);
  remoteAddrs_ = std::make_shared<std::vector<void*>>();
  rkeys_ = std::make_shared<std::vector<uint32_t>>();

  cmClient_ = RdmaGenControlFactory::createControlClient(
      endpoint_, remoteRcQpInfo_, remoteAddrs_, rkeys_);
}

int RdmaGenAtomicRequester::postRequest(const RcBuffer& rcBuf) {
  const auto addr = (uintptr_t)endpoint_->getBuffer(rcBuf.token);
  statsRecorder_->recordStartTime(rcBuf.token, getCurrentTimestampUs());
  const auto key = params().use_block ? ctx()->mr_list[0]->lkey()
                                      : ctx()->mr_list[rcBuf.token]->lkey();

  const uint16_t seqNum = getNextSendSeqNum(rcBuf.qp_idx);
  // IBV_SEND_INLINE is only valid for SEND/WRITE; per IB spec it is undefined
  // for ATOMIC opcodes (no payload follows the headers). Strip it explicitly
  // because getSendFlags would otherwise set it whenever 8 <= max_inline_data.
  const uint32_t verbsFlags =
      getSendFlags(rcBuf.qp_idx, kAtomicOperandBytes) & ~IBV_SEND_INLINE;

  SendWorkRequestAdapter wr{
      .wr_id = createWrIdWithSeq(seqNum, rcBuf.qp_idx, rcBuf.token),
      .opcode = isFetchAdd_ ? SendOpcode::AtomicFetchAndAdd
                            : SendOpcode::AtomicCmpAndSwp,
      .send_flags = convertVerbsSendFlags(verbsFlags),
      .local_addr = addr,
      .length = kAtomicOperandBytes,
      .lkey = key,
  };
  if (isFetchAdd_) {
    wr.target.atomic = AtomicTarget{
        .remote_addr =
            reinterpret_cast<uint64_t>(remoteAddrs_->at(rcBuf.qp_idx)),
        .rkey = rkeys_->at(rcBuf.qp_idx),
        .compare_add = 1,
        .swap = 0,
    };
  } else {
    wr.target.atomic = AtomicTarget{
        .remote_addr =
            reinterpret_cast<uint64_t>(remoteAddrs_->at(rcBuf.qp_idx)),
        .rkey = rkeys_->at(rcBuf.qp_idx),
        .compare_add = seqNum,
        .swap = static_cast<uint64_t>(seqNum + 1),
    };
  }

  int postSendRet = ctx()->peer_list[rcBuf.qp_idx]->postSend(wr);

  LOG_IF(ERROR, postSendRet != 0) << fmt::format(
      "ibv_post_send failed for message {} on qp_idx {}: ret={} errno={} ({})",
      rcBuf.token,
      rcBuf.qp_idx,
      postSendRet,
      errno,
      folly::errnoStr(errno));
  return postSendRet;
}

} // namespace facebook::neteng::ai::rdma_gen
